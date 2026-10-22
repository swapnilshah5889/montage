#include "Agent.h"

#include "Exporter.h"
#include "Project.h"
#include "ai/AdStyles.h"
#include "ai/Brand.h"
#include "ai/Finishing.h"
#include "ai/EditPlan.h"
#include "ai/EditorTools.h"
#include "ai/Generators.h"
#include "ai/Keychain.h"
#include "ai/PlanGuards.h"
#include "ai/Prompts.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QUrl>
#include <QTimer>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QUuid>
#include <cmath>

using namespace mtg;
using namespace mtg::ai;

// ================================================================ ChatModel

int ChatModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant ChatModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= m_items.size()) return {};
    const Item &it = m_items[index.row()];
    switch (role) {
    case KindRole: return it.kind;
    case TextRole: return it.text;
    case PendingRole: return it.pending;
    case ProgressRole: return it.progress;
    case DetailRole: return it.detail;
    case StateRole: return it.state;
    }
    return {};
}

QHash<int, QByteArray> ChatModel::roleNames() const
{
    return {{KindRole, "kind"},         {TextRole, "text"},     {PendingRole, "pending"},
            {ProgressRole, "progress"}, {DetailRole, "detail"}, {StateRole, "phase"}};
}

int ChatModel::add(const QString &kind, const QString &text, bool pending)
{
    const int row = int(m_items.size());
    beginInsertRows({}, row, row);
    m_items.append({kind, text, pending});
    endInsertRows();
    return row;
}

void ChatModel::appendText(int row, const QString &delta)
{
    if (row < 0 || row >= m_items.size()) return;
    m_items[row].text += delta;
    emit dataChanged(index(row), index(row), {TextRole});
}

void ChatModel::setText(int row, const QString &text)
{
    if (row < 0 || row >= m_items.size()) return;
    m_items[row].text = text;
    emit dataChanged(index(row), index(row), {TextRole});
}

void ChatModel::setPending(int row, bool pending)
{
    if (row < 0 || row >= m_items.size()) return;
    m_items[row].pending = pending;
    emit dataChanged(index(row), index(row), {PendingRole});
}

void ChatModel::setProgress(int row, double fraction, const QString &detail)
{
    if (row < 0 || row >= m_items.size()) return;
    m_items[row].progress = fraction;
    if (!detail.isEmpty()) m_items[row].detail = detail;
    emit dataChanged(index(row), index(row), {ProgressRole, DetailRole});
}

void ChatModel::setState(int row, const QString &state, const QString &detail)
{
    if (row < 0 || row >= m_items.size()) return;
    m_items[row].state = state;
    m_items[row].detail = detail;
    m_items[row].pending = state == QLatin1String("running");
    if (state == QLatin1String("done")) m_items[row].progress = 1;
    emit dataChanged(index(row), index(row), {StateRole, DetailRole, PendingRole, ProgressRole});
}

void ChatModel::clear()
{
    beginResetModel();
    m_items.clear();
    endResetModel();
}

QString ChatModel::text(int row) const
{
    return row >= 0 && row < m_items.size() ? m_items[row].text : QString();
}

// ================================================================ profiles

