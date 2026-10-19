#include "EditPlan.h"

#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <QPointF>
#include <QRegularExpression>
#include <cmath>
#include <limits>

namespace mtg::ai {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kEps = 0.01;

struct Aspect {
    const char *name;
    int w, h;
};
constexpr Aspect kAspects[] = {
    {"16:9", 1920, 1080}, {"9:16", 1080, 1920}, {"1:1", 1080, 1080},
    {"4:3", 1440, 1080},  {"3:4", 1080, 1440},  {"21:9", 2560, 1080},
};

const QStringList kTextStyles{"title", "subtitle", "caption", "lower-third", "end-card"};
const QStringList kTextPositions{"center", "top", "bottom", "left", "right",
                                 "top-left", "top-right", "bottom-left", "bottom-right", "custom"};
const QStringList kOverlayPositions{"full", "center", "top", "bottom", "left", "right",
                                    "top-left", "top-right", "bottom-left", "bottom-right", "custom"};

QString fmt(double v)
{
    return QString::number(std::round(v * 100) / 100, 'f', v == std::floor(v) ? 0 : 2);
}

double secs(qint64 us)
{
    return std::round(us / 1e4) / 100.0;  // centisecond precision keeps plans short
}

qint64 toUs(double s)
{
    return qint64(std::llround(s * 1e6));
}

bool near(double a, double b)
{
    return std::abs(a - b) < kEps;
}

// Text anchor positions in transform units (fractions of half the canvas).
bool textAnchor(const QString &name, Transform *t)
{
    static const QHash<QString, QPointF> anchors{
        {"center", {0, 0}},          {"top", {0, -0.72}},           {"bottom", {0, 0.72}},
        {"left", {-0.55, 0}},        {"right", {0.55, 0}},          {"top-left", {-0.55, -0.72}},
        {"top-right", {0.55, -0.72}}, {"bottom-left", {-0.55, 0.72}}, {"bottom-right", {0.55, 0.72}},
    };
    const auto it = anchors.constFind(name);
    if (it == anchors.constEnd()) return false;
    t->x = it->x();
    t->y = it->y();
    return true;
}

// Overlay anchors depend on the clip's fitted size so edges keep a margin.
bool overlayAnchor(const QString &name, const Material &m, const Canvas &c, double scale, Transform *t)
{
    double fw = 1, fh = 1;  // fitted size as a fraction of the canvas, before scaling
    if (m.width > 0 && m.height > 0) {
        const double ma = double(m.width) / m.height, ca = double(c.width) / c.height;
        if (ma > ca) fh = ca / ma;
        else fw = ma / ca;
    }
    const double hx = fw * scale, hy = fh * scale;
    const double mx = 0.05, my = 0.05 * c.width / c.height;
    const double right = std::max(0.0, 1 - hx - mx), down = std::max(0.0, 1 - hy - my);
    t->scale = scale;
    if (name == QLatin1String("full")) {
        *t = Transform{};
        return true;
    }
    t->x = name.endsWith(QLatin1String("left")) ? -right : name.endsWith(QLatin1String("right")) ? right : 0;
    t->y = name.startsWith(QLatin1String("top")) ? -down : name.startsWith(QLatin1String("bottom")) ? down : 0;
    return kOverlayPositions.contains(name) && name != QLatin1String("custom");
}

QJsonValue num(double v)
{
    return std::round(v * 100) / 100;
}

QString itemLabel(const char *list, int i)
{
    return QStringLiteral("%1[%2]").arg(QLatin1String(list)).arg(i);
}

}  // namespace

// ---------------------------------------------------------------- index

MediaIndex::MediaIndex(const Draft &d)
{
    for (const Material &m : d.materials)
        if (m.kind != MaterialKind::Text) m_ids << m.id;
}

QString MediaIndex::alias(const QString &materialId) const
{
    const qsizetype i = m_ids.indexOf(materialId);
    return i < 0 ? QString() : QStringLiteral("m%1").arg(i + 1);
}

