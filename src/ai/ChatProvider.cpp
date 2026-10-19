#include "ChatProvider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

namespace mtg::ai {

// ---------------------------------------------------------------- profile

bool ModelProfile::local() const
{
    const QString host = QUrl(baseUrl).host().toLower();
    return host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") || host == QLatin1String("::1")
           || host.endsWith(QLatin1String(".local"));
}

bool ModelProfile::guided() const
{
    if (mode == QLatin1String("guided")) return true;
    if (mode == QLatin1String("agent")) return false;
    return local();  // auto: small local models get the guided planner
}

// ---------------------------------------------------------------- helpers

namespace {

constexpr int kTimeoutMs = 10 * 60 * 1000;

QString endpoint(const QString &base, const QString &path)
{
    QString b = base.trimmed();
    while (b.endsWith(QLatin1Char('/'))) b.chop(1);
    return b + path;
}

QString errorFromBody(int status, const QByteArray &body, const QString &netError)
{
    const QJsonObject o = QJsonDocument::fromJson(body).object();
    QString msg = o.value("error").toObject().value("message").toString();
    if (msg.isEmpty()) msg = o.value("error").toString();
    if (msg.isEmpty()) msg = o.value("message").toString();
    if (msg.isEmpty()) msg = QString::fromUtf8(body.left(300)).trimmed();
    if (msg.isEmpty()) msg = netError;
    return status > 0 ? QStringLiteral("HTTP %1: %2").arg(status).arg(msg) : msg;
}

// Models often wrap JSON in prose or code fences; take the outermost object.
QJsonObject extractJsonObject(const QString &text, QString *error)
{
    const int start = text.indexOf(QLatin1Char('{'));
    const int end = text.lastIndexOf(QLatin1Char('}'));
    if (start < 0 || end <= start) {
        *error = QStringLiteral("The reply did not contain a JSON object.");
        return {};
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(text.mid(start, end - start + 1).toUtf8(), &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = QStringLiteral("Invalid JSON: ") + pe.errorString();
        return {};
    }
    return doc.object();
}

QString b64(const QByteArray &jpeg)
{
    return QString::fromLatin1(jpeg.toBase64());
}

// Server-sent events over one POST. Emits each event as it completes.
class SseRequest : public QObject
{
    Q_OBJECT
public:
    SseRequest(QNetworkAccessManager *net, const QNetworkRequest &req, const QByteArray &body, QObject *parent)
        : QObject(parent)
    {
        m_reply = net->post(req, body);
        connect(m_reply, &QNetworkReply::readyRead, this, &SseRequest::onReadyRead);
        connect(m_reply, &QNetworkReply::finished, this, &SseRequest::onFinished);
    }
    ~SseRequest() override
    {
        if (m_reply) {
            m_reply->disconnect(this);
            m_reply->abort();
            m_reply->deleteLater();
        }
    }
    void abort()
    {
        if (m_reply) m_reply->abort();
    }

signals:
    void event(const QByteArray &name, const QByteArray &data);
    void done(int status, const QByteArray &errorBody, const QString &networkError);

private:
    int status() const { return m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(); }

    void onReadyRead()
    {
        const QByteArray chunk = m_reply->readAll();
        if (status() >= 400) {
            m_errorBody += chunk;
            return;
        }
        m_buf += chunk;
        m_buf.replace("\r\n", "\n");
        for (int sep = m_buf.indexOf("\n\n"); sep >= 0; sep = m_buf.indexOf("\n\n")) {
            const QByteArray block = m_buf.left(sep);
            m_buf.remove(0, sep + 2);
            QByteArray name, data;
            for (const QByteArray &line : block.split('\n')) {
                if (line.startsWith("event:")) name = line.mid(6).trimmed();
                else if (line.startsWith("data:")) data += (data.isEmpty() ? QByteArray() : QByteArray("\n")) + line.mid(5).trimmed();
            }
            if (!data.isEmpty() || !name.isEmpty()) emit event(name, data);
        }
    }
    void onFinished()
    {
        if (!m_reply) return;
        const int st = status();
        if (st >= 400) m_errorBody += m_reply->readAll();
        const QString net = m_reply->error() == QNetworkReply::NoError ? QString() : m_reply->errorString();
        m_reply->deleteLater();
        m_reply = nullptr;
        emit done(st, m_errorBody, net);
    }

    QPointer<QNetworkReply> m_reply;
    QByteArray m_buf;
    QByteArray m_errorBody;
};

QNetworkRequest jsonRequest(const QString &url)
{
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(kTimeoutMs);
    return req;
}

// ================================================= OpenAI-compatible adapter
// OpenAI, DeepSeek, OpenRouter, Groq, Together, Ollama, LM Studio, llama.cpp,
// vLLM ... all accept /chat/completions with function tools.

class OpenAiProvider : public ChatProvider
{
public:
    explicit OpenAiProvider(QObject *parent)
        : ChatProvider(parent)
        , m_net(new QNetworkAccessManager(this))
    {
    }

    void setProfile(const ModelProfile &p, const QString &key) override
    {
        m_p = p;
        m_key = key;
    }

    void reset(const QString &system, const QList<ToolSpec> &tools) override
    {
        cancel();
        m_system = system;
        m_messages = {};
        m_tools = {};
        for (const ToolSpec &t : tools) {
            m_tools.append(QJsonObject{{"type", "function"},
                                       {"function", QJsonObject{{"name", t.name},
                                                                {"description", t.description},
                                                                {"parameters", t.schema}}}});
        }
    }

    void addUserMessage(const QString &text, const QList<QByteArray> &jpegs) override
    {
        m_messages.append(QJsonObject{{"role", "user"}, {"content", userContent(text, jpegs)}});
    }

    void addToolResults(const QList<ToolResult> &results) override
    {
        QList<QPair<QString, QByteArray>> images;
        for (const ToolResult &r : results) {
            QString text = r.isError ? QStringLiteral("ERROR: ") + r.text : r.text;
            if (!r.jpegs.isEmpty() && !m_p.vision) text += QStringLiteral("\n(images omitted: this model cannot view images)");
            m_messages.append(QJsonObject{{"role", "tool"}, {"tool_call_id", r.callId}, {"content", text}});
            if (m_p.vision)
                for (const QByteArray &j : r.jpegs) images.append({r.name + QLatin1Char(' ') + r.callId, j});
        }
        // Chat Completions tool messages are text-only; images follow as one user message.
        if (!images.isEmpty()) {
            QJsonArray parts{QJsonObject{{"type", "text"}, {"text", "Images returned by the tool calls above, in order."}}};
            for (const auto &[label, jpeg] : images) {
                parts.append(QJsonObject{{"type", "text"}, {"text", label}});
                parts.append(QJsonObject{{"type", "image_url"},
                                         {"image_url", QJsonObject{{"url", "data:image/jpeg;base64," + b64(jpeg)}}}});
            }
            m_messages.append(QJsonObject{{"role", "user"}, {"content", parts}});
        }
    }

    void run() override
    {
        cancel();
        QJsonArray messages{QJsonObject{{"role", "system"}, {"content", m_system}}};
        for (const QJsonValue &m : m_messages) messages.append(m);
        QJsonObject body{{"model", m_p.model}, {"messages", messages}, {"stream", true}};
        if (!m_tools.isEmpty()) body.insert("tools", m_tools);
        if (m_p.maxTokens > 0) body.insert("max_tokens", m_p.maxTokens);
        if (m_p.local()) body.insert("temperature", 0.2);

        m_text.clear();
        m_pending.clear();
        m_finish.clear();
        m_streamError.clear();
        m_sse = new SseRequest(m_net, request("/chat/completions"), QJsonDocument(body).toJson(QJsonDocument::Compact), this);
        connect(m_sse, &SseRequest::event, this, [this](const QByteArray &, const QByteArray &data) { onEvent(data); });
        connect(m_sse, &SseRequest::done, this, [this](int st, const QByteArray &err, const QString &net) { onDone(st, err, net); });
    }

    void requestJson(const QString &system, const QString &user, const QJsonObject &schema,
                     const QList<QByteArray> &jpegs) override
    {
        cancel();
        m_jsonArgs = {system, user, schema};
        m_jsonImages = jpegs;
        sendJson(m_p.structured);
    }

    void cancel() override
    {
        if (m_sse) {
            m_sse->disconnect(this);
            m_sse->deleteLater();
            m_sse = nullptr;
        }
        if (m_jsonReply) {
            m_jsonReply->disconnect(this);
            m_jsonReply->abort();
            m_jsonReply->deleteLater();
            m_jsonReply = nullptr;
        }
    }

private:
    struct PendingCall {
        QString id, name, args;
    };

    QNetworkRequest request(const QString &path) const
    {
        QNetworkRequest req = jsonRequest(endpoint(m_p.baseUrl, path));
        if (!m_key.isEmpty()) req.setRawHeader("Authorization", "Bearer " + m_key.toUtf8());
        return req;
    }

    QJsonValue userContent(const QString &text, const QList<QByteArray> &jpegs) const
    {
        if (jpegs.isEmpty() || !m_p.vision) return text;
        QJsonArray parts{QJsonObject{{"type", "text"}, {"text", text}}};
        for (const QByteArray &j : jpegs)
            parts.append(QJsonObject{{"type", "image_url"}, {"image_url", QJsonObject{{"url", "data:image/jpeg;base64," + b64(j)}}}});
        return parts;
    }

    void onEvent(const QByteArray &data)
    {
        if (data == "[DONE]") return;
        const QJsonObject o = QJsonDocument::fromJson(data).object();
        if (o.contains("error")) {
            m_streamError = errorFromBody(0, data, {});
            return;
        }
        const QJsonObject choice = o.value("choices").toArray().at(0).toObject();
        const QJsonObject delta = choice.value("delta").toObject();
        const QString content = delta.value("content").toString();
        if (!content.isEmpty()) {
            m_text += content;
            emit textDelta(content);
        }
        for (const QJsonValue &tv : delta.value("tool_calls").toArray()) {
            const QJsonObject tc = tv.toObject();
            const int index = tc.value("index").toInt(int(m_pending.size()));
            PendingCall &p = m_pending[index];
            if (tc.contains("id") && !tc.value("id").toString().isEmpty()) p.id = tc.value("id").toString();
            const QJsonObject fn = tc.value("function").toObject();
            if (fn.contains("name")) p.name += fn.value("name").toString();
            const QJsonValue args = fn.value("arguments");
            p.args += args.isObject() ? QString::fromUtf8(QJsonDocument(args.toObject()).toJson(QJsonDocument::Compact))
                                      : args.toString();
        }
        if (choice.contains("finish_reason") && !choice.value("finish_reason").isNull())
            m_finish = choice.value("finish_reason").toString();
    }

    void onDone(int status, const QByteArray &errBody, const QString &net)
    {
        m_sse->deleteLater();
        m_sse = nullptr;
        TurnResult r;
        if (status >= 400 || (!net.isEmpty() && m_text.isEmpty() && m_pending.isEmpty()) || !m_streamError.isEmpty()) {
            r.stop = StopReason::Error;
            r.error = m_streamError.isEmpty() ? errorFromBody(status, errBody, net) : m_streamError;
            emit turnFinished(r);
            return;
        }

        QJsonObject assistant{{"role", "assistant"}, {"content", m_text.isEmpty() ? QJsonValue() : QJsonValue(m_text)}};
        QJsonArray calls;
        int n = 0;
        for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it, ++n) {
            ToolCall c;
            c.id = it->id.isEmpty() ? QStringLiteral("call_%1_%2").arg(m_messages.size()).arg(n) : it->id;
            c.name = it->name;
            c.rawInput = it->args.isEmpty() ? QStringLiteral("{}") : it->args;
            QJsonParseError pe;
            const QJsonDocument doc = QJsonDocument::fromJson(c.rawInput.toUtf8(), &pe);
            c.inputOk = pe.error == QJsonParseError::NoError && doc.isObject();
            c.input = doc.object();
            r.calls.append(c);
            calls.append(QJsonObject{{"id", c.id}, {"type", "function"},
                                     {"function", QJsonObject{{"name", c.name}, {"arguments", c.rawInput}}}});
        }
        if (!calls.isEmpty()) assistant.insert("tool_calls", calls);
        m_messages.append(assistant);

        r.text = m_text;
        if (m_finish == QLatin1String("length")) r.stop = StopReason::MaxTokens;
        else if (m_finish == QLatin1String("content_filter")) r.stop = StopReason::Refusal;
        else r.stop = r.calls.isEmpty() ? StopReason::EndTurn : StopReason::ToolUse;
        emit turnFinished(r);
    }

    // Ollama's own API lets us raise the context window (its default of
    // 4096 tokens silently cuts the start of long prompts) and constrain
    // the output to the schema.
    bool isOllama() const
    {
        const QUrl u(m_p.baseUrl);
        return u.port() == 11434 || u.host().contains(QLatin1String("ollama"));
    }

    void sendOllamaJson()
    {
        const auto &[system, user, schema] = m_jsonArgs;
        QUrl root(m_p.baseUrl);
        root.setPath(QStringLiteral("/api/chat"));
        QJsonObject userMsg{{"role", "user"}, {"content", user}};
        if (m_p.vision && !m_jsonImages.isEmpty()) {
            QJsonArray images;
            for (const QByteArray &j : m_jsonImages) images.append(b64(j));
            userMsg.insert("images", images);
        }
        QJsonObject options{{"num_ctx", m_p.contextTokens > 0 ? m_p.contextTokens : 16384}, {"temperature", 0.2}};
        if (m_p.maxTokens > 0) options.insert("num_predict", m_p.maxTokens);
        const QJsonObject body{{"model", m_p.model},
                               {"messages", QJsonArray{QJsonObject{{"role", "system"}, {"content", system}}, userMsg}},
                               {"stream", false},
                               {"format", schema},
                               {"options", options}};
        QNetworkRequest req = jsonRequest(root.toString());
        m_jsonReply = m_net->post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(m_jsonReply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *reply = m_jsonReply;
            m_jsonReply = nullptr;
            reply->deleteLater();
            const int st = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray data = reply->readAll();
            if (st >= 400 || reply->error() != QNetworkReply::NoError) {
                emit jsonFinished({}, {}, errorFromBody(st, data, reply->errorString()));
                return;
            }
            const QJsonObject o = QJsonDocument::fromJson(data).object();
            if (qEnvironmentVariableIsSet("MONTAGE_DEBUG"))
                qInfo("ollama: prompt %d tokens, answer %d tokens, %.1fs", o.value("prompt_eval_count").toInt(),
                      o.value("eval_count").toInt(), o.value("total_duration").toDouble() / 1e9);
            const QString text = o.value("message").toObject().value("content").toString();
            QString err;
            const QJsonObject obj = extractJsonObject(text, &err);
            emit jsonFinished(obj, text, err);
        });
    }

