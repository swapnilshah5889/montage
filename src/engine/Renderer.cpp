#include "Renderer.h"

#include "AudioDecoder.h"
#include "FFmpeg.h"
#include "VideoDecoder.h"

#include <QImageReader>
#include <QPainter>
#include <QSet>
#include <algorithm>

namespace mtg {

Renderer::Renderer() = default;
Renderer::~Renderer() = default;

void Renderer::setDraft(const Draft &draft)
{
    m_draft = draft;

    // Drop decoders whose segment is gone or now points at another file.
    QSet<QString> live;
    for (const Track &t : m_draft.tracks)
        for (const Segment &s : t.segments) live.insert(s.id);
    for (auto it = m_video.begin(); it != m_video.end();) {
        const Segment *s = m_draft.segment(it->first);
        const Material *m = s ? m_draft.material(s->materialId) : nullptr;
        if (!live.contains(it->first) || !m || m->path != it->second.path) it = m_video.erase(it);
        else ++it;
    }
    for (auto it = m_audio.begin(); it != m_audio.end();) {
        const Segment *s = m_draft.segment(it->first);
        const Material *m = s ? m_draft.material(s->materialId) : nullptr;
        if (!live.contains(it->first) || !m || m->path != it->second.path) it = m_audio.erase(it);
        else ++it;
    }
}

QSize Renderer::fitCanvas(QSize box) const
{
    const QSize canvas(m_draft.canvas.width, m_draft.canvas.height);
    QSize s = canvas.scaled(box, Qt::KeepAspectRatio);
    if (s.width() > canvas.width()) s = canvas;
    return QSize(std::max(2, s.width() & ~1), std::max(2, s.height() & ~1));
}

VideoDecoder *Renderer::videoFor(const Segment &seg, const Material &mat)
{
    VideoSlot &slot = m_video[seg.id];
    if (!slot.dec) {
        slot.dec = std::make_unique<VideoDecoder>();
        slot.path = mat.path;
        if (!slot.dec->open(mat.path)) qWarning("Cannot open video %s", qPrintable(mat.path));
    }
    slot.lastUse = m_tick;
    return slot.dec->isOpen() ? slot.dec.get() : nullptr;
}

AudioDecoder *Renderer::audioFor(const Segment &seg, const Material &mat)
{
    AudioSlot &slot = m_audio[seg.id];
    if (!slot.dec) {
        slot.dec = std::make_unique<AudioDecoder>();
        slot.path = mat.path;
        if (!slot.dec->open(mat.path)) qWarning("Cannot open audio %s", qPrintable(mat.path));
    }
    slot.lastUse = m_tick;
    return slot.dec->isOpen() ? slot.dec.get() : nullptr;
}

QImage Renderer::stillFor(const Material &mat, QSize size)
{
    const QString key = mat.id + QLatin1Char('@') + QString::number(size.width()) + QLatin1Char('x')
                        + QString::number(size.height());
    auto scaled = m_stillScaled.constFind(key);
    if (scaled != m_stillScaled.constEnd()) return *scaled;

    if (!m_stills.contains(mat.id)) {
        QImageReader reader(mat.path);
        reader.setAutoTransform(true);
        m_stills.insert(mat.id, reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }
    const QImage img = m_stills.value(mat.id).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (m_stillScaled.size() > 32) m_stillScaled.clear();
    m_stillScaled.insert(key, img);
    return img;
}

void Renderer::evict()
{
    // Each open decoder holds a demuxer, codec threads and frames; keep only
    // the ones used recently.
    constexpr int kMaxOpen = 8;
    constexpr quint64 kIdleTicks = 90;
    auto prune = [this](auto &map) {
        if (map.size() <= size_t(kMaxOpen)) return;
        for (auto it = map.begin(); it != map.end();) {
            if (m_tick - it->second.lastUse > kIdleTicks) it = map.erase(it);
            else ++it;
        }
    };
    prune(m_video);
    prune(m_audio);
}

QImage Renderer::renderVideo(qint64 t, QSize size)
{
    ++m_tick;
    QImage canvas(size, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);

    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Main track is the bottom layer; overlay tracks stack in order on top.
    for (const Track &track : m_draft.tracks) {
        if (!track.isVisual()) continue;
        for (const Segment &seg : track.segments) {
            if (!seg.contains(t)) continue;
            const Material *mat = m_draft.material(seg.materialId);
            if (!mat || !mat->isVisual() || mat->width <= 0 || mat->height <= 0) break;

            // Fit the clip inside the canvas, then apply the segment's
            // scale and offset (offsets are in half-canvas units).
            QSizeF fitted = QSizeF(mat->width, mat->height).scaled(QSizeF(size), Qt::KeepAspectRatio);
            fitted *= std::clamp(seg.transform.scale, 0.01, 8.0);
            const QPointF center(size.width() / 2.0 * (1 + seg.transform.x),
                                 size.height() / 2.0 * (1 + seg.transform.y));
            const QRectF rect(center - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
            const QSize pixelSize(std::max(2, int(std::lround(fitted.width()))),
                                  std::max(2, int(std::lround(fitted.height()))));
            if (!rect.intersects(QRectF(QPointF(0, 0), QSizeF(size)))) break;

            QImage frame;
            if (mat->kind == MaterialKind::Image) {
                frame = stillFor(*mat, pixelSize);
            } else if (VideoDecoder *dec = videoFor(seg, *mat)) {
                frame = dec->frameAt(seg.sourceStart + (t - seg.start), pixelSize);
            }
            if (frame.isNull()) break;

            p.setOpacity(std::clamp(seg.opacity, 0.0, 1.0));
            p.drawImage(rect, frame);
            break;  // segments in one track never overlap
        }
    }
    p.end();
    evict();
    return canvas;
}

void Renderer::renderAudio(qint64 startSample, int frames, float *out)
{
    std::fill(out, out + size_t(frames) * kChannels, 0.0f);
    const qint64 endSample = startSample + frames;

    for (const Track &track : m_draft.tracks) {
        for (const Segment &seg : track.segments) {
            const Material *mat = m_draft.material(seg.materialId);
            if (!mat || !mat->hasAudio || seg.volume <= 0.0) continue;

            const qint64 segStart = av_rescale(seg.start, kSampleRate, AV_TIME_BASE);
            const qint64 segEnd = av_rescale(seg.end(), kSampleRate, AV_TIME_BASE);
            const qint64 from = std::max(startSample, segStart);
            const qint64 to = std::min(endSample, segEnd);
            if (from >= to) continue;

            AudioDecoder *dec = audioFor(seg, *mat);
            if (!dec) continue;
            const qint64 srcSample = av_rescale(seg.sourceStart, kSampleRate, AV_TIME_BASE) + (from - segStart);
            dec->mixInto(srcSample, int(to - from), out + (from - startSample) * kChannels, float(seg.volume));
        }
    }

    for (int i = 0; i < frames * kChannels; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

}  // namespace mtg