namespace {

QVariantMap toMap(const ModelProfile &p)
{
    return {{"id", p.id},         {"name", p.name},     {"kind", p.kind},       {"baseUrl", p.baseUrl},
            {"model", p.model},   {"vision", p.vision}, {"mode", p.mode},       {"structured", p.structured},
            {"effort", p.effort}, {"maxTokens", p.maxTokens}, {"contextTokens", p.contextTokens},
            {"local", p.local()}, {"guided", p.guided()}};
}

ModelProfile fromMap(const QVariantMap &m)
{
    ModelProfile p;
    p.id = m.value("id").toString();
    p.name = m.value("name").toString();
    p.kind = m.value("kind", "openai").toString();
    p.baseUrl = m.value("baseUrl").toString().trimmed();
    p.model = m.value("model").toString().trimmed();
    p.vision = m.value("vision").toBool();
    p.mode = m.value("mode", "auto").toString();
    p.structured = m.value("structured", "json_schema").toString();
    p.effort = m.value("effort").toString();
    p.maxTokens = m.value("maxTokens").toInt();
    p.contextTokens = m.value("contextTokens").toInt();
    return p;
}

QList<QVariantMap> presetList()
{
    return {
        {{"preset", "ollama"}, {"name", "Local · Ollama"}, {"kind", "openai"}, {"baseUrl", "http://localhost:11434/v1"},
         {"model", "qwen2.5:7b"}, {"vision", false}, {"mode", "auto"}},
        {{"preset", "lmstudio"}, {"name", "Local · LM Studio"}, {"kind", "openai"}, {"baseUrl", "http://localhost:1234/v1"},
         {"model", ""}, {"vision", false}, {"mode", "auto"}},
        {{"preset", "claude"}, {"name", "Claude"}, {"kind", "anthropic"}, {"baseUrl", "https://api.anthropic.com"},
         {"model", "claude-opus-5-5"}, {"vision", true}, {"mode", "agent"}, {"effort", "medium"}},
        {{"preset", "openai"}, {"name", "OpenAI"}, {"kind", "openai"}, {"baseUrl", "https://api.openai.com/v1"},
         {"model", ""}, {"vision", true}, {"mode", "agent"}},
        {{"preset", "deepseek"}, {"name", "DeepSeek"}, {"kind", "openai"}, {"baseUrl", "https://api.deepseek.com"},
         {"model", ""}, {"vision", false}, {"mode", "agent"}, {"structured", "json_object"}},
        {{"preset", "openrouter"}, {"name", "OpenRouter"}, {"kind", "openai"}, {"baseUrl", "https://openrouter.ai/api/v1"},
         {"model", ""}, {"vision", true}, {"mode", "agent"}},
        {{"preset", "custom"}, {"name", "Custom (OpenAI-compatible)"}, {"kind", "openai"}, {"baseUrl", ""},
         {"model", ""}, {"vision", false}, {"mode", "auto"}},
    };
}

QString keyAccount(const QString &profileId)
{
    return QStringLiteral("profile:") + profileId;
}

QString describeCall(const ToolCall &c)
{
    const QString n = c.name;
    if (n == QLatin1String("get_project")) return QStringLiteral("Reading the project");
    if (n == QLatin1String("apply_plan")) return QStringLiteral("Editing the timeline");
    if (n == QLatin1String("look_at_media")) return QStringLiteral("Looking at ") + c.input.value("media").toString();
    if (n == QLatin1String("render_preview")) return QStringLiteral("Checking the result");
    if (n == QLatin1String("find_media")) return QStringLiteral("Looking in ") + c.input.value("folder").toString();
    if (n == QLatin1String("import_media")) return QStringLiteral("Importing media");
    if (n == QLatin1String("export_video")) return QStringLiteral("Exporting the video");
    if (n == QLatin1String("generate_media")) return QStringLiteral("Generating media");
    return n;
}

QString compact(const QJsonObject &o)
{
    return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

}  // namespace

// ================================================================ Agent

Agent::Agent(Project *project, Exporter *exporter, QObject *parent)
    : QObject(parent)
    , m_project(project)
    , m_exporter(exporter)
    , m_tools(new EditorTools(project, exporter, this))
    , m_indexer(new MediaIndexer(this))
    , m_chat(new ChatModel(this))
    , m_net(new QNetworkAccessManager(this))
{
    s_instance = this;
    loadProfiles();

    // One chat row per generated file, with live progress.
    Generators *gen = m_tools->generators();
    connect(gen, &Generators::itemStarted, this, [this](int i, int count, const QString &label) {
        const int row = m_chat->add("generate", label, true);
        m_genRows.insert(i, row);
        m_chat->setState(row, "running", QStringLiteral("Starting"));
        m_chat->setProgress(row, 0, {});
        setStatus(count > 1 ? QStringLiteral("Generating %1 of %2…").arg(i + 1).arg(count) : QStringLiteral("Generating…"));
    });
    connect(gen, &Generators::itemProgress, this, [this](int i, double fraction, const QString &note) {
        m_chat->setProgress(m_genRows.value(i, -1), fraction, note);
    });
    connect(gen, &Generators::itemFinished, this, [this](int i, const GenResult &r) {
        const int row = m_genRows.value(i, -1);
        if (r.ok) m_chat->setState(row, "done", QFileInfo(r.path).fileName());
        else m_chat->setState(row, r.cancelled ? "cancelled" : "failed", r.error);
    });
    connect(gen, &Generators::finished, this, [this] { m_genRows.clear(); });
    // Watch imported media for the scene index; pause it while answering.
    connect(m_project, &Project::changed, this, [this] { QTimer::singleShot(0, this, [this] { m_indexer->watch(m_project->draft()); }); });
    connect(this, &Agent::busyChanged, this, [this] { m_indexer->setPaused(m_busy); });
    connect(m_indexer, &MediaIndexer::statusChanged, this, &Agent::mediaStatusChanged);
    connect(m_indexer, &MediaIndexer::indexed, m_project, &Project::mediaInfoChanged);
    m_indexer->watch(m_project->draft());
}

Agent::~Agent() = default;

Agent *Agent::create(QQmlEngine *, QJSEngine *)
{
    Q_ASSERT(s_instance);
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

void Agent::loadProfiles()
{
    QSettings s;
    const QJsonArray arr = QJsonDocument::fromJson(s.value("ai/profiles").toByteArray()).array();
    for (const QJsonValue &v : arr) m_profiles.append(fromMap(v.toObject().toVariantMap()));
    if (m_profiles.isEmpty()) {
        for (const QVariantMap &preset : presetList()) {
            const QString id = preset.value("preset").toString();
            if (id == QLatin1String("lmstudio") || id == QLatin1String("openrouter") || id == QLatin1String("custom")) continue;
            QVariantMap m = preset;
            m.insert("id", id);
            m_profiles.append(fromMap(m));
        }
        storeProfiles();
    }
    const QString current = s.value("ai/current", m_profiles.first().id).toString();
    m_profile = m_profiles.first();
    for (const ModelProfile &p : m_profiles)
        if (p.id == current) m_profile = p;
}

void Agent::storeProfiles() const
{
    QJsonArray arr;
    for (const ModelProfile &p : m_profiles) {
        QVariantMap m = toMap(p);
        m.remove("local");
        m.remove("guided");
        arr.append(QJsonObject::fromVariantMap(m));
    }
    QSettings s;
    s.setValue("ai/profiles", QJsonDocument(arr).toJson(QJsonDocument::Compact));
    s.setValue("ai/current", m_profile.id);
}

QVariantList Agent::profiles() const
{
    QVariantList out;
    for (const ModelProfile &p : m_profiles) {
        QVariantMap m = toMap(p);
        m.insert("hasKey", hasApiKey(p.id));
        out << m;
    }
    return out;
}

QVariantMap Agent::profileMap() const
{
    QVariantMap m = toMap(m_profile);
    m.insert("hasKey", hasApiKey(m_profile.id));
    return m;
}

QVariantList Agent::presets() const
{
    QVariantList out;
    for (const QVariantMap &p : presetList()) out << p;
    return out;
}

void Agent::setProfileId(const QString &id)
{
    if (id == m_profile.id) return;
    for (const ModelProfile &p : m_profiles) {
        if (p.id != id) continue;
        stop();
        m_profile = p;
        m_conversationStarted = false;
        storeProfiles();
        if (m_chat->rowCount() > 0) m_chat->add("activity", QStringLiteral("Switched to ") + p.name);
        emit profileChanged();
        return;
    }
}

QString Agent::saveProfile(const QVariantMap &map)
{
    ModelProfile p = fromMap(map);
    if (p.id.isEmpty()) p.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    if (p.name.trimmed().isEmpty()) p.name = p.model.isEmpty() ? QStringLiteral("Model") : p.model;
    bool replaced = false;
    for (ModelProfile &existing : m_profiles) {
        if (existing.id == p.id) {
            existing = p;
            replaced = true;
        }
    }
    if (!replaced) m_profiles.append(p);
    if (p.id == m_profile.id) {
        m_profile = p;
        m_conversationStarted = false;
        if (m_provider) m_provider->setProfile(m_profile, apiKeyFor(m_profile));
    }
    storeProfiles();
    emit profilesChanged();
    emit profileChanged();
    return p.id;
}

void Agent::deleteProfile(const QString &id)
{
    if (m_profiles.size() <= 1) return;
    m_profiles.removeIf([&](const ModelProfile &p) { return p.id == id; });
    Keychain::remove(keyAccount(id));
    if (m_profile.id == id) {
        m_profile = m_profiles.first();
        m_conversationStarted = false;
    }
    storeProfiles();
    emit profilesChanged();
    emit profileChanged();
}

void Agent::setApiKey(const QString &id, const QString &key)
{
    Keychain::store(keyAccount(id), key.trimmed());
    if (id == m_profile.id && m_provider) m_provider->setProfile(m_profile, apiKeyFor(m_profile));
    emit profilesChanged();
    emit profileChanged();
}

bool Agent::hasApiKey(const QString &id) const
{
    return Keychain::has(keyAccount(id));
}

QString Agent::apiKeyFor(const ModelProfile &p) const
{
    const QString stored = Keychain::load(keyAccount(p.id));
    if (!stored.isEmpty()) return stored;
    // Developer fallback: the usual environment variables.
    const QString host = QUrl(p.baseUrl).host();
    const char *env = p.kind == QLatin1String("anthropic")    ? "ANTHROPIC_API_KEY"
                      : host.contains(QLatin1String("openai.com"))   ? "OPENAI_API_KEY"
                      : host.contains(QLatin1String("deepseek"))     ? "DEEPSEEK_API_KEY"
                      : host.contains(QLatin1String("openrouter"))   ? "OPENROUTER_API_KEY"
                                                                     : "MONTAGE_API_KEY";
    return qEnvironmentVariable(env);
}

void Agent::fetchModels(const QString &id)
{
    for (const ModelProfile &p : m_profiles) {
        if (p.id != id) continue;
        mtg::ai::fetchModels(m_net, p, apiKeyFor(p), [this, id](const QStringList &models, const QString &error) {
            emit modelsFetched(id, models, error);
        });
        return;
    }
}

void Agent::setBusy(bool busy)
{
    if (busy == m_busy) return;
    m_busy = busy;
    emit busyChanged();
}

void Agent::setStatus(const QString &status)
{
    if (status == m_status) return;
    m_status = status;
    emit statusChanged();
}

void Agent::ensureProvider()
{
    if (m_provider && m_providerKind != m_profile.kind) {
        delete m_provider;
        m_provider = nullptr;
    }
    if (!m_provider) {
        m_provider = ChatProvider::create(m_profile.kind, this);
        m_providerKind = m_profile.kind;
        m_conversationStarted = false;
        connect(m_provider, &ChatProvider::textDelta, this, [this](const QString &delta) {
            if (!m_busy) return;
            if (m_assistantRow < 0) m_assistantRow = m_chat->add("assistant", {}, true);
            m_chat->appendText(m_assistantRow, delta);
        });
        connect(m_provider, &ChatProvider::progressNote, this, [this](const QString &note) {
            if (m_busy) m_chat->add("activity", note);
        });
        connect(m_provider, &ChatProvider::turnFinished, this, &Agent::onTurnFinished);
        connect(m_provider, &ChatProvider::jsonFinished, this, &Agent::onPlan);
    }
    m_provider->setProfile(m_profile, apiKeyFor(m_profile));
}

QString Agent::friendlyError(const QString &error) const
{
    // A local model server that isn't running is the common case: say what to do.
    const bool unreachable = error.contains(QLatin1String("refused"), Qt::CaseInsensitive)
                             || error.contains(QLatin1String("not found"), Qt::CaseInsensitive)
                             || error.contains(QLatin1String("timed out"), Qt::CaseInsensitive)
                             || error.contains(QLatin1String("Host"), Qt::CaseInsensitive);
    if (unreachable && m_profile.local())
        return QStringLiteral("Can't reach %1 at %2. Open the Ollama app (or run \"ollama serve\"), then try again. "
                              "⚙ → Set up shows what's missing.").arg(m_profile.name, m_profile.baseUrl);
    if (error.contains(QLatin1String("model"), Qt::CaseInsensitive) && error.contains(QLatin1String("not found"), Qt::CaseInsensitive))
        return QStringLiteral("%1 isn't installed. Download it from ⚙ → Set up.").arg(m_profile.model);
    return error;
}

QString Agent::mediaStatus() const
{
    return m_indexer->status();
}

bool Agent::waitForMedia(int timeoutMs)
{
    m_indexer->watch(m_project->draft());
    QElapsedTimer timer;
    timer.start();
    while (m_indexer->busy() && timer.elapsed() < timeoutMs) QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
    return !m_indexer->busy();
}

QVariantMap Agent::brand() const
{
    return loadBrandKit().toMap();
}

void Agent::setBrand(const QVariantMap &brand)
{
    BrandKit kit = BrandKit::fromMap(brand);
    if (kit.toMap() == loadBrandKit().toMap()) return;
    saveBrandKit(kit);
    emit brandChanged();
}

QStringList Agent::adStyles() const
{
    return adStyleNames();
}

void Agent::send(const QString &text)
{
    const QString request = text.trimmed();
    if (m_busy || request.isEmpty()) return;
    m_chat->add("user", request);
    if (m_profile.model.isEmpty()) {
        m_chat->add("error", QStringLiteral("Choose a model for %1 in Settings first.").arg(m_profile.name));
        return;
    }
    if (!m_profile.local() && apiKeyFor(m_profile).isEmpty()) {
        m_chat->add("error", QStringLiteral("Add an API key for %1 in Settings.").arg(m_profile.name));
        return;
    }

    setBusy(true);
    m_project->beginUndoGroup();
    m_request = request;
    m_rounds = 0;
    m_genFailed.clear();
    m_lastGood = {};
    m_assistantRow = -1;
    ensureProvider();

    m_tools->setRequest(request);
    const BrandKit kit = loadBrandKit();
    if (brandApplies(request, kit) && !kit.logo.isEmpty() && QFileInfo::exists(kit.logo))
        m_project->importFiles({QUrl::fromLocalFile(kit.logo)});  // no-op once imported

    const QStringList imported = m_tools->importMentionedPaths(request);
    if (!imported.isEmpty())
        m_chat->add("activity", QStringLiteral("Imported %1 file(s): %2").arg(imported.size()).arg(imported.join(", ")));

    if (m_profile.guided()) startGuided(request);
    else startAgentTurn(request);
}

void Agent::stop()
{
    if (!m_busy) return;
    if (m_provider) m_provider->cancel();
    if (m_exporter->running()) m_exporter->cancel();
    m_tools->cancelGeneration();
    m_chat->add("activity", QStringLiteral("Stopped."));
    // A stopped tool loop leaves the conversation without results for its
    // last calls; start the next request in a fresh conversation.
    m_conversationStarted = false;
    finish(false, {});
}

void Agent::newChat()
{
    stop();
    m_chat->clear();
    m_history.clear();
    m_conversationStarted = false;
}

void Agent::finish(bool ok, const QString &reply)
{
    if (m_assistantRow >= 0) m_chat->setPending(m_assistantRow, false);
    m_assistantRow = -1;
    m_project->endUndoGroup();
    setStatus({});
    setBusy(false);
    emit finished(ok, reply);
}

// ---------------------------------------------------------------- agent mode

void Agent::startAgentTurn(const QString &text)
{
    if (!m_conversationStarted) {
        m_provider->reset(agentSystemPrompt(m_profile.vision), m_tools->specs(m_profile.vision));
        m_conversationStarted = true;
    }
    m_provider->addUserMessage(text + QStringLiteral("\n\n<project_state>\n") + m_tools->projectState(true)
                               + QStringLiteral("\n</project_state>"));
    setStatus(QStringLiteral("Thinking…"));
    m_provider->run();
}

void Agent::onTurnFinished(const TurnResult &r)
{
    if (!m_busy) return;
    if (m_assistantRow >= 0) m_chat->setPending(m_assistantRow, false);
    m_assistantRow = -1;

    switch (r.stop) {
    case StopReason::Error:
        m_chat->add("error", friendlyError(r.error));
        m_conversationStarted = false;
        finish(false, {});
        return;
    case StopReason::Refusal:
        m_chat->add("error", QStringLiteral("The model declined this request."));
        finish(false, {});
        return;
    case StopReason::MaxTokens:
        m_chat->add("error", QStringLiteral("The model's reply was cut off before it finished. Try a shorter request."));
        m_conversationStarted = false;
        finish(false, {});
        return;
    case StopReason::Cancelled:
        finish(false, {});
        return;
    case StopReason::ToolUse:
        if (++m_rounds > 30) {
            m_chat->add("error", QStringLiteral("Stopped after 30 steps without finishing."));
            m_conversationStarted = false;
            finish(false, {});
            return;
        }
        runTools(r.calls, {});
        return;
    case StopReason::EndTurn:
        finish(true, r.text);
        return;
    }
}

void Agent::runTools(QList<ToolCall> calls, QList<ToolResult> done)
{
    if (!m_busy) return;
    if (calls.isEmpty()) {
        m_provider->addToolResults(done);
        setStatus(QStringLiteral("Thinking…"));
        m_provider->run();
        return;
    }
    const ToolCall call = calls.takeFirst();
    m_chat->add("activity", describeCall(call));
    setStatus(describeCall(call) + QStringLiteral("…"));
    m_tools->run(call, m_profile.vision, [this, calls, done](const ToolResult &result) mutable {
        if (!m_busy) return;
        done.append(result);
        runTools(calls, done);
    });
}

// ---------------------------------------------------------------- guided mode

void Agent::startGuided(const QString &)
{
    // Even with no media the model can make text videos, or explain what it needs.
    m_attempts = 0;
    m_lastRaw.clear();
    requestPlan({});
}

void Agent::requestPlan(const QString &feedback)
{
    const Draft &d = m_project->draft();
    const MediaIndex index(d);
    QString user = QStringLiteral("MEDIA:\n%1\n\nCURRENT PLAN:\n%2\n(video length now: %3 seconds)\n")
                       .arg(describeMedia(d, index), compact(planFromDraft(d, index)))
                       .arg(std::round(d.duration() / 1e5) / 10);
    if (!m_history.isEmpty()) {
        user += QStringLiteral("\nEARLIER IN THIS CHAT:\n");
        for (qsizetype i = std::max<qsizetype>(0, m_history.size() - 4); i < m_history.size(); ++i)
            user += QStringLiteral("- user: %1\n  you: %2\n").arg(m_history[i].first, m_history[i].second);
    }
    user += QStringLiteral("\nREQUEST: %1\n").arg(m_request);
    if (!feedback.isEmpty()) {
        user += QStringLiteral("\nYOUR LAST ANSWER:\n%1\n\nPROBLEMS TO FIX:\n%2\n\nAnswer again with the full corrected plan.\n")
                    .arg(m_lastRaw, feedback);
    }
    user += QStringLiteral("ANSWER:\n");

    const QList<QByteArray> images = m_profile.vision ? m_tools->mediaSheets(4) : QList<QByteArray>{};
    setStatus(feedback.isEmpty() ? QStringLiteral("Planning the edit…") : QStringLiteral("Fixing the plan…"));
    m_provider->requestJson(guidedSystemPrompt(), user, planSchema(), images);
}

void Agent::onPlan(const QJsonObject &modelPlan, const QString &raw, const QString &error)
{
    if (!m_busy) return;
    if (!raw.isEmpty()) m_lastRaw = raw;
    if (!error.isEmpty()) {
        if (raw.isEmpty()) {  // the request itself failed
            m_chat->add("error", friendlyError(error));
            finish(false, {});
            return;
        }
        if (++m_attempts <= 2) {
            requestPlan(QStringLiteral("Your answer was not a valid JSON object (%1). Reply with only the JSON plan.").arg(error));
            return;
        }
        if (fallBackToLastGood()) return;
        m_chat->add("error", QStringLiteral("The model didn't produce a usable plan. Try rephrasing, or a larger model."));
        finish(false, {});
        return;
    }

    const Draft &current = m_project->draft();
    const MediaIndex index(current);
    const QJsonObject currentPlan = planFromDraft(current, index);

    // Deterministic guards first: keep what the request didn't touch, and
    // hit a requested length exactly. Re-asking small models for either
    // rarely works.
    QJsonObject plan = modelPlan;
    if (qEnvironmentVariableIsSet("MONTAGE_DEBUG")) qInfo("model plan: %s", qPrintable(compact(modelPlan)));
    QStringList notes;
    const QStringList kept = keepUnrequested(m_request, currentPlan, current, index, &plan);
    if (!kept.isEmpty()) notes << QStringLiteral("kept %1 unchanged").arg(kept.join(", "));
    notes += requestedGeneration(m_request, currentPlan, current, index, &plan);
    if (!m_genFailed.isEmpty()) {  // a backend that failed this request isn't asked again on a fix-up round
        QJsonArray items;
        for (const QJsonValue &v : plan.value("generate").toArray())
            if (!m_genFailed.contains(v.toObject().value("kind").toString())) items.append(v);
        plan.insert("generate", items);
        notes.removeIf([&](const QString &n) {
            return std::any_of(m_genFailed.cbegin(), m_genFailed.cend(), [&](const QString &k) { return n.contains(k); });
        });
    }
    if (plan.value("generate").toArray().isEmpty()) {
        bindGenerated({}, current, &plan);  // drops references to media nobody is making
        buildPlan(modelPlan, plan, notes, {});
        return;
    }
    // Make the media first (the same request reuses its files), then build.
    m_tools->resolveGenerated(plan, [this, modelPlan, notes](const GenResolution &r) mutable {
        if (!m_busy) return;
        if (r.cancelled) {
            finish(false, {});
            return;
        }
        for (const QString &e : r.errors) m_chat->add("error", e);
        m_genFailed += r.failedKinds;
        notes.removeIf([&](const QString &n) {  // no "added a generated image" when there is none
            return std::any_of(r.failedKinds.cbegin(), r.failedKinds.cend(), [&](const QString &k) { return n.contains(k); });
        });
        buildPlan(modelPlan, r.plan, notes, r.generated);
    });
}

void Agent::buildPlan(const QJsonObject &modelPlan, QJsonObject plan, QStringList notes, const QStringList &generated)
{
    const Draft &current = m_project->draft();
    const MediaIndex index(current);
    const QJsonObject currentPlan = planFromDraft(current, index);
    notes += placeGenerated(generated, m_request, current, index, &plan);
    // A colour the request names wins over the model's take on it ("dark blue" is #00008B,
    // not the model's navy), unless the request gives an exact code.
    const QString bg = requestedBackground(m_request);
    static const QRegularExpression hexCode(QStringLiteral("#[0-9a-fA-F]{3,8}\\b"));
    if (!bg.isEmpty() && parseColor(plan.value("background").toString()) != bg && !hexCode.match(m_request).hasMatch()) {
        plan.insert("background", bg);
        notes << QStringLiteral("set the background to %1").arg(bg);
    }
    notes += resolveShots(current, index, &plan);  // "shot": 2 / "moment": "the dog" -> exact ranges
    notes += arrangeLines(m_request, current, index, &plan);
    notes += resolveLines(current, index, &plan);  // "line": 3 / "says": "free delivery" -> that sentence
    notes += applySpeechRequests(m_request, current, index, &plan);
    notes += applyNamedShots(m_request, current, index, &plan);
    const double wanted = requestedLength(m_request);
    if (wanted > 0 && fitToLength(wanted, current, index, &plan))
        notes << QStringLiteral("fitted the clips to %1 seconds").arg(wanted);
    notes += pickBestMoments(m_request, current, index, &plan);
    if (addAdMusic(m_request, currentPlan, current, index, &plan)) notes << QStringLiteral("added music under the ad");
    if (duckUnderVoiceover(m_request, current, index, &plan)) notes << QStringLiteral("the music dips while the voiceover speaks");
    if (snapEndTexts(m_request, currentPlan, &plan)) notes << QStringLiteral("moved the end text to finish with the video");
    notes += applyObviousFeatures(m_request, current, index, &plan);
    notes += applySmartRequests(m_request, current, index, &plan);
    if (const QStringList formats = requestedFormats(m_request); !formats.isEmpty()) {
        plan.insert("export", true);
        plan.insert("export_formats", QJsonArray::fromStringList(formats));
    }
    notes += applyBrand(m_request, loadBrandKit(), currentPlan, current, index, &plan);
    notes += applyAdStyle(currentPlan, current, index, &plan);

    PlanReport report;
    const Draft next = draftFromPlan(current, plan, index, &report);
    const QString feedback = report.ok() ? planFeedback(currentPlan, plan, next.duration() / 1e6) : report.text();
    if (!feedback.isEmpty() && m_attempts < 2) {
        ++m_attempts;
        if (report.ok()) m_lastGood = {modelPlan, plan, notes + report.warnings, next, true};
        QString first;
        for (const QString &line : feedback.split('\n')) {
            if (line.trimmed().isEmpty() || line.endsWith(':')) continue;
            first = line.startsWith(QLatin1String("- ")) ? line.mid(2) : line;
            break;
        }
        m_chat->add("activity", QStringLiteral("Checked the plan, asking the model to fix: ") + first);
        requestPlan(feedback);
        return;
    }
    if (!report.ok()) {
        if (fallBackToLastGood()) return;
        m_chat->add("error", QStringLiteral("Couldn't apply the plan:\n") + report.errors.join('\n'));
        finish(false, {});
        return;
    }
    finishBuilt(modelPlan, plan, notes + report.warnings, next);
}

bool Agent::fallBackToLastGood()
{
    if (!m_lastGood.valid) return false;
    const GoodPlan good = m_lastGood;
    m_lastGood = {};
    m_chat->add("activity", QStringLiteral("The model's fix didn't work out, so its earlier plan was used."));
    finishBuilt(good.modelPlan, good.plan, good.notes, good.draft);
    return true;
}

void Agent::finishBuilt(const QJsonObject &modelPlan, const QJsonObject &plan, const QStringList &notes, const Draft &next)
{
    // Steps that need the media itself (cutting to the beat, captions) run off the UI thread.
    if (needsFinishing(next)) {
        setStatus(next.captions.isEmpty() ? QStringLiteral("Cutting to the beat…") : QStringLiteral("Listening for captions…"));
        m_tools->finishAsync(next, [this, modelPlan, plan, notes](const Draft &done, QStringList more, QStringList problems) {
            if (!m_busy) return;
            for (const QString &p : problems) m_chat->add("activity", p);
            applyBuilt(modelPlan, plan, notes + more, done);
        });
        return;
    }
    applyBuilt(modelPlan, plan, notes, next);
}

void Agent::applyBuilt(const QJsonObject &modelPlan, const QJsonObject &plan, const QStringList &notes, const Draft &next)
{
    m_project->applyDraft(next);
    QString reply = plan.value("reply").toString().trimmed();
    if (reply.isEmpty()) reply = QStringLiteral("Done.");
    // The app may have changed the style the model chose (brand kit); keep the reply truthful.
    const QString modelStyle = modelPlan.value("style").toString().trimmed().toLower();
    if (!modelStyle.isEmpty() && !next.style.isEmpty() && modelStyle != next.style)
        reply.replace(QRegularExpression(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(modelStyle)),
                                         QRegularExpression::CaseInsensitiveOption), next.style);
    m_chat->add("assistant", reply);
    if (!notes.isEmpty()) m_chat->add("activity", QStringLiteral("Adjusted: ") + notes.join("; "));
    if (const QString offer = musicOffer(m_request, plan, m_project->draft(), MediaIndex(m_project->draft())); !offer.isEmpty())
        m_chat->add("activity", offer);
    m_history.append({m_request, reply});

    if (plan.value("export").toBool()) {
        setStatus(QStringLiteral("Exporting…"));
        QStringList formats;
        for (const QJsonValue &v : plan.value("export_formats").toArray()) formats << v.toString();
        m_tools->exportVideo(m_project->name(), formats, [this, reply](bool ok, const QString &msg) {
            m_chat->add(ok ? "activity" : "error", ok ? QStringLiteral("Exported to ") + msg : msg);
            finish(ok, reply);
        });
        return;
    }
    finish(true, reply);
}

// Deterministic review of a valid plan against the request: catches the
// mistakes small models make that need the model's judgement to fix
// (dropping clips, missing requested text, music or aspect).
QString Agent::planFeedback(const QJsonObject &current, const QJsonObject &plan, double newLength) const
{
    QStringList problems;
    const QString req = m_request.toLower();
    const Draft &d = m_project->draft();
    const MediaIndex index(d);

    // Which media the plan uses, by id.
    QSet<QString> used;
    for (const char *key : {"main", "overlays", "audio"})
        for (const QJsonValue &v : plan.value(QLatin1String(key)).toArray()) {
            const QString ref = v.toObject().value("media").toString().trimmed();
            QString id = index.materialId(ref);
            if (id.isEmpty())
                for (const QString &mid : index.materialIds())
                    if (d.material(mid)->name.compare(ref, Qt::CaseInsensitive) == 0) id = mid;
            if (!id.isEmpty()) used.insert(index.alias(id));
        }

    // Files the request names ("the logo", "the song", "beach") must appear,
    // unless the request is about removing them.
    static const QRegularExpression removing(QStringLiteral(R"(\b(remove|delete|drop|without|rid|except|no|mute|take out|cut out)\b)"));
    if (!removing.match(req).hasMatch()) {
        for (const QString &id : index.materialIds()) {
            const Material *m = d.material(id);
            const QString stem = QFileInfo(m->name).completeBaseName().toLower();
            if (stem.size() < 3 || !QRegularExpression(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(stem))).match(req).hasMatch())
                continue;
            const QString alias = index.alias(id);
            if (used.contains(alias)) continue;
            const QString where = m->kind == MaterialKind::Audio ? QStringLiteral("\"audio\"")
                                  : m->kind == MaterialKind::Image ? QStringLiteral("\"main\" (as its own clip) or \"overlays\"")
                                                                   : QStringLiteral("\"main\"");
            problems << QStringLiteral("The request mentions %1 (%2) but your plan does not use %2. Add it to %3.")
                            .arg(m->name, alias, where);
        }
    }