    void sendJson(const QString &structured)
    {
        if (isOllama() && structured == QLatin1String("json_schema")) {
            sendOllamaJson();
            return;
        }
        const auto &[system, user, schema] = m_jsonArgs;
        QJsonObject body{{"model", m_p.model},
                         {"messages", QJsonArray{QJsonObject{{"role", "system"}, {"content", system}},
                                                 QJsonObject{{"role", "user"}, {"content", userContent(user, m_jsonImages)}}}},
                         {"stream", false}};
        if (structured == QLatin1String("json_schema")) {
            body.insert("response_format", QJsonObject{{"type", "json_schema"},
                                                       {"json_schema", QJsonObject{{"name", "edit_plan"},
                                                                                   {"schema", schema},
                                                                                   {"strict", false}}}});
        } else if (structured == QLatin1String("json_object")) {
            body.insert("response_format", QJsonObject{{"type", "json_object"}});
        }
        if (m_p.maxTokens > 0) body.insert("max_tokens", m_p.maxTokens);
        if (m_p.local()) body.insert("temperature", 0.2);

        m_jsonReply = m_net->post(request("/chat/completions"), QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(m_jsonReply, &QNetworkReply::finished, this, [this, structured] {
            QNetworkReply *reply = m_jsonReply;
            m_jsonReply = nullptr;
            reply->deleteLater();
            const int st = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray data = reply->readAll();
            if (st == 400 && structured == QLatin1String("json_schema")) {
                // Endpoint doesn't take JSON schemas (e.g. DeepSeek): plain JSON mode.
                m_p.structured = QStringLiteral("json_object");
                sendJson(m_p.structured);
                return;
            }
            if (st >= 400 || reply->error() != QNetworkReply::NoError) {
                emit jsonFinished({}, {}, errorFromBody(st, data, reply->errorString()));
                return;
            }
            const QString text = QJsonDocument::fromJson(data).object().value("choices").toArray().at(0).toObject()
                                     .value("message").toObject().value("content").toString();
            QString err;
            const QJsonObject obj = extractJsonObject(text, &err);
            emit jsonFinished(obj, text, err);
        });
    }

    QNetworkAccessManager *m_net;
    ModelProfile m_p;
    QString m_key;
    QString m_system;
    QJsonArray m_messages;
    QJsonArray m_tools;

    SseRequest *m_sse = nullptr;
    QString m_text;
    QMap<int, PendingCall> m_pending;
    QString m_finish;
    QString m_streamError;

    QNetworkReply *m_jsonReply = nullptr;
    std::tuple<QString, QString, QJsonObject> m_jsonArgs;
    QList<QByteArray> m_jsonImages;
};

// ========================================================= Anthropic adapter
// Claude's native Messages API: adaptive thinking (blocks replayed verbatim),
// progress-note display, prompt caching, eager tool-input streaming and
// server-side refusal fallback.

class AnthropicProvider : public ChatProvider
{
public:
    explicit AnthropicProvider(QObject *parent)
        : ChatProvider(parent)
        , m_net(new QNetworkAccessManager(this))
    {
    }

