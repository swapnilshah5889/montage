#pragma once

#include "model/Draft.h"

#include <QHash>
#include <QImage>
#include <memory>
#include <unordered_map>

namespace mtg {

class VideoDecoder;
class AudioDecoder;

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

    VideoDecoder *videoFor(const Segment &seg, const Material &mat);
    AudioDecoder *audioFor(const Segment &seg, const Material &mat);
    QImage stillFor(const Material &mat, QSize size);
    void evict();

    Draft m_draft;
    std::unordered_map<QString, VideoSlot> m_video;  // keyed by segment id
    std::unordered_map<QString, AudioSlot> m_audio;  // keyed by segment id
    QHash<QString, QImage> m_stills;    // keyed by material id
    QHash<QString, QImage> m_stillScaled;
    quint64 m_tick = 0;
};

}  // namespace mtg