    // "both clips", "all three clips", "all my photos": every one of them must be in the video.
    static const QRegularExpression both(QStringLiteral(R"(\bboth\b)"));
    static const QRegularExpression all(QStringLiteral(
        R"(\ball\s+(?:of\s+)?(?:my\s+|the\s+|these\s+|two\s+|three\s+|four\s+|five\s+)?(clips|videos|shots|footage|photos|pictures|images|media|files)\b|\b(all of them|everything|every clip)\b)"));
    const auto allMatch = all.match(req);
    if (!removing.match(req).hasMatch() && (both.match(req).hasMatch() || allMatch.hasMatch())) {
        const bool photos = allMatch.captured(1).contains(QRegularExpression(QStringLiteral("photos|pictures|images")));
        QStringList candidates, missing;
        for (const QString &id : index.materialIds()) {
            const Material *m = d.material(id);
            const bool fits = photos ? m->kind == MaterialKind::Image : m->kind == MaterialKind::Video;
            if (!fits) continue;
            candidates << index.alias(id);
            if (!used.contains(index.alias(id))) missing << index.alias(id);
        }
        if (candidates.size() >= 2 && !missing.isEmpty())
            problems << QStringLiteral("The user asked for %1, so \"main\" must use every one of: %2. Missing: %3.")
                            .arg(allMatch.hasMatch() ? QStringLiteral("all of them") : QStringLiteral("both"),
                                 candidates.join(", "), missing.join(", "));
    }