    void setProfile(const ModelProfile &p, const QString &key) override
    {
        m_p = p;
        m_key = key;
    }

    void reset(const QString &system, const QList<ToolSpec> &tools) override
    {
        cancel();
        m_system = system;
        m_messages = {};
        m_tools = {};
        for (const ToolSpec &t : tools) {
            m_tools.append(QJsonObject{{"name", t.name},
                                       {"description", t.description},
                                       {"input_schema", t.schema},
                                       {"eager_input_streaming", true}});
        }
    }

    void addUserMessage(const QString &text, const QList<QByteArray> &jpegs) override
    {
        QJsonArray content;
        for (const QByteArray &j : jpegs) content.append(imageBlock(j));
        content.append(QJsonObject{{"type", "text"}, {"text", text}});
        m_messages.append(QJsonObject{{"role", "user"}, {"content", content}});
    }

    void addToolResults(const QList<ToolResult> &results) override
    {
        QJsonArray content;
        for (const ToolResult &r : results) {
            QJsonArray parts{QJsonObject{{"type", "text"}, {"text", r.text.isEmpty() ? QStringLiteral("OK") : r.text}}};
            for (const QByteArray &j : r.jpegs) parts.append(imageBlock(j));
            QJsonObject block{{"type", "tool_result"}, {"tool_use_id", r.callId}, {"content", parts}};
            if (r.isError) block.insert("is_error", true);
            content.append(block);
        }
        m_messages.append(QJsonObject{{"role", "user"}, {"content", content}});
    }

