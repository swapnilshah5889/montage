#pragma once

#include "model/Draft.h"

#include <QHash>
#include <QImage>
#include <QPainter>
#include <memory>
#include <unordered_map>
#include <vector>

namespace mtg {

class VideoDecoder;
class AudioDecoder;
class EffectProcessor;

// Turns a draft into pictures and sound at any point in time. Preview and
// export each own one, so both produce exactly the same output (CapCut's
// preview graph / compile graph split, minus the separate graphs).
class Renderer
{
public:
    Renderer();
    ~Renderer();

    void setDraft(const Draft &draft);
    const Draft &draft() const { return m_draft; }

    // Composite frame at `t` with the canvas scaled to `size`.
    QImage renderVideo(qint64 t, QSize size);

    // Mix `frames` stereo frames starting at timeline sample `startSample`.
    void renderAudio(qint64 startSample, int frames, float *out);

    // Largest size with the canvas aspect ratio that fits in `box`.
    QSize fitCanvas(QSize box) const;

private:
    struct VideoSlot {
        std::unique_ptr<VideoDecoder> dec;
        QString path;
        quint64 lastUse = 0;
    };
    struct AudioSlot {
        std::unique_ptr<AudioDecoder> dec;
        QString path;
        quint64 lastUse = 0;
    };

    // `framed`: apply the segment's framing (main track); otherwise fit.
    void drawMedia(QPainter &p, const Segment &seg, const Material &mat, qint64 t, QSize size, bool framed = false);
    void drawMainTrack(QPainter &p, const Track &track, qint64 t, QSize size, const QColor &background);
    void compose(QPainter &p, const QImage &a, const QImage &b, const QString &name, double u, qint64 t, QSize size);
    VideoDecoder *videoFor(const Segment &seg, const Material &mat);
    AudioDecoder *audioFor(const Segment &seg, const Material &mat);
    QImage stillFor(const Material &mat, QSize size);
    const QImage &original(const Material &mat);
    bool isGraphic(const Material &mat);
    void evict();

    Draft m_draft;
    std::unordered_map<QString, VideoSlot> m_video;  // keyed by segment id
    std::unordered_map<QString, AudioSlot> m_audio;  // keyed by segment id
    QHash<QString, QImage> m_stills;    // keyed by material id
    QHash<QString, QImage> m_stillScaled;
    quint64 m_tick = 0;
    std::unique_ptr<EffectProcessor> m_fx;  // created on first use
    std::vector<float> m_resample;          // scratch for time-remapped audio
};

}  // namespace mtg