    // A requested length the chosen clips can't physically reach.
    const double wanted = requestedLength(m_request);
    if (wanted > 0 && newLength < wanted - 0.5) {
        QStringList unused;
        for (const QString &id : index.materialIds()) {
            const Material *m = d.material(id);
            if (m->kind == MaterialKind::Video && !used.contains(index.alias(id)))
                unused << QStringLiteral("%1 (%2s)").arg(index.alias(id)).arg(std::round(m->durationUs / 1e5) / 10);
        }
        if (!unused.isEmpty())
            problems << QStringLiteral("The user asked for a %1 second video, but the clips you chose are only %2 seconds long "
                                       "in total. Add more clips to \"main\" after the ones you have, from: %3.")
                            .arg(wanted)
                            .arg(std::round(newLength * 10) / 10)
                            .arg(unused.join(", "));
    }

    const int before = int(current.value("main").toArray().size());
    const int after = int(plan.value("main").toArray().size());
    static const QRegularExpression removal(QStringLiteral(
        R"(\b(remove|delete|drop|only|just|keep|cut|trim|short|shorter|shorten|replace|start over|from scratch|fewer|less|best|highlight|without|instead|swap|new video|make a)\b)"));
    if (before >= 2 && after < (before + 1) / 2 && !removal.match(req).hasMatch())
        problems << QStringLiteral("You removed %1 of the %2 clips in \"main\", but the request did not ask to remove clips. "
                                   "Keep every clip from CURRENT PLAN unless the request says to remove it.")
                        .arg(before - after)
                        .arg(before);

