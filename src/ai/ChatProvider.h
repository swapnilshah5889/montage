#pragma once

// Model-agnostic chat interface. The agent speaks only in these types; each
// adapter translates them to one wire format and keeps the conversation in
// that format (append-only, so provider-specific blocks such as Claude's
// thinking blocks are replayed exactly as received).

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;

namespace mtg::ai {

struct ToolSpec {
    QString name;
    QString description;
    QJsonObject schema;  // JSON Schema for the input object
};

struct ToolCall {
    QString id;
    QString name;
    QJsonObject input;
    QString rawInput;     // as received, for error reporting
    bool inputOk = true;  // false when the arguments weren't a JSON object
};

struct ToolResult {
    QString callId;
    QString name;
    QString text;
    QList<QByteArray> jpegs;  // images for models that can see
    bool isError = false;
};

enum class StopReason { EndTurn, ToolUse, MaxTokens, Refusal, Error, Cancelled };

struct TurnResult {
    StopReason stop = StopReason::EndTurn;
    QString text;          // assistant text for this turn
    QList<ToolCall> calls;
    QString error;
};

// One model endpoint as configured in settings.
struct ModelProfile {
    QString id;
    QString name;
    QString kind = QStringLiteral("openai");  // "openai" (OpenAI-compatible) or "anthropic"
    QString baseUrl;
    QString model;
    bool vision = false;      // can the model read images?
    QString mode = QStringLiteral("auto");   // auto | agent | guided
    QString structured = QStringLiteral("json_schema");  // json_schema | json_object | none
    int maxTokens = 0;        // 0 = provider default
    int contextTokens = 0;    // local models: context window to request (0 = 16384)
    QString effort;           // Claude only: low..max; empty = default for the model
    bool local() const;
    bool guided() const;      // resolved mode
};

class ChatProvider : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;

    virtual void setProfile(const ModelProfile &profile, const QString &apiKey) = 0;

    // Agent conversation: start fresh, then alternate user/tool messages
    // and run() calls. Each run() ends with exactly one turnFinished().
    virtual void reset(const QString &system, const QList<ToolSpec> &tools) = 0;
    virtual void addUserMessage(const QString &text, const QList<QByteArray> &jpegs = {}) = 0;
    virtual void addToolResults(const QList<ToolResult> &results) = 0;
    virtual void run() = 0;

    // Single structured request (guided mode): the reply must be a JSON
    // object matching `schema`. Ends with jsonFinished().
    virtual void requestJson(const QString &system, const QString &user, const QJsonObject &schema,
                             const QList<QByteArray> &jpegs = {}) = 0;

    virtual void cancel() = 0;

    static ChatProvider *create(const QString &kind, QObject *parent);

signals:
    void textDelta(const QString &text);
    void progressNote(const QString &note);  // between-tool-call status notes
    void turnFinished(const mtg::ai::TurnResult &result);
    void jsonFinished(const QJsonObject &object, const QString &rawText, const QString &error);
};

// Lists model ids from the endpoint (OpenAI-compatible /models or the
// Anthropic models API). Calls back on the main thread.
void fetchModels(QNetworkAccessManager *net, const ModelProfile &profile, const QString &apiKey,
                 const std::function<void(QStringList, QString)> &done);

}  // namespace mtg::ai