    void run() override
    {
        cancel();
        QJsonObject body = baseBody();
        body.insert("stream", true);
        body.insert("messages", m_messages);
        if (!m_tools.isEmpty()) body.insert("tools", m_tools);
        body.insert("cache_control", QJsonObject{{"type", "ephemeral"}});  // cache the growing history too

        m_blocks.clear();
        m_partial.clear();
        m_stop.clear();
        m_streamError.clear();
        m_sse = new SseRequest(m_net, request(), QJsonDocument(body).toJson(QJsonDocument::Compact), this);
        connect(m_sse, &SseRequest::event, this, [this](const QByteArray &, const QByteArray &data) { onEvent(data); });
        connect(m_sse, &SseRequest::done, this, [this](int st, const QByteArray &err, const QString &net) { onDone(st, err, net); });
    }

    void requestJson(const QString &system, const QString &user, const QJsonObject &schema,
                     const QList<QByteArray> &jpegs) override
    {
        cancel();
        QJsonObject body = baseBody(system);
        QJsonArray content;
        for (const QByteArray &j : jpegs) content.append(imageBlock(j));
        content.append(QJsonObject{{"type", "text"}, {"text", user}});
        body.insert("messages", QJsonArray{QJsonObject{{"role", "user"}, {"content", content}}});
        QJsonObject output = body.value("output_config").toObject();
        output.insert("format", QJsonObject{{"type", "json_schema"}, {"schema", schema}});
        body.insert("output_config", output);

        m_jsonReply = m_net->post(request(), QJsonDocument(body).toJson(QJsonDocument::Compact));
        connect(m_jsonReply, &QNetworkReply::finished, this, [this] {
            QNetworkReply *reply = m_jsonReply;
            m_jsonReply = nullptr;
            reply->deleteLater();
            const int st = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QByteArray data = reply->readAll();
            if (st >= 400 || reply->error() != QNetworkReply::NoError) {
                emit jsonFinished({}, {}, errorFromBody(st, data, reply->errorString()));
                return;
            }
            const QJsonObject msg = QJsonDocument::fromJson(data).object();
            if (msg.value("stop_reason").toString() == QLatin1String("refusal")) {
                emit jsonFinished({}, {}, QStringLiteral("The model declined this request."));
                return;
            }
            QString text;
            for (const QJsonValue &b : msg.value("content").toArray())
                if (b.toObject().value("type").toString() == QLatin1String("text")) text += b.toObject().value("text").toString();
            QString err;
            const QJsonObject obj = extractJsonObject(text, &err);
            emit jsonFinished(obj, text, err);
        });
    }

