#pragma once

// Watches the project's media and builds the scene index for each video and
// photo in the background: shots, quality and on-device labels first
// (seconds), then a short description of every shot from a local
// vision-language model (qwen3-vl through Ollama; about ten seconds a shot).
// Descriptions are cached, so each file is only watched once. Work pauses
// while the assistant is answering so it doesn't slow the chat down.

#include "engine/Scenes.h"
#include "model/Draft.h"

#include <QHash>
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace mtg::ai {

struct VisionConfig {
    QString ollamaUrl;  // http://127.0.0.1:11434
    QString model;      // empty: the best installed vision model
    bool enabled = true;
    int maxShotsPerFile = 12;
    static VisionConfig load();  // settings "vision/*", then MONTAGE_VISION_* in the environment
};

class MediaIndexer : public QObject
{
    Q_OBJECT
public:
    explicit MediaIndexer(QObject *parent = nullptr);

    // Queues every video and photo in `d` that isn't fully indexed yet.
    void watch(const Draft &d);
    void setPaused(bool paused);
    bool busy() const { return m_current >= 0 || !m_queue.isEmpty(); }
    // "Watching your clips: 2 of 5" while working, else empty.
    QString status() const;

signals:
    void statusChanged();
    void indexed(const QString &path);  // a file's index changed (shots, then descriptions)

private:
    struct Job {
        Material material;
        SceneIndex index;
        QList<int> order;  // shots still to describe, most worth describing first
    };
    void next();
    void describeNext();
    void pickModel(const std::function<void()> &then);

    VisionConfig m_config;
    QNetworkAccessManager *m_net;
    QList<Job> m_queue;
    QHash<QString, bool> m_seen;  // path -> fully done
    Job m_job;
    int m_current = -1;  // 0 = analysing shots, 1 = describing
    int m_done = 0, m_total = 0;
    bool m_paused = false;
    bool m_modelChecked = false;
    QString m_model;
    QPointer<QNetworkReply> m_reply;
};

// One frame -> description, framing, subjects and appeal, as JSON from the
// vision model. Exposed for tests and the CLI.
QJsonObject shotRequestBody(const QString &model, const QImage &frame);
bool readShotReply(const QByteArray &body, Shot *out);

}  // namespace mtg::ai