QString MediaIndex::materialId(const QString &alias) const
{
    static const QRegularExpression re(QStringLiteral("^m(\\d+)$"));
    const auto match = re.match(alias.trimmed().toLower());
    if (!match.hasMatch()) return {};
    const int i = match.captured(1).toInt() - 1;
    return i >= 0 && i < m_ids.size() ? m_ids[i] : QString();
}

QString PlanReport::text() const
{
    QStringList lines;
    if (!errors.isEmpty()) lines << QStringLiteral("Errors (nothing was changed):") << ("- " + errors.join("\n- "));
    if (!warnings.isEmpty()) lines << QStringLiteral("Adjusted:") << ("- " + warnings.join("\n- "));
    return lines.join('\n');
}

double seconds(const QJsonValue &v)
{
    if (v.isDouble()) return v.toDouble();
    if (!v.isString()) return kNaN;
    QString s = v.toString().trimmed().toLower();
    if (s.endsWith(QLatin1Char('s'))) s.chop(1);
    if (s.contains(QLatin1Char(':'))) {
        double total = 0;
        for (const QString &part : s.split(QLatin1Char(':'))) {
            bool ok = false;
            const double p = part.toDouble(&ok);
            if (!ok) return kNaN;
            total = total * 60 + p;
        }
        return total;
    }
    bool ok = false;
    const double d = s.toDouble(&ok);
    return ok ? d : kNaN;
}

// ---------------------------------------------------------------- describe

QString describeMedia(const Draft &d, const MediaIndex &index)
{
    QStringList lines;
    for (const QString &id : index.materialIds()) {
        const Material *m = d.material(id);
        QString line = QStringLiteral("%1  %2  %3").arg(index.alias(id), kindName(m->kind), m->name);
        if (m->isTimed()) line += QStringLiteral("  %1s").arg(fmt(secs(m->durationUs)));
        if (m->width > 0) line += QStringLiteral("  %1x%2").arg(m->width).arg(m->height);
        if (m->kind == MaterialKind::Video) line += m->hasAudio ? QStringLiteral("  sound") : QStringLiteral("  no-sound");
        lines << line;
    }
    return lines.isEmpty() ? QStringLiteral("(no media imported yet)") : lines.join('\n');
}

QString summarize(const Draft &d, const MediaIndex &index)
{
    QString aspect = QStringLiteral("%1x%2").arg(d.canvas.width).arg(d.canvas.height);
    for (const Aspect &a : kAspects)
        if (a.w == d.canvas.width && a.h == d.canvas.height) aspect += QStringLiteral(" (%1)").arg(QLatin1String(a.name));
    QStringList out{QStringLiteral("Canvas %1, %2 fps. Length %3s.").arg(aspect, fmt(d.canvas.fps), fmt(secs(d.duration())))};

    QStringList main, overlays, audio, texts;
    for (const Track &t : d.tracks) {
        for (const Segment &s : t.segments) {
            const Material *m = d.material(s.materialId);
            if (!m) continue;
            const QString range = QStringLiteral("%1-%2s").arg(fmt(secs(s.start)), fmt(secs(s.end())));
            if (m->kind == MaterialKind::Text) {
                texts << QStringLiteral("  \"%1\" %2").arg(s.text.text, range);
                continue;
            }
            const QString src = m->isTimed() ? QStringLiteral(" (file %1-%2s)").arg(fmt(secs(s.sourceStart)), fmt(secs(s.sourceStart + s.duration)))
                                             : QString();
            const QString line = QStringLiteral("  %1 %2 %3%4").arg(index.alias(m->id), m->name, range, src);
            if (t.role == TrackRole::Main) main << line;
            else if (t.role == TrackRole::Overlay) overlays << line;
            else audio << line + QStringLiteral(" volume %1").arg(fmt(s.volume));
        }
    }
    out << QStringLiteral("Main:") << (main.isEmpty() ? QStringList{"  (empty)"} : main);
    if (!overlays.isEmpty()) out << QStringLiteral("Overlays:") << overlays;
    if (!audio.isEmpty()) out << QStringLiteral("Audio:") << audio;
    if (!texts.isEmpty()) out << QStringLiteral("Texts:") << texts;
    return out.join('\n');
}

