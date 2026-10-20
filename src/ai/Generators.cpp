#include "Generators.h"

#include "EditPlan.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSize>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>
#include <QWebSocket>
#include <cmath>

namespace mtg::ai {

namespace {

QString expandHome(QString path)
{
    path = path.trimmed();
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/"))) path.replace(0, 1, QDir::homePath());
    return QDir::cleanPath(path);
}

// "Cozy candle-lit shop, warm light!" -> "cozy-candle-lit-shop-warm-light"
QString slug(const QString &text, int maxLen = 40)
{
    QString s = text.toLower();
    s.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    s.remove(QRegularExpression(QStringLiteral("^-+|-+$")));
    if (s.size() > maxLen) {
        s.truncate(maxLen);
        const qsizetype cut = s.lastIndexOf(QLatin1Char('-'));
        if (cut > maxLen / 2) s.truncate(cut);
    }
    return s.isEmpty() ? QStringLiteral("untitled") : s;
}

QString shorten(const QString &text, int maxLen = 48)
{
    const QString t = text.simplified();
    return t.size() <= maxLen ? t : t.left(maxLen - 1).trimmed() + QChar(0x2026);
}

// SDXL's trained sizes closest to each shape.
QSize imageSize(const QString &aspect)
{
    static const QHash<QString, QSize> sizes{{"16:9", {1344, 768}}, {"9:16", {768, 1344}}, {"1:1", {1024, 1024}},
                                             {"4:5", {896, 1152}},  {"3:4", {896, 1152}},  {"4:3", {1152, 896}},
                                             {"21:9", {1536, 640}}};
    return sizes.value(aspect, QSize(1344, 768));
}

// "16:9" -> 1.777...; 0 if it isn't a shape.
double shapeRatio(const QString &aspect)
{
    const double w = aspect.section(QLatin1Char(':'), 0, 0).toDouble(), h = aspect.section(QLatin1Char(':'), 1, 1).toDouble();
    return w > 0 && h > 0 ? w / h : 0;
}

// Copies a helper script from the resources to a real file the Python
// interpreters can run. Rewritten only when the app's copy changed.
QString helperScript(const QString &name)
{
    QFile res(QStringLiteral(":/generate/") + name);
    if (!res.open(QIODevice::ReadOnly)) return {};
    const QByteArray code = res.readAll();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/generate");
    QDir().mkpath(dir);
    const QString path = dir + QLatin1Char('/') + name;
    QFile out(path);
    if (out.open(QIODevice::ReadOnly) && out.readAll() == code) return path;
    out.close();
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return {};
    out.write(code);
    return path;
}

QString firstExisting(const QStringList &candidates)
{
    for (const QString &c : candidates)
        if (QFileInfo::exists(c)) return c;
    return candidates.value(0);
}

// ---------------------------------------------------------------- ComfyUI

// Text to image through ComfyUI's HTTP API: POST /prompt with a workflow
// built here, step progress from its websocket, the result from /history
// and the PNG from /view.
class ComfyImageBackend : public GeneratorBackend
{
public:
    ComfyImageBackend(const GeneratorConfig &config, QObject *parent)
        : GeneratorBackend(parent)
        , m_config(config)
        , m_net(new QNetworkAccessManager(this))
        , m_ws(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    {
        m_poll.setInterval(700);
        connect(&m_poll, &QTimer::timeout, this, [this] { poll(); });
        connect(m_ws, &QWebSocket::textMessageReceived, this, [this](const QString &msg) { onSocket(msg); });
    }

    QString unavailable() const override
    {
        QNetworkAccessManager net;
        QNetworkRequest req(QUrl(m_config.comfyUrl + QStringLiteral("/system_stats")));
        req.setTransferTimeout(2000);
        QNetworkReply *reply = net.get(req);
        QEventLoop loop;
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        return ok ? QString() : notRunning();
    }

    void generate(const GenRequest &request, const QString &outPath) override
    {
        m_out = outPath;
        m_aspect = request.aspect;
        m_promptId.clear();
        m_active = true;
        m_pollErrors = 0;
        m_started.start();
        const QString clientId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QUrl ws(m_config.comfyUrl + QStringLiteral("/ws"));
        ws.setScheme(ws.scheme() == QLatin1String("https") ? QStringLiteral("wss") : QStringLiteral("ws"));
        ws.setQuery(QStringLiteral("clientId=") + clientId);
        m_ws->open(ws);  // progress only; completion comes from /history

        emit progress(0.02, QStringLiteral("Sending to ComfyUI"));
        QNetworkRequest req(QUrl(m_config.comfyUrl + QStringLiteral("/prompt")));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setTransferTimeout(15000);
        const QJsonObject body{{"prompt", workflow(request)}, {"client_id", clientId}};
        QNetworkReply *reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            const QJsonObject o = QJsonDocument::fromJson(reply->readAll()).object();
            if (!m_active) {  // cancelled before ComfyUI answered: take the job back off its queue
                if (const QString id = o.value("prompt_id").toString(); !id.isEmpty()) {
                    post(QStringLiteral("/interrupt"), QJsonObject{{"prompt_id", id}});
                    post(QStringLiteral("/queue"), QJsonObject{{"delete", QJsonArray{id}}});
                }
                return;
            }
            if (reply->error() == QNetworkReply::ConnectionRefusedError || reply->error() == QNetworkReply::HostNotFoundError
                || reply->error() == QNetworkReply::TimeoutError) {
                finish({false, false, {}, notRunning()});
                return;
            }
            if (reply->error() != QNetworkReply::NoError || !o.contains("prompt_id")) {
                finish({false, false, {}, QStringLiteral("ComfyUI rejected the image workflow: ") + describeError(o, reply->errorString())});
                return;
            }
            m_promptId = o.value("prompt_id").toString();
            emit progress(0.05, QStringLiteral("Loading SDXL"));
            m_poll.start();
        });
    }