    void cancel() override
    {
        if (m_sse) {
            m_sse->disconnect(this);
            m_sse->deleteLater();
            m_sse = nullptr;
        }
        if (m_jsonReply) {
            m_jsonReply->disconnect(this);
            m_jsonReply->abort();
            m_jsonReply->deleteLater();
            m_jsonReply = nullptr;
        }
    }

private:
    // Feature support by model family; unknown or older models get a plain request.
    bool isCurrentGen() const
    {
        static const QStringList ids{"claude-opus-5-5", "claude-sonnet-5-5", "claude-fable-5-1", "claude-fable-5",
                                     "claude-opus-5", "claude-haiku-5-5"};
        return ids.contains(m_p.model);
    }
    bool supportsUpdates() const
    {
        static const QStringList ids{"claude-opus-5-5", "claude-sonnet-5-5", "claude-fable-5-1", "claude-fable-5"};
        return ids.contains(m_p.model);
    }
    bool supportsDefaultFallback() const
    {
        static const QStringList ids{"claude-opus-5-5", "claude-sonnet-5-5", "claude-fable-5-1", "claude-opus-5"};
        return ids.contains(m_p.model);
    }

    QStringList betas() const
    {
        QStringList b;
        if (supportsDefaultFallback()) b << QStringLiteral("server-side-fallback-2026-07-01");
        if (supportsUpdates()) b << QStringLiteral("thinking-display-updates-2026-08-18");
        return b;
    }

