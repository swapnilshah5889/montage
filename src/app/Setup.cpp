#include "Setup.h"

#include "Agent.h"
#include "ai/Generators.h"
#include "ai/MediaIndexer.h"
#include "engine/Speech.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QUrl>

namespace {

constexpr const char *kVisionModel = "qwen3-vl:8b-instruct";
constexpr const char *kWhisperUrl = "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.bin";

QString ollamaUrl()
{
    return mtg::ai::VisionConfig::load().ollamaUrl;
}

QString gb(double bytes)
{
    return bytes >= 1e9 ? QStringLiteral("%1 GB").arg(bytes / 1e9, 0, 'f', 1) : QStringLiteral("%1 MB").arg(qRound(bytes / 1e6));
}

}  // namespace

Setup::Setup(Agent *agent, QObject *parent)
    : QObject(parent)
    , m_agent(agent)
    , m_net(new QNetworkAccessManager(this))
{
    s_instance = this;
    refresh();
}

Setup *Setup::create(QQmlEngine *, QJSEngine *)
{
    Q_ASSERT(s_instance);
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

bool Setup::ready() const
{
    for (const Item &i : m_list)
        if (i.required && i.state != QLatin1String("ok")) return false;
    return !m_list.isEmpty();
}

bool Setup::seen() const
{
    return QSettings().value(QStringLiteral("setup/seen")).toBool();
}

void Setup::markSeen()
{
    QSettings().setValue(QStringLiteral("setup/seen"), true);
    emit changed();
}

QString Setup::chatModel() const
{
    // The active profile's model when it's local; the default otherwise.
    const QVariantMap p = m_agent ? m_agent->profileMap() : QVariantMap{};
    const QString base = p.value("baseUrl").toString();
    if (!p.value("model").toString().isEmpty() && (base.contains(QLatin1String("11434")) || base.contains(QLatin1String("localhost"))
                                                    || base.contains(QLatin1String("127.0.0.1"))))
        return p.value("model").toString();
    return QStringLiteral("qwen2.5:7b");
}

Setup::Item *Setup::find(const QString &id)
{
    for (Item &i : m_list)
        if (i.id == id) return &i;
    return nullptr;
}

void Setup::setItem(const Item &item)
{
    if (Item *i = find(item.id)) *i = item;
    else m_list << item;
    publish();
}

void Setup::publish()
{
    m_items.clear();
    for (const Item &i : m_list)
        m_items << QVariantMap{{"id", i.id}, {"name", i.name}, {"purpose", i.purpose}, {"state", i.state}, {"detail", i.detail},
                               {"action", i.action}, {"progress", i.progress}, {"required", i.required}};
    emit changed();
}

void Setup::refresh()
{
    // Rows in order of importance; the Ollama ones fill in when it answers.
    if (m_list.isEmpty()) {
        for (const char *id : {"ollama", "chat", "vision", "whisper", "comfyui", "kokoro", "acestep"})
            m_list << Item{QLatin1String(id), {}, {}, QStringLiteral("working"), QStringLiteral("Checking…"), {}, -1,
                           QLatin1String(id) == QLatin1String("ollama") || QLatin1String(id) == QLatin1String("chat")};
        m_list[0].name = QStringLiteral("Ollama");
        m_list[1].name = QStringLiteral("Assistant model");
        m_list[2].name = QStringLiteral("Vision model");
    }
    // What doesn't need the network first.
    const mtg::ai::GeneratorConfig gen = mtg::ai::GeneratorConfig::load();
    const QString whisperModel = mtg::whisperModelPath();
    if (!m_jobs.contains(QStringLiteral("whisper")))
        setItem({QStringLiteral("whisper"), QStringLiteral("Speech recognition (whisper)"),
                 QStringLiteral("Captions, the transcript and editing by what's said"),
                 whisperModel.isEmpty() ? QStringLiteral("missing") : QStringLiteral("ok"),
                 whisperModel.isEmpty() ? QStringLiteral("466 MB download") : QFileInfo(whisperModel).fileName(),
                 whisperModel.isEmpty() ? QStringLiteral("Download") : QString(), -1, false});
    const bool comfy = QFileInfo::exists(gen.comfyDir);
    setItem({QStringLiteral("comfyui"), QStringLiteral("Image generation (ComfyUI)"), QStringLiteral("Makes product shots and backgrounds"),
             comfy ? QStringLiteral("ok") : QStringLiteral("optional"),
             comfy ? QStringLiteral("installed; started when needed") : QStringLiteral("not installed"),
             comfy ? QString() : QStringLiteral("How to install"), -1, false});
    const bool voice = QFileInfo::exists(gen.kokoroModel) && QFileInfo(gen.kokoroPython).isExecutable();
    setItem({QStringLiteral("kokoro"), QStringLiteral("Voiceover (Kokoro)"), QStringLiteral("Reads your script aloud"),
             voice ? QStringLiteral("ok") : QStringLiteral("optional"), voice ? QStringLiteral("installed") : QStringLiteral("not installed"),
             voice ? QString() : QStringLiteral("How to install"), -1, false});
    const bool music = QFileInfo::exists(gen.aceModel) && QFileInfo(gen.acePython).isExecutable();
    setItem({QStringLiteral("acestep"), QStringLiteral("Music (ACE-Step)"), QStringLiteral("Composes a soundtrack"),
             music ? QStringLiteral("ok") : QStringLiteral("optional"), music ? QStringLiteral("installed") : QStringLiteral("not installed"),
             music ? QString() : QStringLiteral("How to install"), -1, false});

    // Ollama and its models.
    QNetworkRequest req(QUrl(ollamaUrl() + QStringLiteral("/api/tags")));
    req.setTransferTimeout(3000);
    QNetworkReply *reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_ollama = reply->error() == QNetworkReply::NoError;
        m_installed.clear();
        for (const QJsonValue &v : QJsonDocument::fromJson(reply->readAll()).object().value("models").toArray())
            m_installed << v.toObject().value("name").toString();
        setItem({QStringLiteral("ollama"), QStringLiteral("Ollama"), QStringLiteral("Runs the AI models on this Mac"),
                 m_ollama ? QStringLiteral("ok") : QStringLiteral("missing"),
                 m_ollama ? QStringLiteral("running") : QStringLiteral("not running or not installed"),
                 m_ollama ? QString() : QStringLiteral("Get Ollama"), -1, true});
        const QString chat = chatModel();
        auto has = [&](const QString &m) { return m_installed.contains(m) || m_installed.contains(m + QStringLiteral(":latest")); };
        if (!m_jobs.contains(QStringLiteral("chat")))
            setItem({QStringLiteral("chat"), QStringLiteral("Assistant model"), QStringLiteral("Understands what you ask and plans the edit"),
                     !m_ollama ? QStringLiteral("missing") : has(chat) ? QStringLiteral("ok") : QStringLiteral("missing"),
                     has(chat) ? chat : chat + QStringLiteral(" (about 4.7 GB)"),
                     m_ollama && !has(chat) ? QStringLiteral("Download") : QString(), -1, true});
        bool anyVision = false;
        for (const QString &m : m_installed)
            anyVision |= m.contains(QLatin1String("-vl")) || m.contains(QLatin1String("vision")) || m.contains(QLatin1String("llava"));
        if (!m_jobs.contains(QStringLiteral("vision")))
            setItem({QStringLiteral("vision"), QStringLiteral("Vision model"), QStringLiteral("Sees what's in your clips to pick the best shots"),
                     !m_ollama ? QStringLiteral("missing") : anyVision ? QStringLiteral("ok") : QStringLiteral("missing"),
                     anyVision ? QStringLiteral("installed") : QStringLiteral("%1 (about 6 GB)").arg(QLatin1String(kVisionModel)),
                     m_ollama && !anyVision ? QStringLiteral("Download") : QString(), -1, false});
    });
}

