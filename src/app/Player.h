#pragma once

#include <QImage>
#include <QObject>
#include <QQmlEngine>
#include <QThread>

class Project;
namespace mtg { class PreviewEngine; }

// Main-thread facade over the preview engine thread.
class Player : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)

public:
    explicit Player(Project *project, QObject *parent = nullptr);
    ~Player() override;
    static Player *instance() { return s_instance; }
    static Player *create(QQmlEngine *, QJSEngine *engine);

    qint64 position() const { return m_position; }
    bool playing() const { return m_playing; }
    QImage lastFrame() const { return m_frame; }

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void toggle();
    Q_INVOKABLE void seek(qint64 t);
    Q_INVOKABLE void step(int frames);
    Q_INVOKABLE void setPreviewBox(int width, int height);

signals:
    void frameReady(const QImage &frame);
    void positionChanged();
    void playingChanged();

private:
    void pushDraft();
    void setPosition(qint64 t);

    Project *m_project;
    QThread m_thread;
    mtg::PreviewEngine *m_engine;
    QImage m_frame;
    qint64 m_position = 0;
    bool m_playing = false;

    static inline Player *s_instance = nullptr;
};