    void cancel() override
    {
        if (!m_active) return;
        if (!m_promptId.isEmpty()) {
            post(QStringLiteral("/interrupt"), QJsonObject{{"prompt_id", m_promptId}});
            post(QStringLiteral("/queue"), QJsonObject{{"delete", QJsonArray{m_promptId}}});
        }
        finish({false, true, {}, QStringLiteral("Cancelled")});
    }

private:
    QString notRunning() const
    {
        return QStringLiteral("ComfyUI isn't running at %1, so images can't be made. Start it in Terminal with\n"
                              "  cd %2 && .venv/bin/python main.py --listen 127.0.0.1 --port %3\n"
                              "and ask again.")
            .arg(m_config.comfyUrl, QString(m_config.comfyDir).replace(QDir::homePath(), QStringLiteral("~")))
            .arg(QUrl(m_config.comfyUrl).port(8188));
    }

    static QString describeError(const QJsonObject &o, const QString &fallback)
    {
        QStringList parts;
        const QJsonValue err = o.value("error");
        if (err.isObject()) parts << err.toObject().value("message").toString();
        else if (err.isString()) parts << err.toString();
        const QJsonObject nodes = o.value("node_errors").toObject();
        for (auto it = nodes.constBegin(); it != nodes.constEnd(); ++it)
            for (const QJsonValue &e : it.value().toObject().value("errors").toArray())
                parts << e.toObject().value("details").toString(e.toObject().value("message").toString());
        parts.removeAll(QString());
        return parts.isEmpty() ? fallback : parts.join(QStringLiteral("; "));
    }

    // SDXL base + fp16-fix VAE + the 8-step Lightning LoRA (CFG 1, so the
    // negative prompt is only there because the sampler needs one).
    QJsonObject workflow(const GenRequest &r) const
    {
        const QSize size = imageSize(r.aspect);
        const QByteArray h = QCryptographicHash::hash(r.prompt.toUtf8(), QCryptographicHash::Sha1);
        const qint64 seed = (qint64(quint8(h[0])) << 24 | quint8(h[1]) << 16 | quint8(h[2]) << 8 | quint8(h[3]));
        auto node = [](const char *type, QJsonObject inputs) { return QJsonObject{{"class_type", type}, {"inputs", inputs}}; };
        auto link = [](const char *id, int slot) { return QJsonArray{QString::fromLatin1(id), slot}; };
        return QJsonObject{
            {"1", node("CheckpointLoaderSimple", {{"ckpt_name", m_config.checkpoint}})},
            {"2", node("VAELoader", {{"vae_name", m_config.vae}})},
            {"3", node("LoraLoader", {{"model", link("1", 0)}, {"clip", link("1", 1)}, {"lora_name", m_config.lora},
                                      {"strength_model", 1.0}, {"strength_clip", 1.0}})},
            {"4", node("CLIPTextEncode", {{"clip", link("3", 1)},
                                          {"text", r.prompt + QStringLiteral(", high quality, detailed, sharp focus")}})},
            {"5", node("CLIPTextEncode", {{"clip", link("3", 1)}, {"text", "text, watermark, logo, blurry, deformed"}})},
            {"6", node("EmptyLatentImage", {{"width", size.width()}, {"height", size.height()}, {"batch_size", 1}})},
            {"7", node("KSampler", {{"model", link("3", 0)}, {"positive", link("4", 0)}, {"negative", link("5", 0)},
                                    {"latent_image", link("6", 0)}, {"seed", seed}, {"steps", 8}, {"cfg", 1.0},
                                    {"sampler_name", "euler"}, {"scheduler", "sgm_uniform"}, {"denoise", 1.0}})},
            {"8", node("VAEDecode", {{"samples", link("7", 0)}, {"vae", link("2", 0)}})},
            {"9", node("SaveImage", {{"images", link("8", 0)}, {"filename_prefix", "montage"}})},
        };
    }

    void onSocket(const QString &message)
    {
        if (!m_active) return;
        const QJsonObject o = QJsonDocument::fromJson(message.toUtf8()).object();
        const QJsonObject data = o.value("data").toObject();
        if (!m_promptId.isEmpty() && data.contains("prompt_id") && data.value("prompt_id").toString() != m_promptId) return;
        const QString type = o.value("type").toString();
        if (type == QLatin1String("progress")) {
            const int value = data.value("value").toInt(), max = std::max(1, data.value("max").toInt());
            emit progress(0.15 + 0.75 * value / max, QStringLiteral("Drawing, step %1 of %2").arg(value).arg(max));
        } else if (type == QLatin1String("executing")) {
            const QString n = data.value("node").toString();
            if (n == QLatin1String("1")) emit progress(0.08, QStringLiteral("Loading SDXL"));
            else if (n == QLatin1String("8")) emit progress(0.92, QStringLiteral("Decoding"));
        }
    }

