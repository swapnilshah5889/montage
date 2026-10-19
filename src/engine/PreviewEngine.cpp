#include "PreviewEngine.h"

#include "FFmpeg.h"

#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#include <QTimer>

namespace mtg {

PreviewEngine::PreviewEngine(QObject *parent)
    : QObject(parent)
{
}

PreviewEngine::~PreviewEngine()
{
    stopAudio();
}

void PreviewEngine::setDraft(const Draft &draft)
{
    m_renderer.setDraft(draft);
    if (!m_playing) renderAt(m_position);
}

void PreviewEngine::setPreviewBox(QSize box)
{
    if (box.isEmpty() || box == m_box) return;
    m_box = box;
    if (!m_playing) renderAt(m_position);
}

void PreviewEngine::requestSeek(qint64 t)
{
    m_pendingSeek.store(t);
    if (!m_seekQueued.exchange(true))
        QMetaObject::invokeMethod(this, &PreviewEngine::processSeek, Qt::QueuedConnection);
}

void PreviewEngine::processSeek()
{
    m_seekQueued.store(false);
    const qint64 t = m_pendingSeek.load();
    if (m_playing) {
        play(t);  // restart the clock and audio from the new spot
        return;
    }
    renderAt(t);
}

void PreviewEngine::renderAt(qint64 t)
{
    m_position = std::clamp<qint64>(t, 0, std::max<qint64>(0, m_renderer.draft().duration()));
    emit frameReady(m_renderer.renderVideo(m_position, m_renderer.fitCanvas(m_box)), m_position);
}

void PreviewEngine::play(qint64 from)
{
    const qint64 duration = m_renderer.draft().duration();
    if (duration <= 0) {
        emit stopped(0);
        return;
    }
    if (from >= duration - 1000) from = 0;

    stopAudio();
    m_playing = true;
    m_playFrom = from;
    m_position = from;
    m_clock.start();

    if (!m_timer) {
        m_timer = new QTimer(this);
        m_timer->setTimerType(Qt::PreciseTimer);
        connect(m_timer, &QTimer::timeout, this, &PreviewEngine::tick);
    }
    const double fps = m_renderer.draft().canvas.fps > 0 ? m_renderer.draft().canvas.fps : 30.0;
    m_timer->start(std::max(5, int(1000.0 / fps)));

    QAudioFormat format;
    format.setSampleRate(kSampleRate);
    format.setChannelCount(kChannels);
    format.setSampleFormat(QAudioFormat::Float);
    const QAudioDevice device = QMediaDevices::defaultAudioOutput();
    if (!device.isNull() && device.isFormatSupported(format)) {
        m_sink = new QAudioSink(device, format, this);
        m_sink->setBufferSize(kSampleRate * kChannels * int(sizeof(float)) / 5);  // 200 ms
        m_audioOut = m_sink->start();
        m_audioSample = av_rescale(from, kSampleRate, AV_TIME_BASE);
        feedAudio(true);
    }
    tick();
}

void PreviewEngine::pause()
{
    if (!m_playing) return;
    m_playing = false;
    if (m_timer) m_timer->stop();
    stopAudio();
    emit stopped(m_position);
}

void PreviewEngine::stopAudio()
{
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
    }
    m_sink = nullptr;
    m_audioOut = nullptr;
}

void PreviewEngine::feedAudio(bool prime)
{
    if (!m_sink || !m_audioOut) return;
    // Stay ~120 ms ahead of the video clock; on start fill what the
    // device will take so it never begins with an underrun.
    const qint64 clockSample = av_rescale(m_playFrom + m_clock.nsecsElapsed() / 1000, kSampleRate, AV_TIME_BASE);
    const qint64 target = prime ? m_audioSample + kSampleRate / 10 : clockSample + kSampleRate * 12 / 100;
    const int bytesPerFrame = kChannels * int(sizeof(float));
    int frames = int(std::min<qint64>(target - m_audioSample, m_sink->bytesFree() / bytesPerFrame));
    if (frames <= 0) return;
    m_mix.resize(size_t(frames) * kChannels);
    m_renderer.renderAudio(m_audioSample, frames, m_mix.data());
    m_audioOut->write(reinterpret_cast<const char *>(m_mix.data()), qint64(frames) * bytesPerFrame);
    m_audioSample += frames;
}

void PreviewEngine::tick()
{
    if (!m_playing) return;
    const qint64 duration = m_renderer.draft().duration();
    const qint64 t = m_playFrom + m_clock.nsecsElapsed() / 1000;
    if (t >= duration) {
        renderAt(duration - 1);
        pause();
        return;
    }
    feedAudio(false);
    renderAt(t);
}

}  // namespace mtg