// ---------------------------------------------------------------- draft -> plan

static void putVisualExtras(QJsonObject &o, const Segment &s, bool withTransform)
{
    if (!near(s.volume, 1)) o.insert("volume", num(s.volume));
    if (!near(s.opacity, 1)) o.insert("opacity", num(s.opacity));
    if (withTransform) {
        if (!near(s.transform.scale, 1)) o.insert("scale", num(s.transform.scale));
        if (!near(s.transform.x, 0)) o.insert("x", num(s.transform.x));
        if (!near(s.transform.y, 0)) o.insert("y", num(s.transform.y));
    }
}

static QJsonObject textItem(const Segment &s)
{
    QJsonObject o{{"text", s.text.text}, {"at", num(secs(s.start))}, {"duration", num(secs(s.duration))}};

    // Pick the preset that matches the most style fields, then list the differences.
    QString best = QStringLiteral("title");
    int bestScore = -1;
    TextStyle bestStyle;
    Transform bestPos;
    for (const QString &name : kTextStyles) {
        TextStyle st;
        Transform tr;
        applyTextPreset(name, &st, &tr);
        const int score = int(near(st.size, s.text.size)) + int(st.color.compare(s.text.color, Qt::CaseInsensitive) == 0)
                          + int(st.bold == s.text.bold) + int(near(st.outline, s.text.outline))
                          + int(st.background.compare(s.text.background, Qt::CaseInsensitive) == 0)
                          + int(st.align == s.text.align) + int(near(tr.x, s.transform.x) && near(tr.y, s.transform.y));
        if (score > bestScore) {
            bestScore = score;
            best = name;
            bestStyle = st;
            bestPos = tr;
        }
    }
    o.insert("style", best);
    if (!near(bestPos.x, s.transform.x) || !near(bestPos.y, s.transform.y)) {
        QString anchor = QStringLiteral("custom");
        for (const QString &name : kTextPositions) {
            Transform t;
            if (textAnchor(name, &t) && near(t.x, s.transform.x) && near(t.y, s.transform.y)) anchor = name;
        }
        o.insert("position", anchor);
        if (anchor == QLatin1String("custom")) {
            o.insert("x", num(s.transform.x));
            o.insert("y", num(s.transform.y));
        }
    }
    if (bestStyle.color.compare(s.text.color, Qt::CaseInsensitive) != 0) o.insert("color", s.text.color.toUpper());
    if (!near(bestStyle.size, s.text.size)) o.insert("size", std::round(s.text.size * 1000) / 1000);
    if (bestStyle.bold != s.text.bold) o.insert("bold", s.text.bold);
    if (!near(bestStyle.outline, s.text.outline)) o.insert("outline", num(s.text.outline));
    if (bestStyle.background.compare(s.text.background, Qt::CaseInsensitive) != 0) o.insert("box", s.text.background);
    if (bestStyle.align != s.text.align) o.insert("align", s.text.align);
    if (bestStyle.font != s.text.font) o.insert("font", s.text.font);
    if (!near(s.opacity, 1)) o.insert("opacity", num(s.opacity));
    if (!near(s.transform.scale, 1)) o.insert("scale", num(s.transform.scale));
    return o;
}

