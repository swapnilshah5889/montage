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

public:
    explicit Exporter(Project *project, QObject *parent = nullptr);
    ~Exporter() override;
    static Exporter *create(QQmlEngine *, QJSEngine *engine);

    bool running() const { return m_running; }
    double progress() const { return m_progress; }

    Q_INVOKABLE void start(const QUrl &file);
    Q_INVOKABLE void cancel() { m_cancel = true; }

signals:
    void runningChanged();
    void progressChanged();
    void finished(bool ok, const QString &message, const QUrl &file);

private:
    Project *m_project;
    QPointer<QThread> m_thread;
    std::atomic<bool> m_cancel{false};
    double m_progress = 0;
    bool m_running = false;

    static inline Exporter *s_instance = nullptr;
};
