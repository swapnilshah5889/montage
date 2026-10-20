#pragma once

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QThread>
#include <QUrl>
#include <atomic>

class Project;

// Runs an export on a background thread and reports progress to QML.
class Exporter : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(bool running READ running NOTIFY runningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString stage READ stage NOTIFY progressChanged)  // "9:16 · 1 of 3" for multi-format exports

public:
    explicit Exporter(Project *project, QObject *parent = nullptr);
    ~Exporter() override;
    static Exporter *create(QQmlEngine *, QJSEngine *engine);

    bool running() const { return m_running; }
    double progress() const { return m_progress; }
    QString stage() const { return m_stage; }

    // Exports the timeline to `file`. With `formats` ("9:16", "1:1", "4:5",
    // "16:9"...), exports one file per shape next to it ("Promo 9x16.mp4"),
    // each re-laid-out and reframed for that shape.
    Q_INVOKABLE void start(const QUrl &file, const QStringList &formats = {});
    Q_INVOKABLE QStringList formatNames() const;
    // Paths `start` would write, for messages.
    static QStringList outputPaths(const QString &file, const QStringList &formats);
    Q_INVOKABLE void cancel() { m_cancel = true; }

signals:
    void runningChanged();
    void progressChanged();
    // `file` is the first file written; `files` lists all of them.
    void finished(bool ok, const QString &message, const QUrl &file, const QStringList &files);

private:
    Project *m_project;
    QPointer<QThread> m_thread;
    std::atomic<bool> m_cancel{false};
    double m_progress = 0;
    QString m_stage;
    bool m_running = false;

    static inline Exporter *s_instance = nullptr;
};
