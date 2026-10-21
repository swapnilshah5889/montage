#include "Renderer.h"

#include "Animation.h"
#include "AudioDecoder.h"
#include "Effects.h"
#include "Reframe.h"
#include "Speech.h"
#include "FFmpeg.h"
#include "VideoDecoder.h"

#include <QImageReader>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QSet>
#include <algorithm>
#include <cmath>

namespace mtg {

// Box behind text: CTA pills, starburst badges, price tags, ribbons...
static QPainterPath boxPath(const QString &shape, const QRectF &text, double fontPx)
{
    const double pad = fontPx * 0.35;
    const QRectF r = text.adjusted(-pad, -pad * 0.5, pad, pad * 0.5);
    QPainterPath path;
    if (shape == QLatin1String("pill")) {
        const QRectF wide = r.adjusted(-pad * 0.6, -pad * 0.15, pad * 0.6, pad * 0.15);
        path.addRoundedRect(wide, wide.height() / 2, wide.height() / 2);
    } else if (shape == QLatin1String("circle")) {
        const double d = std::max(r.width(), r.height()) * 1.18;
        path.addEllipse(QRectF(r.center() - QPointF(d / 2, d / 2), QSizeF(d, d)));
    } else if (shape == QLatin1String("burst")) {
        const double outer = std::max(r.width(), r.height()) * 0.72, inner = outer * 0.8;
        const int points = 18;
        for (int i = 0; i < points * 2; ++i) {
            const double a = M_PI * i / points - M_PI / 2;
            const double rad = (i % 2 == 0) ? outer : inner;
            const QPointF pt = r.center() + QPointF(std::cos(a) * rad, std::sin(a) * rad);
            i == 0 ? path.moveTo(pt) : path.lineTo(pt);
        }
        path.closeSubpath();
    } else if (shape == QLatin1String("tag")) {
        const double tip = r.height() * 0.5;
        path.moveTo(r.left() - tip, r.center().y());
        path.lineTo(r.left(), r.top());
        path.lineTo(r.right(), r.top());
        path.lineTo(r.right(), r.bottom());
        path.lineTo(r.left(), r.bottom());
        path.closeSubpath();
        QPainterPath hole;
        hole.addEllipse(QPointF(r.left() - tip * 0.25, r.center().y()), tip * 0.14, tip * 0.14);
        path = path.subtracted(hole);
    } else if (shape == QLatin1String("ribbon")) {
        const double notch = r.height() * 0.35;
        const QRectF w = r.adjusted(-notch, 0, notch, 0);
        path.moveTo(w.topLeft());
        path.lineTo(w.topRight());
        path.lineTo(w.right() - notch, w.center().y());
        path.lineTo(w.bottomRight());
        path.lineTo(w.bottomLeft());
        path.lineTo(w.left() + notch, w.center().y());
        path.closeSubpath();
    } else if (shape == QLatin1String("underline")) {
        path.addRect(QRectF(text.left(), text.bottom() - fontPx * 0.05, text.width(), fontPx * 0.14));
    } else {
        path.addRoundedRect(r, pad * 0.6, pad * 0.6);
    }
    return path;
}

// Draws a text clip centred on its animated position. Lines wrap at 90% of
// the canvas width; sizes scale with the canvas so preview matches export.
// Typewriter reveals characters; kinetic pops words in one after another.
static void drawText(QPainter &p, const Segment &seg, QSize canvas, const FrameState &fs)
{
    const TextStyle &st = seg.text;
    if (st.text.trimmed().isEmpty() || fs.opacity <= 0.001) return;

    QFont font(st.font);
    const int shortSide = std::min(canvas.width(), canvas.height());  // same look in 16:9, 9:16 and 1:1
    const double baseScale = std::max(0.05, seg.transform.scale);
    font.setPixelSize(std::max(4, int(std::lround(st.size * shortSide * baseScale))));
    font.setBold(st.bold);
    const Qt::Alignment align = st.align == QLatin1String("left")    ? Qt::AlignLeft
                                : st.align == QLatin1String("right") ? Qt::AlignRight
                                                                     : Qt::AlignHCenter;
    QString text = st.text;
    text.replace(QLatin1Char('\n'), QChar::LineSeparator);
    QTextLayout layout(text, font);
    QTextOption option(align);
    option.setWrapMode(QTextOption::WordWrap);
    layout.setTextOption(option);

    // Round badges (burst, circle) wrap into a compact block instead of
    // growing a huge shape around one long line.
    const bool round = !st.background.isEmpty() && (st.shape == QLatin1String("burst") || st.shape == QLatin1String("circle"));
    const double wrapWidth = round ? std::min(canvas.width() * 0.9, font.pixelSize() * 4.6) : canvas.width() * 0.9;
    double y = 0;
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(wrapWidth);
        line.setPosition(QPointF(0, y));
        y += line.height();
    }
    layout.endLayout();

