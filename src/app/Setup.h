#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>

class QNetworkAccessManager;
class QNetworkReply;
class Agent;

// First-run setup: what Montage's local AI needs and whether it's here.
// Required: Ollama with a chat model. Recommended: a vision model (to see
// what's in clips) and a whisper model (captions, text-based editing).
// Optional: ComfyUI, Kokoro and ACE-Step for generated images, voice and
// music. Models are downloaded with progress - through Ollama's own API,
// or straight into Montage's models folder for whisper.
class Setup : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QVariantList items READ items NOTIFY changed)
    Q_PROPERTY(bool ready READ ready NOTIFY changed)  // everything required is in place
    Q_PROPERTY(bool seen READ seen NOTIFY changed)    // the setup was shown once

public:
    explicit Setup(Agent *agent, QObject *parent = nullptr);
    static Setup *create(QQmlEngine *, QJSEngine *);

    QVariantList items() const { return m_items; }
    bool ready() const;
    bool seen() const;

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void act(const QString &id);  // download, pull or open the link for an item
    Q_INVOKABLE void cancel(const QString &id);
    Q_INVOKABLE void markSeen();

signals:
    void changed();

private:
    struct Item {
        QString id, name, purpose;
        QString state;   // ok | missing | optional | working | error
        QString detail;  // "qwen2.5:7b installed", "4.7 GB download", ...
        QString action;  // button text; empty = none
        double progress = -1;
        bool required = false;
    };
    Item *find(const QString &id);
    void publish();
    void setItem(const Item &item);
    void pullModel(const QString &id, const QString &model);
    void downloadWhisper();
    QString chatModel() const;

    Agent *m_agent;
    QNetworkAccessManager *m_net;
    QList<Item> m_list;
    QVariantList m_items;
    QHash<QString, QPointer<QNetworkReply>> m_jobs;
    QStringList m_installed;  // Ollama models
    bool m_ollama = false;

    static inline Setup *s_instance = nullptr;
};