QJsonObject planFromDraft(const Draft &d, const MediaIndex &index)
{
    QJsonArray main, overlays, audio, texts;
    for (const Track &t : d.tracks) {
        for (const Segment &s : t.segments) {
            const Material *m = d.material(s.materialId);
            if (!m) continue;
            if (m->kind == MaterialKind::Text) {
                texts.append(textItem(s));
                continue;
            }
            const double from = m->isTimed() ? secs(s.sourceStart) : 0;
            const double to = from + secs(s.duration);
            QJsonObject o{{"media", index.alias(m->id)}, {"from", num(from)}, {"to", num(to)}};
            if (t.role == TrackRole::Main) {
                putVisualExtras(o, s, true);
                main.append(o);
            } else if (t.role == TrackRole::Overlay) {
                o.insert("at", num(secs(s.start)));
                QString anchor = QStringLiteral("custom");
                for (const QString &name : kOverlayPositions) {
                    Transform tr;
                    if (overlayAnchor(name, *m, d.canvas, s.transform.scale, &tr) && near(tr.x, s.transform.x)
                        && near(tr.y, s.transform.y) && near(tr.scale, s.transform.scale)) {
                        anchor = name;
                        break;
                    }
                }
                o.insert("position", anchor);
                if (!near(s.transform.scale, 1)) o.insert("scale", num(s.transform.scale));
                if (anchor == QLatin1String("custom")) {
                    o.insert("x", num(s.transform.x));
                    o.insert("y", num(s.transform.y));
                }
                if (!near(s.volume, 1)) o.insert("volume", num(s.volume));
                if (!near(s.opacity, 1)) o.insert("opacity", num(s.opacity));
                overlays.append(o);
            } else {
                o.insert("at", num(secs(s.start)));
                o.insert("volume", num(s.volume));
                audio.append(o);
            }
        }
    }

    QString aspect = d.canvasLocked ? QStringLiteral("keep") : QStringLiteral("original");
    for (const Aspect &a : kAspects)
        if (d.canvasLocked && a.w == d.canvas.width && a.h == d.canvas.height) aspect = QString::fromLatin1(a.name);

    QJsonObject plan{{"reply", ""}, {"aspect", aspect}, {"main", main}, {"overlays", overlays},
                     {"audio", audio}, {"texts", texts}, {"export", false}};
    if (QColor(d.canvas.background) != QColor(Qt::black)) plan.insert("background", d.canvas.background.toUpper());
    return plan;
}

// ---------------------------------------------------------------- plan -> draft

namespace {

struct Builder {
    const Draft &current;
    const MediaIndex &index;
    PlanReport *report;
    Draft out;

    const Material *media(const QJsonObject &item, const QString &where)
    {
        const QString ref = item.value("media").toString().trimmed();
        QString id = index.materialId(ref);
        if (id.isEmpty()) {  // tolerate a file name instead of an id
            for (const QString &mid : index.materialIds())
                if (current.material(mid)->name.compare(ref, Qt::CaseInsensitive) == 0) id = mid;
        }
        if (id.isEmpty()) {
            QStringList valid;
            for (const QString &mid : index.materialIds()) valid << index.alias(mid);
            report->errors << QStringLiteral("%1: unknown media \"%2\". Use one of: %3.")
                                  .arg(where, ref, valid.isEmpty() ? QStringLiteral("(none - import media first)") : valid.join(", "));
            return nullptr;
        }
        return current.material(id);
    }