    // Split into words with their positions, so animations can address them.
    struct Word {
        QPainterPath path;
        QRectF rect;
        int chars;
    };
    QList<Word> words;
    QRectF bounds;
    int totalChars = 0;
    for (int i = 0; i < layout.lineCount(); ++i) {
        const QTextLine line = layout.lineAt(i);
        const int lineStart = line.textStart(), lineEnd = line.textStart() + line.textLength();
        bounds = bounds.united(line.naturalTextRect());
        int pos = lineStart;
        while (pos < lineEnd) {
            while (pos < lineEnd && text.at(pos).isSpace()) ++pos;
            int end = pos;
            while (end < lineEnd && !text.at(end).isSpace()) ++end;
            if (end > pos) {
                const QString w = text.mid(pos, end - pos);
                const double x0 = line.cursorToX(pos), x1 = line.cursorToX(end);
                Word word;
                word.path.addText(QPointF(x0, line.y() + line.ascent()), font, w);
                word.rect = QRectF(x0, line.y(), x1 - x0, line.height());
                word.chars = int(w.size());
                totalChars += word.chars;
                words.append(word);
            }
            pos = end;
        }
    }
    if (words.isEmpty()) return;

    // Typewriter: keep only the revealed characters.
    if (fs.textMode == QLatin1String("typewriter")) {
        int budget = int(std::floor(fs.textProgress * totalChars + 0.0001));
        QList<Word> shown;
        int pos = 0;
        for (const Word &w : words) {
            if (budget <= 0) break;
            if (w.chars <= budget) {
                shown.append(w);
            } else {
                // Partial word: clip its path to the revealed share of its width.
                Word part = w;
                QPainterPath clip;
                clip.addRect(QRectF(w.rect.left(), w.rect.top() - w.rect.height(), w.rect.width() * budget / w.chars, w.rect.height() * 3));
                part.path = w.path.intersected(clip);
                part.rect.setWidth(w.rect.width() * budget / w.chars);
                shown.append(part);
            }
            budget -= w.chars;
            pos += w.chars;
        }
        words = shown;
        if (words.isEmpty()) return;
    }

    const double fontPx = font.pixelSize();
    QPointF center(canvas.width() / 2.0 * (1 + fs.x), canvas.height() / 2.0 * (1 + fs.y));

    // Safe area: where the text rests (box and rotation included) stays inside
    // the frame, so a badge placed for 16:9 still fits in 9:16. Motions still
    // slide in from outside; keyframed positions are left as animated.
    if (!seg.keys.contains(QStringLiteral("x")) && !seg.keys.contains(QStringLiteral("y"))) {
        const QRectF foot = st.background.isEmpty() ? bounds : boxPath(st.shape, bounds, fontPx).boundingRect();
        QTransform turn;
        turn.rotate(seg.transform.rotation);
        QRectF rest = turn.mapRect(foot.translated(-bounds.center()));
        rest.translate(canvas.width() / 2.0 * (1 + seg.transform.x), canvas.height() / 2.0 * (1 + seg.transform.y));
        auto shift = [](double lo, double hi, double margin, double extent) {
            if (hi - lo > extent - 2 * margin) return extent / 2 - (lo + hi) / 2;
            if (lo < margin) return margin - lo;
            if (hi > extent - margin) return extent - margin - hi;
            return 0.0;
        };
        center += QPointF(shift(rest.left(), rest.right(), canvas.width() * 0.03, canvas.width()),
                          shift(rest.top(), rest.bottom(), canvas.height() * 0.03, canvas.height()));
    }

    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setOpacity(fs.opacity);
    p.translate(center);
    p.rotate(fs.rotation);
    const double k = fs.scale / baseScale;
    p.scale(k, k);
    p.translate(-bounds.center());