    void poll()
    {
        if (!m_active || m_promptId.isEmpty()) return;
        if (m_started.elapsed() > 10 * 60 * 1000) {
            post(QStringLiteral("/interrupt"), QJsonObject{{"prompt_id", m_promptId}});
            finish({false, false, {}, QStringLiteral("ComfyUI took more than 10 minutes, so it was stopped.")});
            return;
        }
        QNetworkRequest req(QUrl(m_config.comfyUrl + QStringLiteral("/history/") + m_promptId));
        req.setTransferTimeout(5000);
        QNetworkReply *reply = m_net->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            if (!m_active) return;
            if (reply->error() != QNetworkReply::NoError) {
                if (++m_pollErrors >= 6) finish({false, false, {}, notRunning()});
                return;
            }
            m_pollErrors = 0;
            const QJsonObject entry = QJsonDocument::fromJson(reply->readAll()).object().value(m_promptId).toObject();
            if (entry.isEmpty()) return;  // still queued or running
            const QJsonObject status = entry.value("status").toObject();
            if (status.value("status_str").toString() == QLatin1String("error")) {
                QString why = QStringLiteral("ComfyUI failed to make the image");
                for (const QJsonValue &m : status.value("messages").toArray()) {
                    const QJsonArray pair = m.toArray();
                    if (pair.at(0).toString() == QLatin1String("execution_error"))
                        why += QStringLiteral(": ") + pair.at(1).toObject().value("exception_message").toString().trimmed();
                }
                finish({false, false, {}, why});
                return;
            }
            const QJsonObject outputs = entry.value("outputs").toObject();
            for (auto it = outputs.constBegin(); it != outputs.constEnd(); ++it) {
                const QJsonArray images = it.value().toObject().value("images").toArray();
                if (!images.isEmpty()) {
                    m_poll.stop();
                    fetch(images.first().toObject());
                    return;
                }
            }
            if (status.value("completed").toBool()) finish({false, false, {}, QStringLiteral("ComfyUI finished without an image")});
        });
    }

    void fetch(const QJsonObject &image)
    {
        emit progress(0.96, QStringLiteral("Saving"));
        QUrl url(m_config.comfyUrl + QStringLiteral("/view"));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("filename"), image.value("filename").toString());
        q.addQueryItem(QStringLiteral("subfolder"), image.value("subfolder").toString());
        q.addQueryItem(QStringLiteral("type"), image.value("type").toString(QStringLiteral("output")));
        url.setQuery(q);
        QNetworkReply *reply = m_net->get(QNetworkRequest(url));
        connect(reply, &QNetworkReply::finished, this, [this, reply] {
            reply->deleteLater();
            if (!m_active) return;
            const QByteArray png = reply->readAll();
            if (reply->error() != QNetworkReply::NoError || png.isEmpty()) {
                finish({false, false, {}, QStringLiteral("Couldn't download the image from ComfyUI: ") + reply->errorString()});
                return;
            }
            // SDXL's sizes are near the shape, not on it (1344x768 for 16:9):
            // crop the centre so the canvas gets the exact shape.
            QImage img = QImage::fromData(png);
            const double want = shapeRatio(m_aspect);
            if (!img.isNull() && want > 0) {
                const double have = double(img.width()) / img.height();
                if (have > want + 0.001) {
                    const int w = int(std::lround(img.height() * want)) & ~1;
                    img = img.copy((img.width() - w) / 2, 0, w, img.height());
                } else if (have < want - 0.001) {
                    const int h = int(std::lround(img.width() / want)) & ~1;
                    img = img.copy(0, (img.height() - h) / 2, img.width(), h);
                }
            }
            QDir().mkpath(QFileInfo(m_out).absolutePath());
            const QString part = m_out + QStringLiteral(".part");
            QFile f(part);
            const bool written = f.open(QIODevice::WriteOnly) && (img.isNull() ? f.write(png) == png.size() : img.save(&f, "PNG"));
            f.close();
            if (!written) {
                QFile::remove(part);
                finish({false, false, {}, QStringLiteral("Couldn't write ") + m_out});
                return;
            }
            QFile::remove(m_out);
            QFile::rename(part, m_out);
            finish({true, false, m_out, {}});
        });
    }

    void post(const QString &path, const QJsonObject &body)
    {
        QNetworkRequest req(QUrl(m_config.comfyUrl + path));
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        QNetworkReply *reply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }

    void finish(const GenResult &result)
    {
        if (!m_active) return;
        m_active = false;
        m_poll.stop();
        m_ws->close();
        emit finished(result);
    }

    GeneratorConfig m_config;
    QNetworkAccessManager *m_net;
    QWebSocket *m_ws;
    QTimer m_poll;
    QElapsedTimer m_started;
    QString m_out, m_promptId, m_aspect;
    int m_pollErrors = 0;
    bool m_active = false;
};

// ---------------------------------------------------------------- scripts

// Runs one of the helper scripts in scripts/generate with a model's own
// Python. Scripts print "PROGRESS <0-1> <note>" lines and a final JSON line.
class ScriptBackend : public GeneratorBackend
{
public:
    using GeneratorBackend::GeneratorBackend;