    QNetworkRequest request() const
    {
        QNetworkRequest req = jsonRequest(endpoint(m_p.baseUrl.isEmpty() ? QStringLiteral("https://api.anthropic.com")
                                                                         : m_p.baseUrl,
                                                   QStringLiteral("/v1/messages")));
        req.setRawHeader("x-api-key", m_key.toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
        const QStringList b = betas();
        if (!b.isEmpty()) req.setRawHeader("anthropic-beta", b.join(QLatin1Char(',')).toUtf8());
        return req;
    }

    QJsonObject baseBody(const QString &system = {}) const
    {
        const QString sys = system.isEmpty() ? m_system : system;
        QJsonObject body{{"model", m_p.model},
                         {"max_tokens", m_p.maxTokens > 0 ? m_p.maxTokens : 32000},
                         {"system", QJsonArray{QJsonObject{{"type", "text"}, {"text", sys},
                                                           {"cache_control", QJsonObject{{"type", "ephemeral"}}}}}}};
        if (isCurrentGen()) {
            QJsonObject thinking{{"type", "adaptive"}};
            if (supportsUpdates()) thinking.insert("display", "updates");
            body.insert("thinking", thinking);
            body.insert("output_config", QJsonObject{{"effort", m_p.effort.isEmpty() ? QStringLiteral("medium") : m_p.effort}});
        }
        if (supportsDefaultFallback()) body.insert("fallbacks", "default");
        return body;
    }

    static QJsonObject imageBlock(const QByteArray &jpeg)
    {
        return QJsonObject{{"type", "image"},
                           {"source", QJsonObject{{"type", "base64"}, {"media_type", "image/jpeg"}, {"data", b64(jpeg)}}}};
    }

    void onEvent(const QByteArray &data)
    {
        const QJsonObject e = QJsonDocument::fromJson(data).object();
        const QString type = e.value("type").toString();
        const int index = e.value("index").toInt();
        if (type == QLatin1String("content_block_start")) {
            m_blocks[index] = e.value("content_block").toObject();
        } else if (type == QLatin1String("content_block_delta")) {
            QJsonObject &b = m_blocks[index];
            const QJsonObject d = e.value("delta").toObject();
            const QString dt = d.value("type").toString();
            if (dt == QLatin1String("text_delta")) {
                b.insert("text", b.value("text").toString() + d.value("text").toString());
                emit textDelta(d.value("text").toString());
            } else if (dt == QLatin1String("thinking_delta")) {
                b.insert("thinking", b.value("thinking").toString() + d.value("thinking").toString());
            } else if (dt == QLatin1String("signature_delta")) {
                b.insert("signature", b.value("signature").toString() + d.value("signature").toString());
            } else if (dt == QLatin1String("input_json_delta")) {
                m_partial[index] += d.value("partial_json").toString();
            }
        } else if (type == QLatin1String("content_block_stop")) {
            QJsonObject &b = m_blocks[index];
            const QString bt = b.value("type").toString();
            if (bt == QLatin1String("tool_use")) {
                const QString raw = m_partial.value(index).isEmpty() ? QStringLiteral("{}") : m_partial.value(index);
                QJsonParseError pe;
                const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8(), &pe);
                if (pe.error == QJsonParseError::NoError && doc.isObject()) b.insert("input", doc.object());
                else m_badInput[index] = raw;
            } else if (bt == QLatin1String("thinking")) {
                const QString note = b.value("thinking").toString().trimmed();
                if (!note.isEmpty()) emit progressNote(note);
            }
        } else if (type == QLatin1String("message_delta")) {
            const QString stop = e.value("delta").toObject().value("stop_reason").toString();
            if (!stop.isEmpty()) m_stop = stop;
        } else if (type == QLatin1String("error")) {
            m_streamError = e.value("error").toObject().value("message").toString();
        }
    }