    // from/to (or from/duration) inside the file, in seconds. Returns false on error.
    bool range(const QJsonObject &item, const Material &m, const QString &where, double defaultLen, double *from, double *to)
    {
        const QString alias = index.alias(m.id);
        double f = item.contains("from") ? seconds(item.value("from")) : 0;
        double t = item.contains("to") ? seconds(item.value("to")) : kNaN;
        if (std::isnan(t) && item.contains("duration")) t = f + seconds(item.value("duration"));
        if (std::isnan(f)) {
            report->errors << QStringLiteral("%1: \"from\" must be a number of seconds.").arg(where);
            return false;
        }
        const double len = m.isTimed() ? secs(m.durationUs) : 0;
        if (std::isnan(t)) t = m.isTimed() ? (defaultLen > 0 ? std::min(len, f + defaultLen) : len) : f + (defaultLen > 0 ? defaultLen : 3);
        if (f < 0) f = 0;
        if (m.isTimed() && f >= len) {
            // A start past the end is almost always a time in the finished video;
            // use the same length from the start of the file.
            const double want = std::min(len, (t - f) >= 0.3 ? t - f : std::min(len, 3.0));
            report->warnings << QStringLiteral("%1: \"from\" %2 is past the end of %3 (%4s long); used its first %5s instead.")
                                    .arg(where, fmt(f), alias, fmt(len), fmt(want));
            f = 0;
            t = want;
        }
        if (m.isTimed() && t > len + 0.05) {
            report->warnings << QStringLiteral("%1: %2 is only %3s long, so \"to\" was cut from %4 to %3.")
                                    .arg(where, alias, fmt(len), fmt(t));
            t = len;
        }
        if (t - f < 0.1) {
            report->errors << QStringLiteral("%1: \"to\" (%2) must be at least 0.1s after \"from\" (%3).")
                                  .arg(where, fmt(t), fmt(f));
            return false;
        }
        *from = f;
        *to = t;
        return true;
    }

    double optNumber(const QJsonObject &item, const char *key, double fallback, double lo, double hi)
    {
        if (!item.contains(QLatin1String(key))) return fallback;
        const double v = seconds(item.value(QLatin1String(key)));
        return std::isnan(v) ? fallback : std::clamp(v, lo, hi);
    }

    void placeOn(TrackRole role, const Segment &seg)
    {
        for (Track &t : out.tracks) {
            if (t.role == role && t.fits(seg.start, seg.end())) {
                t.segments.append(seg);
                return;
            }
        }
        Track fresh;
        fresh.id = Draft::newId();
        fresh.role = role;
        fresh.segments.append(seg);
        out.tracks.append(fresh);
    }
};

}  // namespace