    void cancel() override
    {
        if (!m_process) return;
        m_cancelled = true;
        m_process->kill();  // finished() follows
    }

protected:
    void start(const QString &python, const QString &script, const QStringList &args, const QString &outPath,
               const QString &what)
    {
        m_out = outPath;
        m_cancelled = false;
        m_lastJson = {};
        m_buffer.clear();
        m_stderr.clear();
        const QString path = helperScript(script);
        if (path.isEmpty()) {
            emit finished({false, false, {}, QStringLiteral("The %1 helper script is missing from the app.").arg(what)});
            return;
        }
        QDir().mkpath(QFileInfo(outPath).absolutePath());
        QFile::remove(partPath());
        m_process = new QProcess(this);
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PYTHONUNBUFFERED"), QStringLiteral("1"));
        env.insert(QStringLiteral("HF_HUB_OFFLINE"), QStringLiteral("1"));
        env.insert(QStringLiteral("PYTORCH_ENABLE_MPS_FALLBACK"), QStringLiteral("1"));
        m_process->setProcessEnvironment(env);
        m_process->setWorkingDirectory(QFileInfo(path).absolutePath());
        connect(m_process, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
        connect(m_process, &QProcess::readyReadStandardError, this, [this] {
            m_stderr += m_process->readAllStandardError();
            if (m_stderr.size() > 20000) m_stderr = m_stderr.right(8000);
        });
        connect(m_process, &QProcess::finished, this, [this, what](int code, QProcess::ExitStatus status) {
            readOutput();
            QProcess *p = m_process;
            m_process = nullptr;
            p->deleteLater();
            if (m_cancelled) {
                QFile::remove(partPath());
                emit finished({false, true, {}, QStringLiteral("Cancelled")});
                return;
            }
            if (status != QProcess::NormalExit || code != 0 || !QFileInfo::exists(partPath())) {
                QString why = m_lastJson.value("error").toString();
                if (why.isEmpty()) {
                    const QStringList lines = QString::fromUtf8(m_stderr).trimmed().split('\n');
                    why = lines.isEmpty() ? QStringLiteral("exit code %1").arg(code) : lines.last().trimmed();
                }
                emit finished({false, false, {}, QStringLiteral("%1 failed: %2").arg(what, why)});
                return;
            }
            QFile::remove(m_out);
            QFile::rename(partPath(), m_out);
            emit finished({true, false, m_out, {}});
        });
        connect(m_process, &QProcess::errorOccurred, this, [this, python, what](QProcess::ProcessError e) {
            if (e != QProcess::FailedToStart || !m_process) return;
            m_process->deleteLater();
            m_process = nullptr;
            emit finished({false, false, {}, QStringLiteral("Couldn't start %1 (%2).").arg(what, python)});
        });
        m_process->start(python, QStringList{path} + args);
    }

    // Scripts write here; the file gets its real name only when it's complete.
    static QString partPath(const QString &out)
    {
        const QFileInfo fi(out);
        return fi.absolutePath() + QLatin1Char('/') + fi.completeBaseName() + QStringLiteral(".part.") + fi.suffix();
    }
    QString partPath() const { return partPath(m_out); }

private:
    void readOutput()
    {
        if (!m_process) return;
        m_buffer += m_process->readAllStandardOutput();
        qsizetype nl;
        while ((nl = m_buffer.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_buffer.left(nl)).trimmed();
            m_buffer.remove(0, nl + 1);
            if (line.startsWith(QLatin1String("PROGRESS "))) {
                const QString rest = line.mid(9);
                emit progress(rest.section(QLatin1Char(' '), 0, 0).toDouble(), rest.section(QLatin1Char(' '), 1).trimmed());
            } else if (line.startsWith(QLatin1Char('{'))) {
                m_lastJson = QJsonDocument::fromJson(line.toUtf8()).object();
            }
        }
    }

    QPointer<QProcess> m_process;
    QByteArray m_buffer, m_stderr;
    QJsonObject m_lastJson;
    QString m_out;
    bool m_cancelled = false;
};

class KokoroBackend : public ScriptBackend
{
public:
    KokoroBackend(const GeneratorConfig &config, QObject *parent)
        : ScriptBackend(parent)
        , m_config(config)
    {
    }

    QString unavailable() const override
    {
        if (!QFileInfo::exists(m_config.kokoroPython))
            return QStringLiteral("No Python with Kokoro found (looked for %1). Set MONTAGE_KOKORO_PYTHON to one.").arg(m_config.kokoroPython);
        if (!QFileInfo::exists(m_config.kokoroModel + QStringLiteral("/kokoro-v1_0.pth")))
            return QStringLiteral("The Kokoro voice model isn't in %1.").arg(m_config.kokoroModel);
        return {};
    }

    void generate(const GenRequest &r, const QString &outPath) override
    {
        if (const QString why = unavailable(); !why.isEmpty()) {
            emit finished({false, false, {}, why});
            return;
        }
        const QString voice = voiceNames().contains(r.voice) ? r.voice : voiceNames().first();
        start(m_config.kokoroPython, QStringLiteral("kokoro_tts.py"),
              {r.text, QStringLiteral("--out"), partPath(outPath), QStringLiteral("--model-dir"), m_config.kokoroModel,
               QStringLiteral("--voice"), voice},
              outPath, QStringLiteral("The voiceover"));
    }

private:
    GeneratorConfig m_config;
};

class AceStepBackend : public ScriptBackend
{
public:
    AceStepBackend(const GeneratorConfig &config, QObject *parent)
        : ScriptBackend(parent)
        , m_config(config)
    {
    }

    QString unavailable() const override
    {
        if (!QFileInfo::exists(m_config.acePython))
            return QStringLiteral("ACE-Step's Python isn't at %1. Set MONTAGE_ACE_PYTHON to it.").arg(m_config.acePython);
        if (!QFileInfo::exists(m_config.aceModel + QStringLiteral("/acestep-v15-turbo")))
            return QStringLiteral("The ACE-Step 1.5 model isn't in %1.").arg(m_config.aceModel);
        return {};
    }

    void generate(const GenRequest &r, const QString &outPath) override
    {
        if (const QString why = unavailable(); !why.isEmpty()) {
            emit finished({false, false, {}, why});
            return;
        }
        start(m_config.acePython, QStringLiteral("acestep_music.py"),
              {QStringLiteral("--ace-root"), m_config.aceRoot, QStringLiteral("--model-dir"), m_config.aceModel,
               QStringLiteral("--caption"), r.prompt + QStringLiteral(", instrumental"),
               QStringLiteral("--seconds"), QString::number(std::clamp(std::ceil(r.seconds), 5.0, 180.0)),
               QStringLiteral("--bpm"), QString::number(bpmFor(r.prompt)), QStringLiteral("--out"), partPath(outPath)},
              outPath, QStringLiteral("The music"));
    }

private:
    // The language model that would pick a tempo isn't installed; take it from the words.
    static int bpmFor(const QString &prompt)
    {
        const QString p = prompt.toLower();
        static const QRegularExpression fast(QStringLiteral(R"(\b(upbeat|energetic|hype|fast|dance|edm|punchy|driving|workout|party|electro|techno|house|trap|rock)\b)"));
        static const QRegularExpression slow(QStringLiteral(R"(\b(soft|calm|cozy|cosy|gentle|ambient|lo-?fi|chill|relaxing|slow|piano|acoustic|warm|dreamy|elegant|luxury|cinematic)\b)"));
        if (fast.match(p).hasMatch()) return 120;
        if (slow.match(p).hasMatch()) return 80;
        return 100;
    }