    if (!st.background.isEmpty()) {
        // The box hugs what's visible, so a typewriter box grows with the text.
        QRectF box;
        for (const Word &w : words) box = box.united(w.rect);
        if (fs.textMode != QLatin1String("typewriter")) box = bounds;
        const QPainterPath shape = boxPath(st.shape, box, fontPx);
        if (!st.shadow.isEmpty()) {
            QColor sc(st.shadow);
            sc.setAlphaF(0.55);
            p.fillPath(shape.translated(fontPx * 0.06, fontPx * 0.08), sc);
        }
        p.fillPath(shape, QColor(st.background));
    }

    QBrush fill(QColor(st.color));
    if (!st.gradient.isEmpty()) {
        QLinearGradient g(bounds.topLeft(), bounds.bottomLeft());
        g.setColorAt(0, QColor(st.color));
        g.setColorAt(1, QColor(st.gradient));
        fill = QBrush(g);
    }
    QPen outlinePen(QColor(st.outlineColor), st.outline * fontPx * 2);
    outlinePen.setJoinStyle(Qt::RoundJoin);
    QColor shadow(st.shadow);
    shadow.setAlphaF(shadow.alphaF() * 0.75);
    const QPointF shadowOffset(fontPx * 0.05, fontPx * 0.07);

    const bool kinetic = fs.textMode == QLatin1String("kinetic");
    for (int i = 0; i < words.size(); ++i) {
        const Word &w = words[i];
        p.save();
        if (kinetic) {
            // Each word pops in after the previous one has started.
            const double u = std::clamp(fs.textProgress * (words.size() + 1.0) - i, 0.0, 1.0);
            if (u <= 0) {
                p.restore();
                continue;
            }
            const double ws = 0.3 + 0.7 * ease(QStringLiteral("back"), u);
            p.setOpacity(p.opacity() * std::min(1.0, u * 3));
            p.translate(w.rect.center());
            p.scale(ws, ws);
            p.translate(-w.rect.center());
        }
        if (shadow.isValid() && !st.shadow.isEmpty()) p.fillPath(w.path.translated(shadowOffset), shadow);
        if (st.outline > 0) p.strokePath(w.path, outlinePen);
        p.fillPath(w.path, fill);
        p.restore();
    }
    p.restore();
}