void Setup::act(const QString &id)
{
    if (id == QLatin1String("ollama")) QDesktopServices::openUrl(QUrl(QStringLiteral("https://ollama.com/download")));
    else if (id == QLatin1String("chat")) pullModel(id, chatModel());
    else if (id == QLatin1String("vision")) pullModel(id, QLatin1String(kVisionModel));
    else if (id == QLatin1String("whisper")) downloadWhisper();
    else if (id == QLatin1String("comfyui")) QDesktopServices::openUrl(QUrl(QStringLiteral("https://docs.comfy.org/installation/desktop/macos")));
    else if (id == QLatin1String("kokoro")) QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/hexgrad/kokoro")));
    else if (id == QLatin1String("acestep")) QDesktopServices::openUrl(QUrl(QStringLiteral("https://github.com/ace-step/ACE-Step")));
}

void Setup::cancel(const QString &id)
{
    if (QNetworkReply *r = m_jobs.value(id)) r->abort();
}

void Setup::pullModel(const QString &id, const QString &model)
{
    if (m_jobs.contains(id)) return;
    Item item = *find(id);
    item.state = QStringLiteral("working");
    item.detail = QStringLiteral("Downloading %1…").arg(model);
    item.action = QStringLiteral("Cancel");
    item.progress = 0;
    setItem(item);
    // Ollama streams progress as JSON lines.
    QNetworkRequest req(QUrl(ollamaUrl() + QStringLiteral("/api/pull")));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    QNetworkReply *reply = m_net->post(req, QJsonDocument(QJsonObject{{"model", model}, {"stream", true}}).toJson(QJsonDocument::Compact));
    m_jobs.insert(id, reply);
    auto buffer = std::make_shared<QByteArray>();
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, id, model, buffer] {
        *buffer += reply->readAll();
        qsizetype nl;
        while ((nl = buffer->indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(buffer->left(nl)).object();
            buffer->remove(0, nl + 1);
            if (Item *i = find(id); i && o.contains("total") && o.value("total").toDouble() > 0) {
                i->progress = o.value("completed").toDouble() / o.value("total").toDouble();
                i->detail = QStringLiteral("Downloading %1: %2 of %3").arg(model, gb(o.value("completed").toDouble()), gb(o.value("total").toDouble()));
                publish();
            }
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, model] {
        reply->deleteLater();
        m_jobs.remove(id);
        if (reply->error() != QNetworkReply::NoError) {
            if (Item *i = find(id)) {
                i->state = QStringLiteral("error");
                i->detail = reply->error() == QNetworkReply::OperationCanceledError ? QStringLiteral("Cancelled")
                                                                                     : QStringLiteral("Couldn't download %1: %2").arg(model, reply->errorString());
                i->action = QStringLiteral("Download");
                i->progress = -1;
                publish();
            }
            return;
        }
        refresh();
    });
}