    GeneratorConfig m_config;
};

}  // namespace

// ---------------------------------------------------------------- requests

QStringList generateKinds()
{
    return {"image", "voiceover", "music"};
}

QStringList voiceNames()
{
    return {"af_heart", "af_bella", "am_michael", "am_puck", "bf_emma", "bm_george"};
}

QString voiceFor(const QString &description)
{
    const QString d = description.trimmed().toLower();
    if (voiceNames().contains(d)) return d;
    const bool british = QRegularExpression(QStringLiteral(R"(\b(british|uk|english accent|london)\b)")).match(d).hasMatch();
    const bool male = QRegularExpression(QStringLiteral(R"(\b(male|man|man's|guy|deep|baritone|masculine|him|his)\b)")).match(d).hasMatch();
    const bool female = QRegularExpression(QStringLiteral(R"(\b(female|woman|woman's|girl|lady|feminine|her)\b)")).match(d).hasMatch();
    if (british) return male ? QStringLiteral("bm_george") : QStringLiteral("bf_emma");
    if (male) return QStringLiteral("am_michael");
    if (female) return QStringLiteral("af_heart");
    return {};
}

QString GenRequest::label() const
{
    if (kind == QLatin1String("voiceover")) return QStringLiteral("Voiceover · “%1”").arg(shorten(text));
    if (kind == QLatin1String("music")) return QStringLiteral("Music · %1 · %2s").arg(shorten(prompt, 40)).arg(std::ceil(seconds));
    return QStringLiteral("Image · %1").arg(shorten(prompt));
}

QString GenRequest::fileName() const
{
    const QString words = kind == QLatin1String("voiceover") ? text : prompt;
    const QString key = QStringList{kind, prompt.simplified(), text.simplified(), voice, QString::number(std::ceil(seconds)), aspect}.join('|');
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(6));
    const QString ext = kind == QLatin1String("image") ? QStringLiteral("png") : QStringLiteral("wav");
    return QStringLiteral("%1-%2-%3.%4").arg(kind, slug(words), hash, ext);
}

QString generatedKind(const QString &fileName)
{
    static const QRegularExpression re(QStringLiteral(R"(^(image|voiceover|music)-[a-z0-9-]+-[0-9a-f]{6}\.(png|wav)$)"));
    const auto m = re.match(QFileInfo(fileName).fileName());
    return m.hasMatch() ? m.captured(1) : QString();
}

bool isVoiceover(const Material &m)
{
    if (m.kind != MaterialKind::Audio) return false;
    if (const QString k = generatedKind(m.name); !k.isEmpty()) return k == QLatin1String("voiceover");
    static const QRegularExpression named(QStringLiteral(R"((^|[^a-z])(voice[ _-]?over|vo|narration|narrator|voice)([^a-z]|$))"),
                                          QRegularExpression::CaseInsensitiveOption);
    return named.match(QFileInfo(m.name).completeBaseName()).hasMatch();
}

QList<GenRequest> generateRequests(const QJsonObject &plan, QStringList *warnings)
{
    QList<GenRequest> out;
    QSet<QString> names;
    const QJsonArray items = plan.value("generate").toArray();
    static const QRegularExpression mediaAlias(QStringLiteral("^m\\d+$"));
    for (int i = 0; i < items.size(); ++i) {
        const QJsonObject o = items[i].toObject();
        const QString where = QStringLiteral("generate[%1]").arg(i);
        GenRequest r;
        r.kind = o.value("kind").toString().trimmed().toLower();
        if (r.kind == QLatin1String("voice") || r.kind == QLatin1String("narration") || r.kind == QLatin1String("speech")) r.kind = QStringLiteral("voiceover");
        if (r.kind == QLatin1String("photo") || r.kind == QLatin1String("picture") || r.kind == QLatin1String("background")) r.kind = QStringLiteral("image");
        if (r.kind == QLatin1String("song") || r.kind == QLatin1String("soundtrack")) r.kind = QStringLiteral("music");
        r.prompt = o.value("prompt").toString().simplified();
        r.text = o.value("text").toString().simplified();
        if (r.kind == QLatin1String("voiceover") && r.text.isEmpty()) r.text = r.prompt;
        if (r.kind != QLatin1String("voiceover") && r.prompt.isEmpty()) r.prompt = r.text;
        if (!generateKinds().contains(r.kind)) {
            if (warnings) *warnings << QStringLiteral("%1: unknown kind \"%2\"; use one of %3.").arg(where, r.kind, generateKinds().join(", "));
            continue;
        }
        if ((r.kind == QLatin1String("voiceover") ? r.text : r.prompt).isEmpty()) {
            if (warnings)
                *warnings << QStringLiteral("%1: a %2 needs \"%3\"; left out.").arg(where, r.kind,
                                                                                    r.kind == QLatin1String("voiceover") ? "text" : "prompt");
            continue;
        }
        if (r.kind == QLatin1String("voiceover")) {
            r.prompt.clear();
            const QString voice = o.value("voice").toString();
            r.voice = voiceFor(voice);
            if (r.voice.isEmpty()) r.voice = voiceNames().first();
        } else {
            r.text.clear();
        }
        if (r.kind == QLatin1String("music")) {
            const double s = seconds(o.value("seconds"));
            r.seconds = std::isnan(s) || s <= 0 ? 15 : std::clamp(s, 5.0, 180.0);
        }
        if (r.kind == QLatin1String("image")) r.aspect = o.value("aspect").toString().trimmed();
        r.as = o.value("as").toString().trimmed().toLower();
        if (r.as.isEmpty() || mediaAlias.match(r.as).hasMatch() || names.contains(r.as)) r.as = QStringLiteral("g%1").arg(i + 1);
        names.insert(r.as);
        out << r;
    }
    return out;
}

