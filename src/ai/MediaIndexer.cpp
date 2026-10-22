#include "MediaIndexer.h"

#include "engine/Speech.h"

#include <QBuffer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QThread>
#include <algorithm>

namespace mtg::ai {

namespace {

QString setting(const char *key, const char *env, const QString &fallback)
{
    if (const QString v = qEnvironmentVariable(env); !v.isEmpty()) return v;
    return QSettings().value(QStringLiteral("vision/") + QLatin1String(key), fallback).toString();
}

// Installed vision models, best first for labelling shots: no thinking
// (fast), then size.
int visionRank(const QString &name)
{
    const QString n = name.toLower();
    if (n.startsWith(QLatin1String("qwen3-vl")) && n.contains(QLatin1String("instruct"))) return 0;
    if (n.startsWith(QLatin1String("qwen2.5vl")) || n.startsWith(QLatin1String("qwen2.5-vl"))) return 1;
    if (n.startsWith(QLatin1String("qwen3-vl"))) return 2;  // thinks first: slower
    if (n.startsWith(QLatin1String("gemma3")) || n.contains(QLatin1String("minicpm-v")) || n.contains(QLatin1String("llava"))) return 3;
    if (n.contains(QLatin1String("vision")) || n.contains(QLatin1String("-vl")) || n.contains(QLatin1String("moondream"))) return 4;
    return -1;
}

}  // namespace

VisionConfig VisionConfig::load()
{
    VisionConfig c;
    c.ollamaUrl = setting("ollama_url", "MONTAGE_VISION_URL", QStringLiteral("http://127.0.0.1:11434"));
    c.model = setting("model", "MONTAGE_VISION_MODEL", {});
    c.enabled = setting("enabled", "MONTAGE_VISION", QStringLiteral("1")) != QLatin1String("0");
    return c;
}

QJsonObject shotRequestBody(const QString &model, const QImage &frame)
{
    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    frame.save(&buf, "JPEG", 85);
    const QJsonObject schema{
        {"type", "object"},
        {"properties", QJsonObject{{"description", QJsonObject{{"type", "string"}}},
                                   {"subjects", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}}},
                                   {"shot", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"wide", "medium", "close-up", "detail"}}}},
                                   {"appeal", QJsonObject{{"type", "integer"}}}}},
        {"required", QJsonArray{"description", "subjects", "shot", "appeal"}}};
    return {{"model", model},
            {"stream", false},
            {"think", false},
            {"keep_alive", "10m"},
            {"format", schema},
            {"options", QJsonObject{{"temperature", 0}, {"num_ctx", 4096}, {"num_predict", 160}}},
            {"messages", QJsonArray{QJsonObject{
                             {"role", "user"},
                             {"content", "You label shots for a video editor. Describe what this frame shows in at most 12 plain "
                                         "words (people, objects, place, action; not the style), list up to 3 main subjects, give "
                                         "the shot type, and rate 1-5 how good it would look in an ad (sharp, well lit, clear "
                                         "subject). Answer as JSON."},
                             {"images", QJsonArray{QString::fromLatin1(jpeg.toBase64())}}}}}};
}

bool readShotReply(const QByteArray &body, Shot *out)
{
    const QJsonObject reply = QJsonDocument::fromJson(body).object();
    const QJsonObject o = QJsonDocument::fromJson(reply.value("message").toObject().value("content").toString().toUtf8()).object();
    const QString description = o.value("description").toString().simplified();
    if (description.isEmpty()) return false;
    out->description = description.left(120);
    out->framing = o.value("shot").toString();
    out->subjects.clear();
    for (const QJsonValue &v : o.value("subjects").toArray())
        if (out->subjects.size() < 3 && !v.toString().trimmed().isEmpty()) out->subjects << v.toString().trimmed().toLower();
    out->appeal = std::clamp(o.value("appeal").toInt(3), 1, 5);
    return true;
}