Draft draftFromPlan(const Draft &current, const QJsonObject &plan, const MediaIndex &index, PlanReport *report)
{
    Builder b{current, index, report, current};
    Draft &out = b.out;
    out.tracks.clear();
    Track main;
    main.id = current.tracks.isEmpty() ? Draft::newId() : current.tracks.first().id;
    out.tracks.append(main);

    // Canvas
    const QString aspect = plan.value("aspect").toString(QStringLiteral("keep")).trimmed().toLower();
    if (aspect == QLatin1String("original")) {
        out.canvasLocked = false;
    } else if (aspect != QLatin1String("keep") && !aspect.isEmpty()) {
        bool found = false;
        for (const Aspect &a : kAspects) {
            if (aspect == QLatin1String(a.name)) {
                out.canvas.width = a.w;
                out.canvas.height = a.h;
                out.canvasLocked = true;
                found = true;
            }
        }
        if (!found) report->warnings << QStringLiteral("aspect \"%1\" is not supported; kept the current shape.").arg(aspect);
    }

    if (plan.contains("background")) {
        QString name = plan.value("background").toString().trimmed().toLower();
        name.replace(QLatin1String("grey"), QLatin1String("gray"));
        QColor bg(name);
        if (!bg.isValid()) bg = QColor(name.remove(QLatin1Char(' ')));  // "dark blue" -> darkblue
        if (bg.isValid()) out.canvas.background = bg.name().toUpper();
        else report->warnings << QStringLiteral("background \"%1\" is not a colour like #1E3A8A; kept the current one.")
                                     .arg(plan.value("background").toString());
    }

    // Main track, back to back
    QJsonArray mainItems = plan.value("main").toArray();

    // Small models often write main clips' from/to as times in the finished
    // video (0-5, 5-10, 10-15) instead of times inside each file. If every
    // "from" equals the running total and that runs past a file's end, read
    // the values as lengths starting at each file's beginning.
    if (mainItems.size() >= 2) {
        bool cumulative = true, pastEnd = false;
        double total = 0;
        for (int i = 0; i < mainItems.size() && cumulative; ++i) {
            const QJsonObject item = mainItems[i].toObject();
            const double f = seconds(item.value("from")), t = seconds(item.value("to"));
            if (std::isnan(f) || std::isnan(t) || t <= f || std::abs(f - total) > 0.05) cumulative = false;
            const QString id = index.materialId(item.value("media").toString());
            const Material *m = id.isEmpty() ? nullptr : current.material(id);
            if (m && m->isTimed() && t > secs(m->durationUs) + 0.05 && t - f <= secs(m->durationUs) + 0.05) pastEnd = true;
            total += t - f;
        }
        if (cumulative && pastEnd) {
            for (int i = 0; i < mainItems.size(); ++i) {
                QJsonObject item = mainItems[i].toObject();
                const double f = seconds(item.value("from")), t = seconds(item.value("to"));
                item.insert("from", 0);
                item.insert("to", t - f);
                mainItems[i] = item;
            }
            report->warnings << QStringLiteral("\"main\" used times in the video as file positions; each clip now starts at the "
                                               "beginning of its own file with the same length.");
        }
    }
    qint64 cursor = 0;
    for (int i = 0; i < mainItems.size(); ++i) {
        const QJsonObject item = mainItems[i].toObject();
        const QString where = itemLabel("main", i);
        const Material *m = b.media(item, where);
        if (!m) continue;
        if (m->kind == MaterialKind::Audio) {
            report->errors << QStringLiteral("%1: %2 is an audio file; put it in \"audio\", not \"main\".").arg(where, index.alias(m->id));
            continue;
        }
        double from = 0, to = 0;
        if (!b.range(item, *m, where, 0, &from, &to)) continue;
        Segment s;
        s.id = Draft::newId();
        s.materialId = m->id;
        s.start = cursor;
        s.duration = toUs(to - from);
        s.sourceStart = m->isTimed() ? toUs(from) : 0;
        s.volume = b.optNumber(item, "volume", 1, 0, 2);
        s.opacity = b.optNumber(item, "opacity", 1, 0, 1);
        s.transform.scale = b.optNumber(item, "scale", 1, 0.05, 8);
        s.transform.x = b.optNumber(item, "x", 0, -2, 2);
        s.transform.y = b.optNumber(item, "y", 0, -2, 2);
        out.tracks[0].segments.append(s);
        cursor += s.duration;
    }
    const double videoLen = secs(cursor);

    // Keeps a timed item inside the video. Something placed at or after the
    // end ("the logo at the end") moves back so it ends with the video.
    auto fitToVideo = [&](const QString &where, double &at, double *len) {
        if (videoLen <= 0) return true;
        if (at >= videoLen - 0.05) {
            *len = std::min(*len, videoLen);
            report->warnings << QStringLiteral("%1 started at %2s, after the video ends (%3s); it now ends with the video.")
                                    .arg(where, fmt(at), fmt(videoLen));
            at = videoLen - *len;
            return true;
        }
        if (at + *len > videoLen + 0.05) {
            report->warnings << QStringLiteral("%1 ran past the end of the video; it now ends at %2s.").arg(where, fmt(videoLen));
            *len = videoLen - at;
        }
        return true;
    };

    // Overlays
    const QJsonArray overlayItems = plan.value("overlays").toArray();
    for (int i = 0; i < overlayItems.size(); ++i) {
        const QJsonObject item = overlayItems[i].toObject();
        const QString where = itemLabel("overlays", i);
        const Material *m = b.media(item, where);
        if (!m) continue;
        if (m->kind == MaterialKind::Audio) {
            report->errors << QStringLiteral("%1: %2 is an audio file; put it in \"audio\".").arg(where, index.alias(m->id));
            continue;
        }
        double from = 0, to = 0;
        if (!b.range(item, *m, where, 3, &from, &to)) continue;
        double at = std::max(0.0, b.optNumber(item, "at", 0, 0, 1e6));
        double len = to - from;
        if (!fitToVideo(where, at, &len)) continue;
        Segment s;
        s.id = Draft::newId();
        s.materialId = m->id;
        s.start = toUs(at);
        s.duration = toUs(len);
        s.sourceStart = m->isTimed() ? toUs(from) : 0;
        s.volume = b.optNumber(item, "volume", 1, 0, 2);
        s.opacity = b.optNumber(item, "opacity", 1, 0, 1);
        const double scale = b.optNumber(item, "scale", 0.4, 0.05, 8);
        const QString pos = item.value("position").toString(QStringLiteral("top-right")).trimmed().toLower();
        if (!overlayAnchor(pos, *m, out.canvas, scale, &s.transform)) {
            s.transform.scale = scale;
            s.transform.x = b.optNumber(item, "x", 0, -2, 2);
            s.transform.y = b.optNumber(item, "y", 0, -2, 2);
            if (pos != QLatin1String("custom"))
                report->warnings << QStringLiteral("%1: unknown position \"%2\"; used x/y (or the centre).").arg(where, pos);
        }
        b.placeOn(TrackRole::Overlay, s);
    }

    // Audio
    const QJsonArray audioItems = plan.value("audio").toArray();
    for (int i = 0; i < audioItems.size(); ++i) {
        const QJsonObject item = audioItems[i].toObject();
        const QString where = itemLabel("audio", i);
        const Material *m = b.media(item, where);
        if (!m) continue;
        if (!m->hasAudio) {
            report->errors << QStringLiteral("%1: %2 has no sound.").arg(where, index.alias(m->id));
            continue;
        }
        double at = std::max(0.0, b.optNumber(item, "at", 0, 0, 1e6));
        double from = 0, to = 0;
        if (!b.range(item, *m, where, videoLen > 0 ? videoLen - at : 0, &from, &to)) continue;
        double len = to - from;
        if (!fitToVideo(where, at, &len)) continue;
        Segment s;
        s.id = Draft::newId();
        s.materialId = m->id;
        s.start = toUs(at);
        s.duration = toUs(len);
        s.sourceStart = toUs(from);
        s.volume = b.optNumber(item, "volume", 1, 0, 2);
        b.placeOn(TrackRole::Audio, s);
    }

    // Texts
    const QJsonArray textItems = plan.value("texts").toArray();
    for (int i = 0; i < textItems.size(); ++i) {
        const QJsonObject item = textItems[i].toObject();
        const QString where = itemLabel("texts", i);
        const QString text = item.value("text").toString().trimmed();
        if (text.isEmpty()) {
            report->warnings << QStringLiteral("%1 had no text; left out.").arg(where);
            continue;
        }
        double at = std::max(0.0, b.optNumber(item, "at", 0, 0, 1e6));
        double len = b.optNumber(item, "duration", 3, 0.1, 1e6);
        if (!fitToVideo(where, at, &len)) continue;

        Segment s;
        s.id = Draft::newId();
        s.materialId = kTextMaterialId;
        s.start = toUs(at);
        s.duration = toUs(len);
        s.text.text = text;
        const QString style = item.value("style").toString(QStringLiteral("title")).trimmed().toLower();
        if (!applyTextPreset(style, &s.text, &s.transform)) {
            applyTextPreset(QStringLiteral("title"), &s.text, &s.transform);
            report->warnings << QStringLiteral("%1: unknown style \"%2\"; used title.").arg(where, style);
        }
        const QString pos = item.value("position").toString().trimmed().toLower();
        if (pos == QLatin1String("custom") || (pos.isEmpty() && (item.contains("x") || item.contains("y")))) {
            s.transform.x = b.optNumber(item, "x", s.transform.x, -2, 2);
            s.transform.y = b.optNumber(item, "y", s.transform.y, -2, 2);
        } else if (!pos.isEmpty() && !textAnchor(pos, &s.transform)) {
            report->warnings << QStringLiteral("%1: unknown position \"%2\"; kept the style's position.").arg(where, pos);
        }
        if (item.contains("color")) s.text.color = item.value("color").toString(s.text.color);
        if (item.contains("size")) s.text.size = b.optNumber(item, "size", s.text.size, 0.01, 0.5);
        if (item.contains("bold")) s.text.bold = item.value("bold").toBool(s.text.bold);
        if (item.contains("outline")) s.text.outline = b.optNumber(item, "outline", s.text.outline, 0, 0.3);
        if (item.contains("box")) s.text.background = item.value("box").toString();
        if (item.contains("align")) s.text.align = item.value("align").toString(s.text.align);
        if (item.contains("font")) s.text.font = item.value("font").toString(s.text.font);
        s.opacity = b.optNumber(item, "opacity", 1, 0, 1);
        s.transform.scale = b.optNumber(item, "scale", 1, 0.05, 8);
        out.ensureTextMaterial();
        b.placeOn(TrackRole::Overlay, s);
    }


    out.normalize();
    return out;
}

