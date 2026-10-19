#pragma once

#include "Renderer.h"

#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <atomic>
#include <vector>

class QAudioSink;
class QIODevice;
class QTimer;

namespace mtg {

// Lives on its own thread: renders preview frames and feeds the audio
// device while playing. All public slots are invoked through queued calls.
class PreviewEngine : public QObject
{
    Q_OBJECT
public:
    explicit PreviewEngine(QObject *parent = nullptr);
    ~PreviewEngine() override;

    // Thread-safe: coalesces rapid scrubbing into one render.
    void requestSeek(qint64 t);

public slots:
    void setDraft(const mtg::Draft &draft);
    void play(qint64 from);
    void pause();
    void setPreviewBox(QSize box);

signals:
    void frameReady(const QImage &frame, qint64 t);
    void stopped(qint64 t);

private:
    void processSeek();
    void tick();
    void feedAudio(bool prime);
    void renderAt(qint64 t);
    void stopAudio();

    Renderer m_renderer;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
    qint64 m_playFrom = 0;
    qint64 m_position = 0;
    bool m_playing = false;
    QSize m_box{1280, 1280};

    QAudioSink *m_sink = nullptr;
    QIODevice *m_audioOut = nullptr;
    qint64 m_audioSample = 0;
    std::vector<float> m_mix;

    std::atomic<qint64> m_pendingSeek{0};
    std::atomic<bool> m_seekQueued{false};
};

}  // namespace mtg