void bindGenerated(const QHash<QString, QString> &made, const Draft &d, QJsonObject *plan)
{
    QSet<QString> failed;
    for (const GenRequest &r : generateRequests(*plan))
        if (!made.contains(r.as)) failed.insert(r.as);
    const MediaIndex index(d);

    // Small models sometimes refer to a name they never listed ("g2" for the
    // only item, called "g1"). Such a reference takes a made file of the
    // right kind that nothing else uses, else it's left out.
    static const QRegularExpression genName(QStringLiteral("^g\\d+$"));
    QStringList unused;
    QSet<QString> referenced;
    for (const char *key : {"main", "overlays", "audio"})
        for (const QJsonValue &v : plan->value(QLatin1String(key)).toArray())
            referenced << v.toObject().value("media").toString().trimmed().toLower();
    for (auto it = made.cbegin(); it != made.cend(); ++it)
        if (!referenced.contains(it.key())) unused << it.key();
    unused.sort();
    auto adopt = [&](bool audio) {
        for (qsizetype i = 0; i < unused.size(); ++i) {
            const Material *m = d.material(index.materialId(made.value(unused[i])));
            if (m && (m->kind == MaterialKind::Audio) == audio) return unused.takeAt(i);
        }
        return QString();
    };
    // Names used in the wrong place (a voiceover's name in "main") swap with
    // a name of the right kind, as long as that one fits where it's used too.
    QHash<QString, QString> map = made;
    QHash<QString, bool> inAudio;  // name -> first used in "audio"
    for (const char *key : {"main", "overlays", "audio"})
        for (const QJsonValue &v : plan->value(QLatin1String(key)).toArray()) {
            const QString ref = v.toObject().value("media").toString().trimmed().toLower();
            if (map.contains(ref) && !inAudio.contains(ref)) inAudio.insert(ref, QLatin1String(key) == QLatin1String("audio"));
        }
    auto fits = [&](const QString &alias, bool audio) {
        const Material *m = d.material(index.materialId(alias));
        return m && (m->kind == MaterialKind::Audio) == audio;
    };
    for (auto a = inAudio.cbegin(); a != inAudio.cend(); ++a) {
        if (fits(map.value(a.key()), a.value())) continue;
        for (auto b = map.begin(); b != map.end(); ++b) {
            if (b.key() == a.key() || !fits(b.value(), a.value())) continue;
            if (inAudio.contains(b.key()) && !fits(map.value(a.key()), inAudio.value(b.key()))) continue;
            std::swap(map[a.key()], b.value());
            break;
        }
    }

    for (const char *key : {"main", "overlays", "audio"}) {
        if (!plan->contains(QLatin1String(key))) continue;
        QJsonArray out;
        for (const QJsonValue &v : plan->value(QLatin1String(key)).toArray()) {
            QJsonObject item = v.toObject();
            QString ref = item.value("media").toString().trimmed().toLower();
            if (failed.contains(ref)) continue;
            if (!map.contains(ref) && genName.match(ref).hasMatch() && index.materialId(ref).isEmpty()) {
                ref = adopt(QLatin1String(key) == QLatin1String("audio"));
                if (ref.isEmpty()) continue;
            }
            if (map.contains(ref)) {
                const QString alias = map.value(ref);
                if (!fits(alias, QLatin1String(key) == QLatin1String("audio"))) continue;  // no file of the right kind
                item.insert("media", alias);
                const Material *m = d.material(index.materialId(alias));
                if (m && m->isTimed()) {  // the model couldn't know the length
                    const double len = std::floor(m->durationUs / 1e4) / 100;
                    const double from = std::clamp(seconds(item.value("from")), 0.0, std::max(0.0, len - 0.1));
                    const double to = seconds(item.value("to"));
                    item.insert("from", std::isnan(from) || isVoiceover(*m) ? 0.0 : from);
                    if (std::isnan(to) || to > len || isVoiceover(*m)) item.insert("to", len);  // never cut a voiceover short
                }
            }
            out.append(item);
        }
        plan->insert(QLatin1String(key), out);
    }
    plan->remove("generate");
}

// ---------------------------------------------------------------- config