// ---------------------------------------------------------------- schema

QJsonObject planSchema()
{
    auto obj = [](QJsonObject props, QJsonArray required) {
        return QJsonObject{{"type", "object"}, {"additionalProperties", false}, {"properties", props}, {"required", required}};
    };
    auto number = [] { return QJsonObject{{"type", "number"}}; };
    auto string = [] { return QJsonObject{{"type", "string"}}; };
    auto choice = [](const QStringList &values) { return QJsonObject{{"type", "string"}, {"enum", QJsonArray::fromStringList(values)}}; };
    auto array = [](QJsonObject items) { return QJsonObject{{"type", "array"}, {"items", items}}; };

    const QJsonObject clip = obj({{"media", string()}, {"from", number()}, {"to", number()}, {"volume", number()},
                                  {"opacity", number()}, {"scale", number()}, {"x", number()}, {"y", number()}},
                                 {"media", "from", "to"});
    const QJsonObject overlay = obj({{"media", string()}, {"at", number()}, {"from", number()}, {"to", number()},
                                     {"position", choice(kOverlayPositions)}, {"scale", number()}, {"x", number()},
                                     {"y", number()}, {"volume", number()}, {"opacity", number()}},
                                    {"media", "at", "from", "to", "position"});
    const QJsonObject audio = obj({{"media", string()}, {"at", number()}, {"from", number()}, {"to", number()},
                                   {"volume", number()}},
                                  {"media", "at", "from", "to", "volume"});
    const QJsonObject text = obj({{"text", string()}, {"at", number()}, {"duration", number()},
                                  {"style", choice(kTextStyles)}, {"position", choice(kTextPositions)},
                                  {"color", string()}, {"size", number()}, {"bold", QJsonObject{{"type", "boolean"}}},
                                  {"outline", number()}, {"box", string()}, {"align", choice({"left", "center", "right"})},
                                  {"font", string()}, {"x", number()}, {"y", number()}, {"opacity", number()},
                                  {"scale", number()}},
                                 {"text", "at", "duration", "style"});

    return obj({{"reply", string()},
                {"aspect", choice({"keep", "original", "16:9", "9:16", "1:1", "4:3", "3:4", "21:9"})},
                {"main", array(clip)},
                {"overlays", array(overlay)},
                {"audio", array(audio)},
                {"texts", array(text)},
                {"background", string()},
                {"export", QJsonObject{{"type", "boolean"}}}},
               {"reply", "aspect", "main", "overlays", "audio", "texts", "export"});
}

}  // namespace mtg::ai