void Setup::downloadWhisper()
{
    if (m_jobs.contains(QStringLiteral("whisper"))) return;
    QDir().mkpath(mtg::whisperModelDir());
    const QString target = mtg::whisperModelDir() + QStringLiteral("/ggml-small.bin");
    auto file = std::make_shared<QFile>(target + QStringLiteral(".part"));
    if (!file->open(QIODevice::WriteOnly)) return;
    Item item = *find(QStringLiteral("whisper"));
    item.state = QStringLiteral("working");
    item.detail = QStringLiteral("Downloading the whisper model…");
    item.action = QStringLiteral("Cancel");
    item.progress = 0;
    setItem(item);
    QNetworkRequest req{QUrl(QLatin1String(kWhisperUrl))};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_net->get(req);
    m_jobs.insert(QStringLiteral("whisper"), reply);
    connect(reply, &QNetworkReply::readyRead, this, [reply, file] { file->write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (Item *i = find(QStringLiteral("whisper")); i && total > 0) {
            i->progress = double(got) / double(total);
            i->detail = QStringLiteral("Downloading the whisper model: %1 of %2").arg(gb(double(got)), gb(double(total)));
            publish();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, file, target] {
        reply->deleteLater();
        m_jobs.remove(QStringLiteral("whisper"));
        file->write(reply->readAll());
        file->close();
        if (reply->error() != QNetworkReply::NoError || file->size() < 10'000'000) {
            QFile::remove(file->fileName());
            if (Item *i = find(QStringLiteral("whisper"))) {
                i->state = QStringLiteral("error");
                i->detail = reply->error() == QNetworkReply::OperationCanceledError ? QStringLiteral("Cancelled")
                                                                                     : QStringLiteral("Couldn't download: %1").arg(reply->errorString());
                i->action = QStringLiteral("Download");
                i->progress = -1;
                publish();
            }
            return;
        }
        QFile::remove(target);
        QFile::rename(file->fileName(), target);
        refresh();
    });
}