GeneratorConfig GeneratorConfig::load()
{
    QSettings settings;
    auto value = [&](const char *key, const QString &fallback) {
        const QByteArray env = QByteArray("MONTAGE_") + QByteArray(key).toUpper();
        QString v = qEnvironmentVariable(env.constData());
        if (v.isEmpty()) v = settings.value(QStringLiteral("generate/") + QLatin1String(key)).toString();
        v = v.trimmed();
        if (v.isEmpty()) return fallback;
        return v.contains(QLatin1String("://")) ? v : expandHome(v);  // cleanPath would mangle URLs
    };
    const QString studio = QDir::homePath() + QStringLiteral("/ai-studio");
    GeneratorConfig c;
    c.outputDir = value("output_dir", QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QStringLiteral("/Montage/Generated"));
    c.comfyUrl = value("comfy_url", QStringLiteral("http://127.0.0.1:8188"));
    if (c.comfyUrl.endsWith(QLatin1Char('/'))) c.comfyUrl.chop(1);
    c.comfyDir = value("comfy_dir", studio + QStringLiteral("/comfyui"));
    c.checkpoint = value("checkpoint", QStringLiteral("sd_xl_base_1.0.safetensors"));
    c.vae = value("vae", QStringLiteral("sdxl_vae_fp16_fix.safetensors"));
    c.lora = value("lora", QStringLiteral("sdxl_lightning_8step_lora.safetensors"));
    // Kokoro has no checkout of its own in ~/ai-studio; its env was set up
    // next to the animation-studio project that first used it.
    c.kokoroPython = value("kokoro_python", firstExisting({
        studio + QStringLiteral("/kokoro/.venv/bin/python"),
        studio + QStringLiteral("/envs/kokoro/.venv/bin/python"),
        studio + QStringLiteral("/tools/kokoro/.venv/bin/python"),
        QDir::homePath() + QStringLiteral("/SPL/Projects/animation-studio/envs/kokoro/.venv/bin/python"),
    }));
    c.kokoroModel = value("kokoro_model", studio + QStringLiteral("/models/kokoro-82m"));
    c.aceRoot = value("ace_root", studio + QStringLiteral("/ace-step"));
    c.acePython = value("ace_python", c.aceRoot + QStringLiteral("/.venv/bin/python"));
    c.aceModel = value("ace_model", studio + QStringLiteral("/models/ace-step-1.5"));
    c.ollamaUrl = value("ollama_url", QStringLiteral("http://127.0.0.1:11434"));
    c.freeMemory = value("free_memory", QStringLiteral("1")) != QLatin1String("0");
    return c;
}

// ---------------------------------------------------------------- runner

Generators::Generators(QObject *parent)
    : Generators(GeneratorConfig::load(), parent)
{
}

Generators::Generators(const GeneratorConfig &config, QObject *parent)
    : QObject(parent)
    , m_config(config)
{
    setBackend(QStringLiteral("image"), new ComfyImageBackend(m_config, this));
    setBackend(QStringLiteral("voiceover"), new KokoroBackend(m_config, this));
    setBackend(QStringLiteral("music"), new AceStepBackend(m_config, this));
}

void Generators::setBackend(const QString &kind, GeneratorBackend *backend)
{
    delete m_backends.take(kind);
    backend->setParent(this);
    m_backends.insert(kind, backend);
    connect(backend, &GeneratorBackend::progress, this, [this, kind](double fraction, const QString &note) {
        if (running() && m_queue[m_index].kind == kind) emit itemProgress(m_index, std::clamp(fraction, 0.0, 1.0), note);
    });
    connect(backend, &GeneratorBackend::finished, this, [this, kind](const GenResult &result) {
        if (!running() || m_queue[m_index].kind != kind || m_results[m_index].ok || !m_results[m_index].error.isEmpty()) return;
        m_results[m_index] = result;
        if (result.ok) m_lastHeavy = kind == QLatin1String("voiceover") ? m_lastHeavy : kind;
        emit itemFinished(m_index, result);
        ++m_index;
        QTimer::singleShot(0, this, &Generators::next);
    });
}

QString Generators::unavailable(const QString &kind) const
{
    GeneratorBackend *b = m_backends.value(kind);
    return b ? b->unavailable() : QStringLiteral("Can't generate \"%1\"; use one of %2.").arg(kind, generateKinds().join(", "));
}

void Generators::run(const QList<GenRequest> &requests)
{
    if (running()) return;
    m_queue = requests;
    m_results = QList<GenResult>(requests.size());
    m_cancelled = false;
    m_index = 0;
    QTimer::singleShot(0, this, &Generators::next);
}

void Generators::cancel()
{
    if (!running()) return;
    m_cancelled = true;
    if (m_index < m_queue.size())
        if (GeneratorBackend *b = m_backends.value(m_queue[m_index].kind)) b->cancel();
}

void Generators::next()
{
    if (!running()) return;
    if (m_cancelled || m_index >= m_queue.size()) {
        for (int i = m_index; i < m_queue.size(); ++i)
            if (!m_results[i].ok && m_results[i].error.isEmpty()) m_results[i] = {false, true, {}, QStringLiteral("Cancelled")};
        m_index = -1;
        emit finished(m_results, m_cancelled);
        return;
    }
    const GenRequest &r = m_queue[m_index];
    emit itemStarted(m_index, int(m_queue.size()), r.label());
    const QString out = m_config.outputDir + QLatin1Char('/') + r.fileName();
    GeneratorBackend *b = m_backends.value(r.kind);
    GenResult early;
    if (QFileInfo(out).size() > 0) early = {true, false, out, {}};  // made before: same request, same file
    else if (!b) early = {false, false, {}, QStringLiteral("Can't generate \"%1\".").arg(r.kind)};
    if (early.ok || !early.error.isEmpty()) {
        m_results[m_index] = early;
        emit itemFinished(m_index, early);
        ++m_index;
        QTimer::singleShot(0, this, &Generators::next);
        return;
    }
    freeMemoryFor(r.kind);
    b->generate(r, out);
}