MediaIndexer::MediaIndexer(QObject *parent)
    : QObject(parent)
    , m_config(VisionConfig::load())
    , m_net(new QNetworkAccessManager(this))
{
}

void MediaIndexer::watch(const Draft &d)
{
    bool added = false;
    for (const Material &m : d.materials) {
        if ((m.kind != MaterialKind::Video && m.kind != MaterialKind::Image && m.kind != MaterialKind::Audio) || m.path.isEmpty()) continue;
        if (m_seen.contains(m.path) || (m_current >= 0 && m_job.material.path == m.path)) continue;
        if (std::any_of(m_queue.cbegin(), m_queue.cend(), [&](const Job &j) { return j.material.path == m.path; })) continue;
        const bool needsWords = wantsTranscript(m) && !cachedFileTranscript(m).done;
        const SceneIndex cached = m.kind == MaterialKind::Audio ? SceneIndex{} : cachedSceneIndex(m);
        const bool scenesDone = m.kind == MaterialKind::Audio || (!cached.isEmpty() && (cached.described || !m_config.enabled));
        if (scenesDone && !needsWords) {
            m_seen.insert(m.path, true);
            continue;
        }
        m_queue.append(Job{m, {}, {}});
        ++m_total;
        added = true;
    }
    if (added) {
        emit statusChanged();
        next();
    }
}

void MediaIndexer::setPaused(bool paused)
{
    if (m_paused == paused) return;
    m_paused = paused;
    if (paused) return;
    if (m_current == 1 && !m_reply) describeNext();
    else next();
}

QString MediaIndexer::status() const
{
    if (m_current < 0 && m_queue.isEmpty()) return {};
    const QString which = QStringLiteral("%1 of %2").arg(std::min(m_done + 1, m_total)).arg(m_total);
    if (m_current == 2) return QStringLiteral("Listening to your clips (%1)").arg(which);
    return m_current == 1 ? QStringLiteral("Watching your clips (%1): describing shots").arg(which)
                          : QStringLiteral("Watching your clips (%1)").arg(which);
}