    const QString aspect = plan.value("aspect").toString();
    const auto wants = [&](const char *pattern) { return QRegularExpression(QLatin1String(pattern)).match(req).hasMatch(); };
    if (wants(R"(\b(vertical|portrait|tiktok|reels?|shorts|9:16|story|stories)\b)") && aspect != QLatin1String("9:16")
        && current.value("aspect").toString() != QLatin1String("9:16"))
        problems << QStringLiteral("The user wants a vertical video: set \"aspect\" to \"9:16\".");
    else if (wants(R"(\b(square|1:1|instagram post)\b)") && aspect != QLatin1String("1:1")
             && current.value("aspect").toString() != QLatin1String("1:1"))
        problems << QStringLiteral("The user wants a square video: set \"aspect\" to \"1:1\".");
    else if (wants(R"(\b(landscape|horizontal|youtube|16:9|widescreen)\b)") && aspect != QLatin1String("16:9")
             && current.value("aspect").toString() != QLatin1String("16:9"))
        problems << QStringLiteral("The user wants a landscape video: set \"aspect\" to \"16:9\".");

    // Quoted text the user wants on screen.
    if (wants(R"(\b(title|text|caption|subtitle|heading|says|say|write|label|lower third|end card)\b)")) {
        static const QRegularExpression quoted(QStringLiteral("[\"“]([^\"”]{1,80})[\"”]"));
        QString allText;
        for (const QJsonValue &t : plan.value("texts").toArray()) allText += t.toObject().value("text").toString().toLower() + '\n';
        const QString spoken = voiceoverScript(m_request);  // heard, not shown
        for (auto it = quoted.globalMatch(m_request); it.hasNext();) {
            const QString q = it.next().captured(1).trimmed();
            if (!q.isEmpty() && q != spoken && !allText.contains(q.toLower()) && !req.contains(QStringLiteral("remove")))
                problems << QStringLiteral("The request asks for the text \"%1\" but no item in \"texts\" has it.").arg(q);
        }
    }

