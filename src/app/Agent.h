#pragma once

#include "ai/ChatProvider.h"

#include <QAbstractListModel>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>

class Project;
class Exporter;
class QNetworkAccessManager;
namespace mtg::ai { class EditorTools; }

// Chat transcript shown in the assistant panel.
class ChatModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by Agent")
public:
    enum Role { KindRole = Qt::UserRole + 1, TextRole, PendingRole };
    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // kind: user | assistant | activity | error
    int add(const QString &kind, const QString &text, bool pending = false);
    void appendText(int row, const QString &delta);
    void setText(int row, const QString &text);
    void setPending(int row, bool pending);
    void clear();
    QString text(int row) const;

private:
    struct Item {
        QString kind, text;
        bool pending = false;
    };
    QList<Item> m_items;
};

// The assistant: turns chat requests into edits using whichever model the
// user picked. Capable models run a tool loop; small local models answer
// with one schema-constrained edit plan that the app checks and repairs.
class Agent : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(ChatModel *messages READ messages CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QVariantList profiles READ profiles NOTIFY profilesChanged)
    Q_PROPERTY(QString profileId READ profileId WRITE setProfileId NOTIFY profileChanged)
    Q_PROPERTY(QVariantMap profile READ profileMap NOTIFY profileChanged)
    Q_PROPERTY(QVariantMap brand READ brand WRITE setBrand NOTIFY brandChanged)

public:
    Agent(Project *project, Exporter *exporter, QObject *parent);
    ~Agent() override;
    static Agent *instance() { return s_instance; }
    static Agent *create(QQmlEngine *, QJSEngine *engine);

    ChatModel *messages() const { return m_chat; }
    bool busy() const { return m_busy; }
    QString status() const { return m_status; }
    QVariantList profiles() const;
    QString profileId() const { return m_profile.id; }
    void setProfileId(const QString &id);
    QVariantMap profileMap() const;
    QVariantMap brand() const;
    void setBrand(const QVariantMap &brand);
    Q_INVOKABLE QStringList adStyles() const;

    Q_INVOKABLE void send(const QString &text);
    Q_INVOKABLE void stop();
    Q_INVOKABLE void newChat();

    // Settings
    Q_INVOKABLE QString saveProfile(const QVariantMap &profile);
    Q_INVOKABLE void deleteProfile(const QString &id);
    Q_INVOKABLE void setApiKey(const QString &id, const QString &key);
    Q_INVOKABLE bool hasApiKey(const QString &id) const;
    Q_INVOKABLE void fetchModels(const QString &id);
    Q_INVOKABLE QVariantList presets() const;

signals:
    void busyChanged();
    void statusChanged();
    void profilesChanged();
    void profileChanged();
    void brandChanged();
    void modelsFetched(const QString &id, const QStringList &models, const QString &error);
    void finished(bool ok, const QString &reply);

private:
    void loadProfiles();
    void storeProfiles() const;
    QString apiKeyFor(const mtg::ai::ModelProfile &p) const;
    void setBusy(bool busy);
    void setStatus(const QString &status);
    void finish(bool ok, const QString &reply);
    void ensureProvider();

    // Agent mode
    void startAgentTurn(const QString &text);
    void onTurnFinished(const mtg::ai::TurnResult &r);
    void runTools(QList<mtg::ai::ToolCall> calls, QList<mtg::ai::ToolResult> done);

    // Guided mode
    void startGuided(const QString &text);
    void requestPlan(const QString &feedback);
    void onPlan(const QJsonObject &plan, const QString &raw, const QString &error);
    QString planFeedback(const QJsonObject &current, const QJsonObject &plan, double newLength) const;

    Project *m_project;
    Exporter *m_exporter;
    mtg::ai::EditorTools *m_tools;
    ChatModel *m_chat;
    QNetworkAccessManager *m_net;
    mtg::ai::ChatProvider *m_provider = nullptr;
    QString m_providerKind;

    QList<mtg::ai::ModelProfile> m_profiles;
    mtg::ai::ModelProfile m_profile;
    bool m_conversationStarted = false;

    bool m_busy = false;
    QString m_status;
    int m_assistantRow = -1;
    int m_rounds = 0;
    QString m_request;
    QString m_lastRaw;
    int m_attempts = 0;
    QList<QPair<QString, QString>> m_history;  // guided mode: (request, reply)

    static inline Agent *s_instance = nullptr;
};