void MediaIndexer::next()
{
    if (m_paused || m_current >= 0) return;
    if (m_queue.isEmpty()) {
        m_done = m_total = 0;
        emit statusChanged();
        return;
    }
    m_job = m_queue.takeFirst();
    m_current = 0;
    emit statusChanged();
    if (m_job.material.kind == MaterialKind::Audio) {  // nothing to look at: just listen
        m_current = 2;
        emit statusChanged();
        transcribe([this] {
            m_seen.insert(m_job.material.path, true);
            ++m_done;
            m_current = -1;
            emit statusChanged();
            next();
        });
        return;
    }
    auto *result = new SceneIndex;
    const Material material = m_job.material;
    QThread *worker = QThread::create([result, material] { *result = sceneIndex(material); });
    connect(worker, &QThread::finished, this, [this, worker, result] {
        m_job.index = *result;
        delete result;
        worker->deleteLater();
        emit indexed(m_job.material.path);
        // Describe the longest shots first; very short ones only if that's all there is.
        for (qsizetype i = 0; i < m_job.index.shots.size(); ++i)
            if (m_job.index.shots[i].description.isEmpty()) m_job.order << int(i);
        std::sort(m_job.order.begin(), m_job.order.end(),
                  [&](int a, int b) { return m_job.index.shots[a].length() > m_job.index.shots[b].length(); });
        if (m_job.order.size() > 1)
            m_job.order.removeIf([&](int i) { return m_job.index.shots[i].length() > 0 && m_job.index.shots[i].length() < 800'000; });
        while (m_job.order.size() > m_config.maxShotsPerFile) m_job.order.removeLast();
        if (!m_config.enabled || m_job.order.isEmpty()) {
            m_current = 2;
            emit statusChanged();
            transcribe([this] {
                m_seen.insert(m_job.material.path, true);
                ++m_done;
                m_current = -1;
                emit statusChanged();
                next();
            });
            return;
        }
        m_current = 1;
        emit statusChanged();
        pickModel([this] { describeNext(); });
    });
    worker->start();
}

bool MediaIndexer::wantsTranscript(const Material &m)
{
    // Long files would take minutes; whisper must be installed.
    return m.hasAudio && m.durationUs > 0 && m.durationUs < qint64(45) * 60 * 1'000'000 && whisperProblem().isEmpty()
           && !qEnvironmentVariableIsSet("MONTAGE_NO_TRANSCRIBE");
}

void MediaIndexer::transcribe(const std::function<void()> &then)
{
    if (!wantsTranscript(m_job.material) || cachedFileTranscript(m_job.material).done) {
        then();
        return;
    }
    const Material material = m_job.material;
    QThread *worker = QThread::create([material] { fileTranscript(material); });
    connect(worker, &QThread::finished, this, [this, worker, then] {
        worker->deleteLater();
        emit indexed(m_job.material.path);
        then();
    });
    worker->start();
}

void MediaIndexer::pickModel(const std::function<void()> &then)
{
    if (m_modelChecked) {
        then();
        return;
    }
    QNetworkRequest req(QUrl(m_config.ollamaUrl + QStringLiteral("/api/tags")));
    req.setTransferTimeout(5000);
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, then] {
        reply->deleteLater();
        m_modelChecked = true;
        QStringList installed;
        for (const QJsonValue &v : QJsonDocument::fromJson(reply->readAll()).object().value("models").toArray())
            installed << v.toObject().value("name").toString();
        if (!m_config.model.isEmpty()) {
            m_model = installed.contains(m_config.model) || installed.contains(m_config.model + QStringLiteral(":latest")) ? m_config.model : QString();
        } else {
            int best = 99;
            for (const QString &name : installed)
                if (const int r = visionRank(name); r >= 0 && r < best) best = r, m_model = name;
        }
        if (m_model.isEmpty() && qEnvironmentVariableIsSet("MONTAGE_DEBUG"))
            qInfo("media indexer: no vision model installed; shots get on-device labels only");
        then();
    });
}

void MediaIndexer::describeNext()
{
    if (m_paused || m_reply) return;
    auto finishJob = [this] {
        setShotDescriptions(m_job.material, m_job.index.shots);
        emit indexed(m_job.material.path);
        m_current = 2;
        emit statusChanged();
        transcribe([this] {
            m_seen.insert(m_job.material.path, true);
            ++m_done;
            m_current = -1;
            emit statusChanged();
            next();
        });
    };
    if (m_model.isEmpty() || m_job.order.isEmpty()) {
        finishJob();
        return;
    }
    const int i = m_job.order.takeFirst();
    const QImage frame = shotFrame(m_job.material, m_job.index.shots[i], 448);
    if (frame.isNull()) {
        describeNext();
        return;
    }
    QNetworkRequest req(QUrl(m_config.ollamaUrl + QStringLiteral("/api/chat")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setTransferTimeout(180'000);
    m_reply = m_net->post(req, QJsonDocument(shotRequestBody(m_model, frame)).toJson(QJsonDocument::Compact));
    connect(m_reply, &QNetworkReply::finished, this, [this, reply = m_reply.data(), i, finishJob] {
        reply->deleteLater();
        m_reply = nullptr;
        const bool ok = reply->error() == QNetworkReply::NoError && readShotReply(reply->readAll(), &m_job.index.shots[i]);
        if (!ok) {
            if (qEnvironmentVariableIsSet("MONTAGE_DEBUG")) qInfo("media indexer: %s", qPrintable(reply->errorString()));
            m_job.order.clear();  // the model isn't answering: keep what we have
        } else {
            setShotDescriptions(m_job.material, m_job.index.shots);  // usable shot by shot
            emit indexed(m_job.material.path);
        }
        if (m_job.order.isEmpty()) finishJob();
        else describeNext();
    });
}

}  // namespace mtg::ai