    // Offers and calls to action the user wants on screen, even unquoted.
    {
        QString allText;
        for (const QJsonValue &t : plan.value("texts").toArray()) allText += t.toObject().value("text").toString().toLower() + '\n';
        static const QRegularExpression offer(QStringLiteral(
            R"((\$\s?\d+(?:[.,]\d{1,2})?|\d{1,3}\s?%(?:\s?off)?|\b(?:shop|buy|order|book|sign up|subscribe|download|learn more|get yours|join)(?: it| one)? now\b|\blink in bio\b))"),
            QRegularExpression::CaseInsensitiveOption);
        const bool removingText = wants(R"(\b(remove|delete|without|get rid|take (it |them )?(off|out))\b)");
        for (auto it = offer.globalMatch(m_request); it.hasNext() && !removingText;) {
            const QString want = it.next().captured(1).trimmed();
            QString loose = want.toLower();
            loose.remove(QLatin1Char(' '));
            QString have = allText;
            have.remove(QLatin1Char(' '));
            if (!have.contains(loose))
                problems << QStringLiteral("The request asks for \"%1\" on screen but no item in \"texts\" has it. Add a text with it "
                                           "(style \"price\" or \"badge\" for prices and discounts, \"cta\" for a call to action like Shop now).")
                                .arg(want);
        }
    }

    if (wants(R"(\b(music|song|soundtrack|track|background audio|beat)\b)")
        && !wants(R"(\b(remove|delete|no|without|mute|rid|drop|lose|stop|cut|take out|turn off|silence)\b)")
        && plan.value("audio").toArray().isEmpty()) {
        bool haveAudio = false;
        const Draft &d = m_project->draft();
        const MediaIndex index(d);  // keep alive for the loop below
        for (const QString &id : index.materialIds()) haveAudio |= d.material(id)->kind == MaterialKind::Audio;
        if (haveAudio)
            problems << QStringLiteral("The user asked for music: put the audio file (type audio in MEDIA) in \"audio\".");
    }
    problems += missingFeatures(m_request, plan);
    problems += missingShots(m_request, plan, d, index);
    problems += missingLines(m_request, plan, d, index);
    return problems.join('\n');
}