    void onDone(int status, const QByteArray &errBody, const QString &net)
    {
        m_sse->deleteLater();
        m_sse = nullptr;
        TurnResult r;
        if (status >= 400 || !m_streamError.isEmpty() || (m_blocks.isEmpty() && !net.isEmpty())) {
            r.stop = StopReason::Error;
            r.error = m_streamError.isEmpty() ? errorFromBody(status, errBody, net) : m_streamError;
            emit turnFinished(r);
            return;
        }

        // Append the assistant turn exactly as received (thinking blocks
        // included); a tool_use whose input didn't parse is kept with {}.
        QJsonArray content;
        for (auto it = m_blocks.begin(); it != m_blocks.end(); ++it) {
            QJsonObject b = it.value();
            const QString bt = b.value("type").toString();
            if (bt == QLatin1String("text")) r.text += b.value("text").toString();
            if (bt == QLatin1String("tool_use")) {
                ToolCall c;
                c.id = b.value("id").toString();
                c.name = b.value("name").toString();
                c.inputOk = !m_badInput.contains(it.key());
                c.rawInput = c.inputOk ? QString::fromUtf8(QJsonDocument(b.value("input").toObject()).toJson(QJsonDocument::Compact))
                                       : m_badInput.value(it.key());
                if (!c.inputOk || !b.value("input").isObject()) b.insert("input", QJsonObject{});
                c.input = b.value("input").toObject();
                r.calls.append(c);
            }
            content.append(b);
        }
        m_badInput.clear();
        m_messages.append(QJsonObject{{"role", "assistant"}, {"content", content}});

        if (m_stop == QLatin1String("tool_use")) r.stop = StopReason::ToolUse;
        else if (m_stop == QLatin1String("max_tokens")) r.stop = StopReason::MaxTokens;
        else if (m_stop == QLatin1String("refusal")) r.stop = StopReason::Refusal;
        else r.stop = r.calls.isEmpty() ? StopReason::EndTurn : StopReason::ToolUse;
        emit turnFinished(r);
    }