// Image and music models each want most of a 24 GB Mac. Before one runs,
// unload the chat models Ollama keeps around, and ComfyUI's SDXL before music.
void Generators::freeMemoryFor(const QString &kind)
{
    if (!m_config.freeMemory || kind == QLatin1String("voiceover")) return;
    auto *net = new QNetworkAccessManager(this);
    QTimer::singleShot(30000, net, &QObject::deleteLater);
    auto post = [net](const QString &url, const QJsonObject &body) {
        QNetworkRequest req{QUrl(url)};
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        req.setTransferTimeout(5000);
        QNetworkReply *reply = net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        QObject::connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    };
    QNetworkRequest ps{QUrl(m_config.ollamaUrl + QStringLiteral("/api/ps"))};
    ps.setTransferTimeout(3000);
    QNetworkReply *reply = net->get(ps);
    const QString ollama = m_config.ollamaUrl;
    connect(reply, &QNetworkReply::finished, net, [reply, post, ollama] {
        reply->deleteLater();
        for (const QJsonValue &m : QJsonDocument::fromJson(reply->readAll()).object().value("models").toArray())
            post(ollama + QStringLiteral("/api/generate"), QJsonObject{{"model", m.toObject().value("name").toString()}, {"keep_alive", 0}});
    });
    if (kind == QLatin1String("music") && m_lastHeavy == QLatin1String("image"))
        post(m_config.comfyUrl + QStringLiteral("/free"), QJsonObject{{"unload_models", true}, {"free_memory", true}});
}

// With nothing imported, small models name generated items like media
// ("as": "m1"). Such a name that isn't real media yet becomes "g<n>" here,
// with its references, before importing gives "m1" to a real file.
static QJsonObject renameMediaLikeNames(QJsonObject plan, const Draft &current)
{
    static const QRegularExpression mediaAlias(QStringLiteral("^m\\d+$"));
    const MediaIndex index(current);
    QJsonArray items = plan.value("generate").toArray();
    QSet<QString> taken;
    for (const QJsonValue &v : items) taken << v.toObject().value("as").toString().trimmed().toLower();
    QHash<QString, QString> renamed;
    int n = 0;
    for (qsizetype i = 0; i < items.size(); ++i) {
        QJsonObject o = items[i].toObject();
        const QString as = o.value("as").toString().trimmed().toLower();
        if (!mediaAlias.match(as).hasMatch() || !index.materialId(as).isEmpty()) continue;
        QString fresh;
        do fresh = QStringLiteral("g%1").arg(++n);
        while (taken.contains(fresh));
        taken << fresh;
        renamed.insert(as, fresh);
        o.insert("as", fresh);
        items[i] = o;
    }
    if (renamed.isEmpty()) return plan;
    plan.insert("generate", items);
    for (const char *key : {"main", "overlays", "audio"}) {
        QJsonArray list = plan.value(QLatin1String(key)).toArray();
        for (qsizetype i = 0; i < list.size(); ++i) {
            QJsonObject o = list[i].toObject();
            const QString ref = o.value("media").toString().trimmed().toLower();
            if (!renamed.contains(ref)) continue;
            o.insert("media", renamed.value(ref));
            list[i] = o;
        }
        if (plan.contains(QLatin1String(key))) plan.insert(QLatin1String(key), list);
    }
    return plan;
}

void Generators::resolve(const QJsonObject &original, const Draft &current, const ImportFn &import,
                         const std::function<void(const GenResolution &)> &done)
{
    const QJsonObject plan = renameMediaLikeNames(original, current);
    QStringList warnings;
    QList<GenRequest> requests = generateRequests(plan, &warnings);
    if (requests.isEmpty() || running()) {
        GenResolution r;
        r.plan = plan;
        bindGenerated({}, current, &r.plan);
        r.errors = warnings;
        if (running()) r.errors << QStringLiteral("Already generating; try again when it finishes.");
        done(r);
        return;
    }

    // Images default to the shape the video will have.
    static const QStringList shapes{"16:9", "9:16", "1:1", "4:5", "3:4", "4:3", "21:9"};
    QString aspect = plan.value("aspect").toString().trimmed();
    if (!shapes.contains(aspect)) {
        const double ratio = double(current.canvas.width) / std::max(1, current.canvas.height);
        aspect = ratio > 1.2 ? QStringLiteral("16:9") : ratio < 0.7 ? QStringLiteral("9:16") : ratio < 0.9 ? QStringLiteral("4:5") : QStringLiteral("1:1");
    }
    for (GenRequest &r : requests)
        if (r.kind == QLatin1String("image") && !shapes.contains(r.aspect)) r.aspect = aspect;

    auto *conn = new QMetaObject::Connection;
    *conn = connect(this, &Generators::finished, this,
                    [this, conn, plan, requests, warnings, import, done](const QList<GenResult> &results, bool cancelled) {
                        disconnect(*conn);
                        delete conn;
                        GenResolution r;
                        r.cancelled = cancelled;
                        r.errors = warnings;
                        QStringList paths;
                        for (const GenResult &g : results)
                            if (g.ok) paths << g.path;
                        const Draft &d = import(paths);
                        const MediaIndex index(d);
                        for (int i = 0; i < requests.size(); ++i) {
                            if (!results[i].ok) {
                                if (!results[i].cancelled) r.errors << requests[i].label() + QStringLiteral(": ") + results[i].error;
                                if (!r.failedKinds.contains(requests[i].kind)) r.failedKinds << requests[i].kind;
                                continue;
                            }
                            for (const Material &m : d.materials) {
                                if (QFileInfo(m.path) != QFileInfo(results[i].path)) continue;
                                r.made.insert(requests[i].as, index.alias(m.id));
                                if (!r.generated.contains(index.alias(m.id))) r.generated << index.alias(m.id);
                            }
                        }
                        // Bind against the plan with the cleaned-up requests (names, aspects).
                        QJsonObject p = plan;
                        QJsonArray items;
                        for (const GenRequest &q : requests) items.append(QJsonObject{{"kind", q.kind}, {"prompt", q.prompt}, {"text", q.text}, {"as", q.as}});
                        p.insert("generate", items);
                        bindGenerated(r.made, d, &p);
                        r.plan = p;
                        done(r);
                    });
    run(requests);
}

}  // namespace mtg::ai