// Cheap blur for animated blur-in/out: shrink and enlarge again.
static QImage softened(const QImage &img, double radius)
{
    if (radius < 0.5 || img.isNull()) return img;
    const double f = 1.0 + radius / 3.0;
    const QSize small(std::max(1, int(img.width() / f)), std::max(1, int(img.height() / f)));
    return img.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
        .scaled(img.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
}

// Decode sizes are rounded so animated scale doesn't re-create the scaler every frame.
static QSize quantized(QSizeF s)
{
    auto q = [](double v) { return std::max(16, int(std::ceil(v / 32.0)) * 32); };
    return QSize(q(s.width()), q(s.height()));
}

Renderer::Renderer() = default;
Renderer::~Renderer() = default;  // out of line: EffectProcessor is incomplete in the header

void Renderer::setDraft(const Draft &draft)
{
    m_draft = draft;
    m_speechReady = false;

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

const QImage &Renderer::original(const Material &mat)
{
    if (!m_stills.contains(mat.id)) {
        QImageReader reader(mat.path);
        reader.setAutoTransform(true);
        m_stills.insert(mat.id, reader.read().convertToFormat(QImage::Format_ARGB32_Premultiplied));
    }
    return m_stills[mat.id];
}

// Logos, stickers and other cut-out graphics: an image whose corners are
// transparent. Auto framing shows these whole instead of cropping them.
bool Renderer::isGraphic(const Material &mat)
{
    if (mat.kind != MaterialKind::Image) return false;
    const QImage &img = original(mat);
    if (img.isNull() || !img.hasAlphaChannel()) return false;
    const int w = img.width() - 1, h = img.height() - 1;
    for (const QPoint &pt : {QPoint(0, 0), QPoint(w, 0), QPoint(0, h), QPoint(w, h)})
        if (qAlpha(img.pixel(pt)) < 250) return true;
    return false;
}

QImage Renderer::stillFor(const Material &mat, QSize size)
{
    const QString key = mat.id + QLatin1Char('@') + QString::number(size.width()) + QLatin1Char('x')
                        + QString::number(size.height());
    auto scaled = m_stillScaled.constFind(key);
    if (scaled != m_stillScaled.constEnd()) return *scaled;

    const QImage img = original(mat).scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
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

// Draws a video or photo clip at timeline time `t`. `t` may fall a little
// outside the clip during transitions: the source keeps playing (or holds
// its first/last frame) and the clip's animation holds its edge state.
void Renderer::drawMedia(QPainter &p, const Segment &seg, const Material &mat, qint64 t, QSize size, bool framed)
{
    if (!mat.isVisual() || mat.width <= 0 || mat.height <= 0) return;
    const qint64 local = t - seg.start;
    const FrameState fs = animate(seg, std::clamp<qint64>(local, 0, std::max<qint64>(0, seg.duration - 1)));
    if (fs.opacity <= 0.001) return;

    // Size the clip to the canvas (inside it, or covering it for fill and
    // auto framing), then apply the animated scale, offset (half-canvas
    // units) and rotation around its centre.
    QString framing = framed ? seg.framing : QString();
    if (framing == QLatin1String("auto") && isGraphic(mat)) framing = QStringLiteral("fit");
    const bool cover = framing == QLatin1String("auto") || framing == QLatin1String("fill");
    const QSizeF base = QSizeF(mat.width, mat.height).scaled(QSizeF(size), cover ? Qt::KeepAspectRatioByExpanding : Qt::KeepAspectRatio);
    const QSizeF fitted = base * std::clamp(fs.scale, 0.0, 8.0);
    if (fitted.width() < 1 || fitted.height() < 1) return;
    QPointF center(size.width() / 2.0 * (1 + fs.x), size.height() / 2.0 * (1 + fs.y));
    if (framing == QLatin1String("auto") && (fitted.width() > size.width() + 2 || fitted.height() > size.height() + 2)) {
        // Auto Reframe: slide the crop to keep the subject in frame, never past the edges.
        const qint64 src = mat.kind == MaterialKind::Image ? 0 : seg.sourceAt(std::clamp<qint64>(local, 0, seg.duration));
        const QPointF c = subjectAt(mat, seg.sourceStart, seg.sourceSpan(), src);
        const double mx = std::max(0.0, (fitted.width() - size.width()) / 2), my = std::max(0.0, (fitted.height() - size.height()) / 2);
        center += QPointF(std::clamp((0.5 - c.x()) * fitted.width(), -mx, mx), std::clamp((0.5 - c.y()) * fitted.height(), -my, my));
    }
    const QRectF rect(center - QPointF(fitted.width() / 2, fitted.height() / 2), fitted);
    if (fs.rotation == 0 && !rect.intersects(QRectF(QPointF(0, 0), QSizeF(size)))) return;

    // Decode at the larger of the resting and animated size (capped),
    // rounded so a moving scale doesn't rebuild the scaler every frame.
    const double decodeScale = std::clamp(std::max(fs.scale, seg.transform.scale), 0.05, 2.0);
    const QSize pixelSize = quantized(base * decodeScale);

    QImage frame;
    if (mat.kind == MaterialKind::Image) {
        frame = stillFor(mat, pixelSize);
    } else if (VideoDecoder *dec = videoFor(seg, mat)) {
        frame = dec->frameAt(std::max<qint64>(0, seg.sourceAt(local)), pixelSize);
    }
    if (frame.isNull()) return;
    frame = softened(frame, fs.blur * size.height() / 1080.0);
    if (!seg.effects.isEmpty()) {
        if (!m_fx) m_fx = std::make_unique<EffectProcessor>();
        frame = m_fx->apply(frame, seg.effects, local / 1e6);
    }

    if (framing == QLatin1String("fit-blur")) {
        // Fill the bars with a soft, dimmed copy of the clip.
        const QSizeF back = QSizeF(mat.width, mat.height).scaled(QSizeF(size), Qt::KeepAspectRatioByExpanding);
        QImage small = frame.scaled(quantized(back / 10), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        small = softened(small, 4);
        p.save();
        p.setOpacity(p.opacity() * fs.opacity);
        p.drawImage(QRectF(QPointF((size.width() - back.width()) / 2, (size.height() - back.height()) / 2), back), small);
        p.fillRect(QRectF(QPointF(0, 0), QSizeF(size)), QColor(0, 0, 0, 70));
        p.restore();
    }

    p.save();
    p.setOpacity(p.opacity() * fs.opacity);
    p.translate(center);
    if (fs.rotation != 0) p.rotate(fs.rotation);
    p.drawImage(QRectF(-fitted.width() / 2, -fitted.height() / 2, fitted.width(), fitted.height()), frame);
    p.restore();
}

// The main track, including transitions centred on its cuts.
void Renderer::drawMainTrack(QPainter &p, const Track &track, qint64 t, QSize size, const QColor &background)
{
    const QList<Segment> &segs = track.segments;
    qsizetype i = -1;
    for (qsizetype k = 0; k < segs.size(); ++k)
        if (segs[k].contains(t)) i = k;
    if (i < 0) return;

    auto span = [](const Segment &out, const Segment &in) {
        return std::clamp<qint64>(in.transition.us, 1, std::min(out.duration, in.duration));
    };
    const Segment *a = nullptr, *b = nullptr;
    double u = 0;
    if (i > 0 && !segs[i].transition.name.isEmpty()) {
        const qint64 d = span(segs[i - 1], segs[i]), cut = segs[i].start;
        if (t < cut + d / 2) {
            a = &segs[i - 1];
            b = &segs[i];
            u = double(t - (cut - d / 2)) / double(d);
        }
    }
    if (!a && i + 1 < segs.size() && !segs[i + 1].transition.name.isEmpty()) {
        const qint64 d = span(segs[i], segs[i + 1]), cut = segs[i + 1].start;
        if (t >= cut - d / 2) {
            a = &segs[i];
            b = &segs[i + 1];
            u = double(t - (cut - d / 2)) / double(d);
        }
    }
    if (!a) {
        if (const Material *m = m_draft.material(segs[i].materialId)) drawMedia(p, segs[i], *m, t, size, true);
        return;
    }

    auto layer = [&](const Segment &seg) {
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(background);
        if (const Material *m = m_draft.material(seg.materialId)) {
            QPainter lp(&img);
            lp.setRenderHint(QPainter::SmoothPixmapTransform);
            drawMedia(lp, seg, *m, t, size, true);
        }
        return img;
    };
    compose(p, layer(*a), layer(*b), b->transition.name, std::clamp(u, 0.0, 1.0), t, size);
}

// Blends the outgoing (a) and incoming (b) frames; u runs 0 -> 1 across the transition.
void Renderer::compose(QPainter &p, const QImage &a, const QImage &b, const QString &name, double u, qint64 t, QSize size)
{
    const double e = ease(QStringLiteral("ease-in-out"), u);
    const double w = size.width(), h = size.height();
    const QImage &near = u < 0.5 ? a : b;  // the side currently showing for cut-style transitions
    const double peak = 1 - std::abs(2 * u - 1);  // 0 at the edges, 1 at the cut
    auto fx = [&](const QImage &img, const QString &effect, double amount) {
        if (!m_fx) m_fx = std::make_unique<EffectProcessor>();
        return amount <= 0.001 ? img : m_fx->apply(img, {Effect{effect, amount, {}}}, t / 1e6);
    };
    auto around = [&](const QImage &img, double scale, double angle, double opacity) {
        p.save();
        p.setOpacity(opacity);
        p.translate(w / 2, h / 2);
        p.rotate(angle);
        p.scale(scale, scale);
        p.drawImage(QPointF(-w / 2, -h / 2), img);
        p.restore();
    };

    if (name == QLatin1String("fade-black") || name == QLatin1String("fade-white")) {
        p.drawImage(0, 0, near);
        p.fillRect(QRectF(0, 0, w, h), QColor(name == QLatin1String("fade-black") ? 0 : 255, name == QLatin1String("fade-black") ? 0 : 255,
                                              name == QLatin1String("fade-black") ? 0 : 255, int(255 * std::clamp(peak, 0.0, 1.0))));
    } else if (name.startsWith(QLatin1String("push-"))) {
        const QPointF dir = name.endsWith(QLatin1String("left")) ? QPointF(-w, 0) : name.endsWith(QLatin1String("right")) ? QPointF(w, 0)
                            : name.endsWith(QLatin1String("up")) ? QPointF(0, -h) : QPointF(0, h);
        p.drawImage(dir * e, a);
        p.drawImage(dir * (e - 1), b);
    } else if (name.startsWith(QLatin1String("slide-"))) {
        const QPointF dir = name.endsWith(QLatin1String("left")) ? QPointF(-w, 0) : name.endsWith(QLatin1String("right")) ? QPointF(w, 0)
                            : name.endsWith(QLatin1String("up")) ? QPointF(0, -h) : QPointF(0, h);
        p.drawImage(0, 0, a);
        p.drawImage(dir * (e - 1), b);
    } else if (name == QLatin1String("zoom")) {
        const double z = u < 0.5 ? 1 + 1.6 * u : 1 + 1.6 * (1 - u);
        around(fx(near, QStringLiteral("zoom-blur"), 1.6 * peak), z, 0, 1);
    } else if (name == QLatin1String("whip")) {
        // One continuous fast pan from a to b, blurred hardest at the cut.
        const double blur = 2.5 * std::pow(peak, 0.7);
        p.drawImage(QPointF(-e * w, 0), fx(a, QStringLiteral("motion-blur"), blur));
        p.drawImage(QPointF((1 - e) * w, 0), fx(b, QStringLiteral("motion-blur"), blur));
    } else if (name == QLatin1String("spin")) {
        // Spin through the cut, scaled so the rotated frame always fills the screen.
        const double angle = u < 0.5 ? 120 * ease(QStringLiteral("ease-in"), 2 * u)
                                     : -120 * (1 - ease(QStringLiteral("ease-out"), 2 * u - 1));
        const double th = std::abs(angle) * M_PI / 180;
        const double cover = std::max((w * std::cos(th) + h * std::sin(th)) / w, (w * std::sin(th) + h * std::cos(th)) / h);
        around(fx(near, QStringLiteral("zoom-blur"), 1.2 * peak), cover * (1 + 0.15 * peak), angle, 1);
    } else if (name == QLatin1String("glitch")) {
        p.drawImage(0, 0, fx(near, QStringLiteral("glitch"), 1.5 * peak));
    } else if (name == QLatin1String("flash")) {
        p.drawImage(0, 0, near);
        p.fillRect(QRectF(0, 0, w, h), QColor(255, 255, 255, int(255 * std::clamp(peak * 1.2, 0.0, 1.0))));
    } else if (name == QLatin1String("wipe")) {
        p.drawImage(0, 0, a);
        p.save();
        p.setClipRect(QRectF(0, 0, w * e, h));
        p.drawImage(0, 0, b);
        p.restore();
    } else if (name == QLatin1String("circle")) {
        p.drawImage(0, 0, a);
        QPainterPath iris;
        const double r = std::hypot(w, h) / 2 * e;
        iris.addEllipse(QPointF(w / 2, h / 2), r, r);
        p.save();
        p.setClipPath(iris);
        p.drawImage(0, 0, b);
        p.restore();
    } else if (name == QLatin1String("blur")) {
        p.drawImage(0, 0, fx(a, QStringLiteral("blur"), 2.2 * std::min(1.0, 2 * u)));
        p.save();
        p.setOpacity(std::clamp((u - 0.3) / 0.4, 0.0, 1.0));
        p.drawImage(0, 0, fx(b, QStringLiteral("blur"), 2.2 * std::min(1.0, 2 * (1 - u))));
        p.restore();
    } else {  // crossfade
        p.drawImage(0, 0, a);
        p.save();
        p.setOpacity(e);
        p.drawImage(0, 0, b);
        p.restore();
    }
}

QImage Renderer::renderVideo(qint64 t, QSize size)
{
    ++m_tick;
    QImage canvas(size, QImage::Format_ARGB32_Premultiplied);
    const QColor background(m_draft.canvas.background);
    canvas.fill(background.isValid() ? background : QColor(Qt::black));

    QPainter p(&canvas);
    p.setRenderHint(QPainter::SmoothPixmapTransform);

    // Main track is the bottom layer; overlay tracks stack in order on top.
    for (const Track &track : m_draft.tracks) {
        if (!track.isVisual()) continue;
        if (track.role == TrackRole::Main) {
            drawMainTrack(p, track, t, size, canvas.pixelColor(0, 0));
            continue;
        }
        for (const Segment &seg : track.segments) {
            if (!seg.contains(t)) continue;
            const Material *mat = m_draft.material(seg.materialId);
            const FrameState fs = animate(seg, t - seg.start);
            if (mat && mat->kind == MaterialKind::Effect) {
                // Adjustment layer: re-process everything composited so far.
                if (!seg.effects.isEmpty()) {
                    if (!m_fx) m_fx = std::make_unique<EffectProcessor>();
                    p.end();
                    canvas = m_fx->applyMoment(canvas, seg.effects, (t - seg.start) / 1e6,
                                               seg.duration > 0 ? double(t - seg.start) / double(seg.duration) : 0);
                    p.begin(&canvas);
                    p.setRenderHint(QPainter::SmoothPixmapTransform);
                }
                break;
            }
            if (mat && mat->kind == MaterialKind::Text) {
                if (seg.effects.isEmpty()) {
                    drawText(p, seg, size, fs);
                } else {
                    // Text with effects (glow, glitch, RGB split...): render on its own layer first.
                    if (!m_fx) m_fx = std::make_unique<EffectProcessor>();
                    QImage layer(size, QImage::Format_ARGB32_Premultiplied);
                    layer.fill(Qt::transparent);
                    QPainter lp(&layer);
                    drawText(lp, seg, size, fs);
                    lp.end();
                    p.drawImage(0, 0, m_fx->apply(layer, seg.effects, (t - seg.start) / 1e6, true));
                }
                break;
            }
            if (mat) drawMedia(p, seg, *mat, t, size);
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
    if (!m_speechReady) {  // where someone talks, for ducking (found once per draft)
        bool ducks = false;
        for (const Track &t : m_draft.tracks)
            for (const Segment &s : t.segments) ducks |= s.duck > 0;
        m_speech = ducks ? speechSpans(m_draft) : QList<QPair<qint64, qint64>>{};
        m_speechReady = true;
        if (qEnvironmentVariableIsSet("MONTAGE_DEBUG"))
            for (const auto &sp : m_speech) qInfo("speech %.2f-%.2f", sp.first / 1e6, sp.second / 1e6);
    }

    for (const Track &track : m_draft.tracks) {
        for (const Segment &seg : track.segments) {
            const Material *mat = m_draft.material(seg.materialId);
            if (!mat || !mat->hasAudio || seg.volume <= 0.0) continue;

            const qint64 segStart = av_rescale(seg.start, kSampleRate, AV_TIME_BASE);
            const qint64 segEnd = av_rescale(seg.end(), kSampleRate, AV_TIME_BASE);
            const qint64 from = std::max(startSample, segStart);
            const qint64 to = std::min(endSample, segEnd);
            if (from >= to) continue;

            if (seg.speed <= 0) continue;  // freeze frames are silent
            AudioDecoder *dec = audioFor(seg, *mat);
            if (!dec) continue;
            const bool ducked = seg.duck > 0 && !m_speech.isEmpty();
            // Volume for a block centred on timeline sample `mid`: lower while someone speaks.
            auto gainAt = [&](qint64 mid) {
                if (!ducked) return float(seg.volume);
                const double dip = duckAmount(m_speech, av_rescale(mid, AV_TIME_BASE, kSampleRate));
                return float(seg.volume * (1 - (1 - seg.duck) * dip));
            };
            if (ducked && seg.speed == 1.0 && seg.curve.isEmpty()) {
                constexpr qint64 kBlock = 512;
                for (qint64 b = from; b < to; b += kBlock) {
                    const qint64 n = std::min(kBlock, to - b);
                    const qint64 srcSample = av_rescale(seg.sourceStart, kSampleRate, AV_TIME_BASE) + (b - segStart);
                    dec->mixInto(srcSample, int(n), out + (b - startSample) * kChannels, gainAt(b + n / 2));
                }
                continue;
            }
            if (seg.speed == 1.0 && seg.curve.isEmpty()) {
                const qint64 srcSample = av_rescale(seg.sourceStart, kSampleRate, AV_TIME_BASE) + (from - segStart);
                dec->mixInto(srcSample, int(to - from), out + (from - startSample) * kChannels, float(seg.volume));
                continue;
            }
            // Time-remapped audio: read the source span each block covers and
            // resample it to the block (pitch follows speed, like tape).
            constexpr qint64 kBlock = 512;
            for (qint64 b = from; b < to; b += kBlock) {
                const qint64 n = std::min(kBlock, to - b);
                const qint64 localA = av_rescale(b - segStart, AV_TIME_BASE, kSampleRate);
                const qint64 localB = av_rescale(b + n - segStart, AV_TIME_BASE, kSampleRate);
                const qint64 srcA = av_rescale(seg.sourceAt(localA), kSampleRate, AV_TIME_BASE);
                const qint64 srcB = av_rescale(seg.sourceAt(localB), kSampleRate, AV_TIME_BASE);
                const int m = int(std::clamp<qint64>(srcB - srcA, 1, kBlock * 32));
                m_resample.assign(size_t(m + 1) * kChannels, 0.0f);
                dec->mixInto(srcA, m + 1, m_resample.data(), 1.0f);
                float *dst = out + (b - startSample) * kChannels;
                const float gain = gainAt(b + n / 2);
                for (qint64 i = 0; i < n; ++i) {
                    const double pos = double(i) * m / double(n);
                    const int k = int(pos);
                    const float f = float(pos - k);
                    for (int c = 0; c < kChannels; ++c) {
                        const float v = m_resample[size_t(k) * kChannels + c] * (1 - f) + m_resample[size_t(k + 1) * kChannels + c] * f;
                        dst[i * kChannels + c] += v * gain;
                    }
                }
            }
        }
    }

    for (int i = 0; i < frames * kChannels; ++i) out[i] = std::clamp(out[i], -1.0f, 1.0f);
}

}  // namespace mtg