    QNetworkAccessManager *m_net;
    ModelProfile m_p;
    QString m_key;
    QString m_system;
    QJsonArray m_messages;
    QJsonArray m_tools;

    SseRequest *m_sse = nullptr;
    QMap<int, QJsonObject> m_blocks;
    QMap<int, QString> m_partial;
    QMap<int, QString> m_badInput;
    QString m_stop;
    QString m_streamError;
    QNetworkReply *m_jsonReply = nullptr;
};

}  // namespace

ChatProvider *ChatProvider::create(const QString &kind, QObject *parent)
{
    if (kind == QLatin1String("anthropic")) return new AnthropicProvider(parent);
    return new OpenAiProvider(parent);
}

void fetchModels(QNetworkAccessManager *net, const ModelProfile &p, const QString &apiKey,
                 const std::function<void(QStringList, QString)> &done)
{
    const bool anthropic = p.kind == QLatin1String("anthropic");
    const QString base = p.baseUrl.isEmpty() && anthropic ? QStringLiteral("https://api.anthropic.com") : p.baseUrl;
    QNetworkRequest req{QUrl(endpoint(base, anthropic ? QStringLiteral("/v1/models?limit=100") : QStringLiteral("/models")))};
    req.setTransferTimeout(20000);
    if (anthropic) {
        req.setRawHeader("x-api-key", apiKey.toUtf8());
        req.setRawHeader("anthropic-version", "2023-06-01");
    } else if (!apiKey.isEmpty()) {
        req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    }
    QNetworkReply *reply = net->get(req);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done] {
        reply->deleteLater();
        const int st = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        if (st >= 400 || reply->error() != QNetworkReply::NoError) {
            done({}, errorFromBody(st, data, reply->errorString()));
            return;
        }
        QStringList ids;
        for (const QJsonValue &v : QJsonDocument::fromJson(data).object().value("data").toArray())
            ids << v.toObject().value("id").toString();
        ids.removeAll(QString());
        ids.sort();
        done(ids, {});
    });
}

}  // namespace mtg::ai

#include "ChatProvider.moc"
