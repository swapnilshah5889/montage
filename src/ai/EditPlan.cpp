#include "EditPlan.h"

#include "AdStyles.h"
#include "Generators.h"
#include "engine/Animation.h"
#include "engine/Beats.h"
#include "engine/Effects.h"

#include <QColor>
#include <QFileInfo>
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
    {"4:3", 1440, 1080},  {"3:4", 1080, 1440},  {"21:9", 2560, 1080}, {"4:5", 1080, 1350},
};

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

const QStringList kKeyProps{"x", "y", "scale", "rotation", "opacity"};

// Motion, rotation and keyframes of a segment, written only when set.
QJsonArray effectsToPlan(const QList<Effect> &fx)
{
    QJsonArray a;
    for (const Effect &e : fx) {
        QJsonObject o{{"name", e.name}};
        if (!near(e.amount, 1)) o.insert("amount", num(e.amount));
        if (!e.path.isEmpty()) o.insert("path", e.path);
        a.append(o);
    }
    return a;
}

// `defaults` is the motion a text preset brings along: matching slots are
// left out, and a removed default is written as "none".
void putAnimation(QJsonObject &o, const Segment &s, const Motion &defaults = Motion{}, double baseRotation = 0)
{
    if (!s.effects.isEmpty()) o.insert("effects", effectsToPlan(s.effects));
    if (!near(s.transform.rotation, baseRotation)) o.insert("rotation", num(s.transform.rotation));
    const Motion &m = s.motion;
    auto slot = [&](const char *key, const QString &value, const QString &def) {
        if (value == def) return false;
        o.insert(QLatin1String(key), value.isEmpty() ? QStringLiteral("none") : value);
        return !value.isEmpty();
    };
    if ((slot("in", m.in, defaults.in) || m.in == defaults.in) && !m.in.isEmpty() && m.inUs != defaults.inUs)
        o.insert("in_duration", num(m.inUs / 1e6));
    if ((slot("out", m.out, defaults.out) || m.out == defaults.out) && !m.out.isEmpty() && m.outUs != defaults.outUs)
        o.insert("out_duration", num(m.outUs / 1e6));
    if ((slot("loop", m.loop, defaults.loop) || m.loop == defaults.loop) && !m.loop.isEmpty() && !near(m.loopSpeed, defaults.loopSpeed))
        o.insert("loop_speed", num(m.loopSpeed));
    if (!s.keys.isEmpty()) {
        QJsonObject keys;
        for (auto it = s.keys.cbegin(); it != s.keys.cend(); ++it) {
            QJsonArray pts;
            for (const KeyPoint &k : it.value())
                pts.append(QJsonObject{{"t", num(k.t / 1e6)}, {"v", num(k.v)}, {"ease", k.ease}});
            keys.insert(it.key(), pts);
        }
        o.insert("keys", keys);
    }
}

QString colorName(const QJsonValue &v)
{
    QString name = v.toString().trimmed().toLower();
    if (name.isEmpty()) return {};
    QColor c(name);
    if (!c.isValid()) c = QColor(name.remove(QLatin1Char(' ')));
    return c.isValid() ? c.name(c.alpha() < 255 ? QColor::HexArgb : QColor::HexRgb).toUpper() : QString();
}

}  // namespace

// ---------------------------------------------------------------- index

MediaIndex::MediaIndex(const Draft &d)
{
    for (const Material &m : d.materials)
        if (m.kind != MaterialKind::Text && m.kind != MaterialKind::Effect) m_ids << m.id;
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

    QStringList main, overlays, audio, texts, moments;
    for (const Track &t : d.tracks) {
        for (const Segment &s : t.segments) {
            const Material *m = d.material(s.materialId);
            if (!m) continue;
            const QString range = QStringLiteral("%1-%2s").arg(fmt(secs(s.start)), fmt(secs(s.end())));
            if (m->kind == MaterialKind::Text) {
                texts << QStringLiteral("  \"%1\" %2").arg(s.text.text, range);
                continue;
            }
            if (m->kind == MaterialKind::Effect) {
                QStringList names;
                for (const Effect &e : s.effects) names << e.name;
                moments << QStringLiteral("  %1 %2").arg(names.join("+"), range);
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
    if (!moments.isEmpty()) out << QStringLiteral("Moments:") << moments;
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
    Motion bestMotion;
    for (const QString &name : textPresetNames()) {
        TextStyle st;
        Transform tr;
        Motion mv;
        applyTextPreset(name, &st, &tr, &mv);
        const int score = int(near(st.size, s.text.size)) + int(st.color.compare(s.text.color, Qt::CaseInsensitive) == 0)
                          + int(st.bold == s.text.bold) + int(near(st.outline, s.text.outline))
                          + int(st.background.compare(s.text.background, Qt::CaseInsensitive) == 0)
                          + int(st.align == s.text.align) + int(near(tr.x, s.transform.x) && near(tr.y, s.transform.y))
                          + int(st.shape == s.text.shape) + int(st.shadow.compare(s.text.shadow, Qt::CaseInsensitive) == 0)
                          + int(st.gradient.compare(s.text.gradient, Qt::CaseInsensitive) == 0)
                          + int(near(tr.rotation, s.transform.rotation)) + 2 * int(mv == s.motion);
        if (score > bestScore) {
            bestScore = score;
            best = name;
            bestStyle = st;
            bestPos = tr;
            bestMotion = mv;
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
    if (bestStyle.shadow.compare(s.text.shadow, Qt::CaseInsensitive) != 0) o.insert("shadow", s.text.shadow.isEmpty() ? QStringLiteral("none") : s.text.shadow.toUpper());
    if (bestStyle.gradient.compare(s.text.gradient, Qt::CaseInsensitive) != 0) o.insert("gradient", s.text.gradient.isEmpty() ? QStringLiteral("none") : s.text.gradient.toUpper());
    if (bestStyle.shape != s.text.shape) o.insert("shape", s.text.shape.isEmpty() ? QStringLiteral("rounded") : s.text.shape);
    putAnimation(o, s, bestMotion, bestPos.rotation);
    return o;
}

QJsonObject planFromDraft(const Draft &d, const MediaIndex &index)
{
    QJsonArray main, overlays, audio, texts, moments;
    for (const Track &t : d.tracks) {
        for (const Segment &s : t.segments) {
            const Material *m = d.material(s.materialId);
            if (!m) continue;
            if (m->kind == MaterialKind::Text) {
                if (s.origin != QLatin1String("captions")) texts.append(textItem(s));  // auto captions: see "captions"
                continue;
            }
            if (m->kind == MaterialKind::Effect) {
                if (s.effects.isEmpty()) continue;
                QJsonObject o{{"effect", s.effects.first().name}, {"at", num(secs(s.start))}, {"duration", num(secs(s.duration))}};
                if (!near(s.effects.first().amount, 1)) o.insert("amount", num(s.effects.first().amount));
                if (s.effects.size() > 1) o.insert("effects", effectsToPlan(s.effects.mid(1)));
                moments.append(o);
                continue;
            }
            const double from = m->isTimed() ? secs(s.sourceStart) : 0;
            const double to = s.speed > 0 ? from + secs(m->isTimed() ? s.sourceSpan() : s.duration) : from;
            QJsonObject o{{"media", index.alias(m->id)}, {"from", num(from)}, {"to", num(to)}};
            if (m->kind == MaterialKind::Video && s.speed != 1.0) {
                o.insert("speed", num(s.speed));
                if (s.speed == 0) o.insert("duration", num(secs(s.duration)));
            }
            if (!s.curve.isEmpty()) o.insert("speed_curve", s.curve);
            if (t.role == TrackRole::Main) {
                putVisualExtras(o, s, true);
                putAnimation(o, s);
                if (!s.transition.name.isEmpty()) {
                    o.insert("transition", s.transition.name);
                    if (s.transition.us != Transition{}.us) o.insert("transition_duration", num(s.transition.us / 1e6));
                } else if (!d.style.isEmpty() && !main.isEmpty()) {
                    o.insert("transition", "none");  // a hard cut someone chose; a restyle keeps it
                }
                if (s.framing != QLatin1String("auto")) o.insert("framing", s.framing);
                if (s.speech) o.insert("speech", true);
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
                putAnimation(o, s);
                overlays.append(o);
            } else {
                o.insert("at", num(secs(s.start)));
                o.insert("volume", num(s.volume));
                if (s.speech) o.insert("speech", true);
                if (s.duck > 0) o.insert("duck", num(s.duck));
                audio.append(o);
            }
        }
    }

    QString aspect = d.canvasLocked ? QStringLiteral("keep") : QStringLiteral("original");
    for (const Aspect &a : kAspects)
        if (d.canvasLocked && a.w == d.canvas.width && a.h == d.canvas.height) aspect = QString::fromLatin1(a.name);

    QJsonObject plan{{"reply", ""}, {"aspect", aspect}, {"main", main}, {"overlays", overlays},
                     {"audio", audio}, {"texts", texts}, {"export", false}};
    if (!moments.isEmpty()) plan.insert("moments", moments);
    if (QColor(d.canvas.background) != QColor(Qt::black)) plan.insert("background", d.canvas.background.toUpper());
    if (!d.style.isEmpty()) plan.insert("style", d.style);
    if (!d.beatSync.isEmpty()) plan.insert("beat_sync", d.beatSync);
    if (!d.captions.isEmpty()) plan.insert("captions", d.captions);
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

    // Source range plus speed for a video/photo clip. Freeze frames
    // ("speed": 0) hold "from" for "duration" seconds. Returns false on error.
    bool timing(const QJsonObject &item, const Material &m, const QString &where, double defaultLen, Segment &s)
    {
        double speed = m.kind == MaterialKind::Video ? optNumber(item, "speed", 1, 0, 8) : 1;
        if (speed > 0 && speed < 0.1) speed = 0.1;
        double from = 0, to = 0;
        if (speed == 0) {
            const double len = secs(m.durationUs);
            from = std::clamp(item.contains("from") ? seconds(item.value("from")) : 0.0, 0.0, std::max(0.0, len - 0.04));
            if (std::isnan(from)) from = 0;
            s.duration = toUs(optNumber(item, "duration", 2, 0.1, 30));
        } else {
            if (!range(item, m, where, defaultLen, &from, &to)) return false;
            s.duration = toUs((to - from) / speed);
        }
        s.sourceStart = m.isTimed() ? toUs(from) : 0;
        s.speed = speed;
        const QString curve = item.value("speed_curve").toString().trimmed().toLower();
        if (!curve.isEmpty() && curve != QLatin1String("none")) {
            if (speed > 0 && speedCurveNames().contains(curve)) s.curve = curve;
            else if (speed > 0)
                report->warnings << QStringLiteral("%1: unknown speed_curve \"%2\"; use one of %3.").arg(where, curve, speedCurveNames().join(", "));
        }
        return true;
    }

    double optNumber(const QJsonObject &item, const char *key, double fallback, double lo, double hi)
    {
        if (!item.contains(QLatin1String(key))) return fallback;
        const double v = seconds(item.value(QLatin1String(key)));
        return std::isnan(v) ? fallback : std::clamp(v, lo, hi);
    }

    // Motion presets, rotation and keyframes from a plan item.
    void animation(const QJsonObject &item, Segment &s, const QString &where)
    {
        s.transform.rotation = optNumber(item, "rotation", s.transform.rotation, -720, 720);
        auto motion = [&](const char *slot, QString *name) {
            const QString v = item.value(QLatin1String(slot)).toString().trimmed().toLower();
            if (v.isEmpty()) return;
            if (v == QLatin1String("none")) {
                name->clear();
                return;
            }
            if (motionNames(QLatin1String(slot)).contains(v)) *name = v;
            else
                report->warnings << QStringLiteral("%1: unknown \"%2\" animation \"%3\"; use one of %4.")
                                        .arg(where, QLatin1String(slot), v, motionNames(QLatin1String(slot)).join(", "));
        };
        motion("in", &s.motion.in);
        motion("out", &s.motion.out);
        motion("loop", &s.motion.loop);
        if (item.contains("in_duration")) s.motion.inUs = toUs(optNumber(item, "in_duration", 0.5, 0.05, 10));
        if (item.contains("out_duration")) s.motion.outUs = toUs(optNumber(item, "out_duration", 0.4, 0.05, 10));
        if (item.contains("loop_speed")) s.motion.loopSpeed = optNumber(item, "loop_speed", 1, 0.05, 10);

        const QJsonObject keys = item.value("keys").toObject();
        for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) {
            if (!kKeyProps.contains(it.key())) {
                report->warnings << QStringLiteral("%1: can't keyframe \"%2\"; use %3.").arg(where, it.key(), kKeyProps.join(", "));
                continue;
            }
            QList<KeyPoint> pts;
            for (const QJsonValue &pv : it.value().toArray()) {
                double t = kNaN, v = kNaN;
                QString e;
                if (pv.isArray()) {  // [t, v, ease]
                    const QJsonArray a = pv.toArray();
                    t = seconds(a.at(0));
                    v = seconds(a.at(1));
                    e = a.at(2).toString();
                } else {
                    const QJsonObject o = pv.toObject();
                    t = seconds(o.value("t"));
                    v = seconds(o.value("v"));
                    e = o.value("ease").toString();
                }
                if (std::isnan(t) || std::isnan(v)) continue;
                if (!easeNames().contains(e)) e = QStringLiteral("ease-in-out");
                pts.append(KeyPoint{std::clamp<qint64>(toUs(t), 0, s.duration), v, e});
            }
            std::sort(pts.begin(), pts.end(), [](const KeyPoint &a, const KeyPoint &b) { return a.t < b.t; });
            if (!pts.isEmpty()) s.keys.insert(it.key(), pts);
        }
    }

    // "effects": ["cinematic", {"name": "glow", "amount": 0.6}, {"name": "lut", "path": "/x.cube"}]
    QList<Effect> effects(const QJsonValue &value, const QString &where, bool moments)
    {
        QList<Effect> out;
        const QStringList known = moments ? effectNames() + momentNames() : effectNames();
        for (const QJsonValue &v : value.toArray()) {
            Effect e;
            if (v.isString()) {
                e.name = v.toString().trimmed().toLower();
            } else {
                const QJsonObject o = v.toObject();
                e.name = o.value("name").toString(o.value("effect").toString()).trimmed().toLower();
                e.amount = std::clamp(o.contains("amount") ? seconds(o.value("amount")) : 1.0, -1.0, 2.0);
                if (std::isnan(e.amount)) e.amount = 1.0;
                e.path = o.value("path").toString();
            }
            if (!known.contains(e.name)) {
                report->warnings << QStringLiteral("%1: unknown effect \"%2\"; use one of %3.").arg(where, e.name, known.join(", "));
                continue;
            }
            if (e.name == QLatin1String("lut") && !QFileInfo::exists(e.path)) {
                report->warnings << QStringLiteral("%1: LUT file \"%2\" not found; left out.").arg(where, e.path);
                continue;
            }
            out << e;
        }
        return out;
    }

    // Effect moments sit on their own tracks above everything else.
    void placeMoment(const Segment &seg)
    {
        for (Track &t : out.tracks) {
            if (t.role != TrackRole::Overlay || t.segments.isEmpty()) continue;
            const bool onlyMoments = std::all_of(t.segments.cbegin(), t.segments.cend(),
                                                 [](const Segment &s) { return s.materialId == kEffectMaterialId; });
            if (onlyMoments && t.fits(seg.start, seg.end())) {
                t.segments.append(seg);
                return;
            }
        }
        Track fresh;
        fresh.id = Draft::newId();
        fresh.role = TrackRole::Overlay;
        fresh.segments.append(seg);
        out.tracks.append(fresh);
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

    if (plan.contains("style")) {
        const QString style = plan.value("style").toString().trimmed().toLower();
        if (adStyleNames().contains(style)) out.style = style;
        else if (style.isEmpty() || style == QLatin1String("none")) out.style.clear();
        else report->warnings << QStringLiteral("style \"%1\" is not one of %2; kept the current one.")
                                     .arg(style, adStyleNames().join(", "));
    }

    if (plan.contains("beat_sync")) {
        const QString mode = plan.value("beat_sync").toString().trimmed().toLower();
        if (beatSyncNames().contains(mode)) out.beatSync = mode;
        else if (mode == QLatin1String("beat")) out.beatSync = QStringLiteral("beats");
        else if (mode == QLatin1String("bar")) out.beatSync = QStringLiteral("bars");
        else out.beatSync.clear();  // "off", "none", ""
    }
    if (plan.contains("captions")) {
        const QString style = plan.value("captions").toString().trimmed().toLower();
        if (textPresetNames().contains(style)) out.captions = style;
        else if (style == QLatin1String("auto") || style == QLatin1String("on") || plan.value("captions").toBool())
            out.captions = QStringLiteral("caption");
        else out.captions.clear();
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
        Segment s;
        if (!b.timing(item, *m, where, 0, s)) continue;
        s.id = Draft::newId();
        s.materialId = m->id;
        s.start = cursor;
        s.volume = b.optNumber(item, "volume", 1, 0, 2);
        s.opacity = b.optNumber(item, "opacity", 1, 0, 1);
        s.transform.scale = b.optNumber(item, "scale", 1, 0.05, 8);
        s.transform.x = b.optNumber(item, "x", 0, -2, 2);
        s.transform.y = b.optNumber(item, "y", 0, -2, 2);
        b.animation(item, s, where);
        s.effects = b.effects(item.value("effects"), where, false);
        const QString tr = item.value("transition").toString().trimmed().toLower();
        if (!tr.isEmpty() && tr != QLatin1String("none") && tr != QLatin1String("cut")) {
            if (i == 0) {
                report->warnings << QStringLiteral("%1: the first clip has nothing to transition from; use \"in\" for an entrance.").arg(where);
            } else if (transitionNames().contains(tr)) {
                s.transition.name = tr;
                if (item.contains("transition_duration"))
                    s.transition.us = toUs(b.optNumber(item, "transition_duration", 0.5, 0.1, 3));
            } else {
                report->warnings << QStringLiteral("%1: unknown transition \"%2\"; use one of %3.").arg(where, tr, transitionNames().join(", "));
            }
        }
        s.speech = item.value("speech").toBool();
        if (item.contains("framing")) {
            const QString f = item.value("framing").toString().trimmed().toLower();
            if (framingNames().contains(f)) s.framing = f;
            else report->warnings << QStringLiteral("%1: unknown framing \"%2\"; use one of %3.").arg(where, f, framingNames().join(", "));
        }
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
        Segment s;
        if (!b.timing(item, *m, where, 3, s)) continue;
        double at = std::max(0.0, b.optNumber(item, "at", 0, 0, 1e6));
        double len = s.duration / 1e6;
        if (!fitToVideo(where, at, &len)) continue;
        s.id = Draft::newId();
        s.materialId = m->id;
        s.start = toUs(at);
        s.duration = toUs(len);
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
        b.animation(item, s, where);
        s.effects = b.effects(item.value("effects"), where, false);
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
        s.speech = item.value("speech").toBool();
        if (item.contains("duck")) s.duck = b.optNumber(item, "duck", 0.3, 0, 1);
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
        if (!applyTextPreset(style, &s.text, &s.transform, &s.motion)) {
            applyTextPreset(QStringLiteral("title"), &s.text, &s.transform, &s.motion);
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
        if (item.contains("shadow")) s.text.shadow = colorName(item.value("shadow"));
        if (item.contains("gradient")) s.text.gradient = colorName(item.value("gradient"));
        if (item.contains("shape")) {
            const QString shape = item.value("shape").toString().trimmed().toLower();
            if (textShapeNames().contains(shape)) s.text.shape = shape == QLatin1String("rounded") ? QString() : shape;
            else report->warnings << QStringLiteral("%1: unknown shape \"%2\"; use one of %3.").arg(where, shape, textShapeNames().join(", "));
        }
        b.animation(item, s, where);
        s.effects = b.effects(item.value("effects"), where, false);
        out.ensureTextMaterial();
        b.placeOn(TrackRole::Overlay, s);
    }


    // Effect moments (adjustment layers), above everything.
    const QJsonArray momentItems = plan.value("moments").toArray();
    for (int i = 0; i < momentItems.size(); ++i) {
        const QJsonObject item = momentItems[i].toObject();
        const QString where = itemLabel("moments", i);
        QJsonArray list{QJsonObject{{"name", item.value("effect").toString(item.value("name").toString())},
                                    {"amount", item.contains("amount") ? item.value("amount") : QJsonValue(1.0)}}};
        for (const QJsonValue &extra : item.value("effects").toArray()) list.append(extra);
        Segment s;
        s.id = Draft::newId();
        s.materialId = kEffectMaterialId;
        s.effects = b.effects(list, where, true);
        if (s.effects.isEmpty()) continue;
        double at = std::max(0.0, b.optNumber(item, "at", 0, 0, 1e6));
        const bool shortHit = momentNames().contains(s.effects.first().name);
        double len = b.optNumber(item, "duration", shortHit ? 0.5 : 3, 0.05, 1e6);
        if (!fitToVideo(where, at, &len)) continue;
        s.start = toUs(at);
        s.duration = toUs(len);
        out.ensureEffectMaterial();
        b.placeMoment(s);
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

    // Shared animation fields for clips, overlays and texts.
    const QJsonObject keyPoint = obj({{"t", number()}, {"v", number()}, {"ease", choice(easeNames())}}, {"t", "v"});
    QJsonObject keyProps;
    for (const QString &p : kKeyProps) keyProps.insert(p, array(keyPoint));
    const QJsonObject keys = obj(keyProps, {});
    const QJsonObject effect = obj({{"name", choice(effectNames())}, {"amount", number()}, {"path", string()}}, {"name"});
    auto animated = [&](QJsonObject props) {
        props.insert("effects", array(effect));
        props.insert("in", choice(motionNames("in") << "none"));
        props.insert("out", choice(motionNames("out") << "none"));
        props.insert("loop", choice(motionNames("loop") << "none"));
        props.insert("in_duration", number());
        props.insert("out_duration", number());
        props.insert("loop_speed", number());
        props.insert("rotation", number());
        props.insert("keys", keys);
        return props;
    };

    const QJsonObject clip = obj(animated({{"media", string()}, {"from", number()}, {"to", number()}, {"volume", number()},
                                           {"opacity", number()}, {"scale", number()}, {"x", number()}, {"y", number()},
                                           {"transition", choice(transitionNames() << "none")}, {"transition_duration", number()},
                                           {"framing", choice(framingNames())}, {"speech", QJsonObject{{"type", "boolean"}}},
                                           {"speed", number()}, {"speed_curve", choice(speedCurveNames())}, {"duration", number()}}),
                                 {"media", "from", "to"});
    const QJsonObject overlay = obj(animated({{"media", string()}, {"at", number()}, {"from", number()}, {"to", number()},
                                              {"speed", number()}, {"speed_curve", choice(speedCurveNames())}, {"duration", number()},
                                              {"position", choice(kOverlayPositions)}, {"scale", number()}, {"x", number()},
                                              {"y", number()}, {"volume", number()}, {"opacity", number()}}),
                                    {"media", "at", "from", "to", "position"});
    const QJsonObject audio = obj({{"media", string()}, {"at", number()}, {"from", number()}, {"to", number()},
                                   {"volume", number()}, {"speech", QJsonObject{{"type", "boolean"}}}, {"duck", number()}},
                                  {"media", "at", "from", "to", "volume"});
    const QJsonObject text = obj(animated({{"text", string()}, {"at", number()}, {"duration", number()},
                                  {"style", choice(textPresetNames())}, {"position", choice(kTextPositions)},
                                  {"color", string()}, {"size", number()}, {"bold", QJsonObject{{"type", "boolean"}}},
                                  {"outline", number()}, {"box", string()}, {"align", choice({"left", "center", "right"})},
                                  {"font", string()}, {"x", number()}, {"y", number()}, {"opacity", number()},
                                  {"scale", number()}, {"shadow", string()}, {"gradient", string()},
                                  {"shape", choice(textShapeNames())}}),
                                 {"text", "at", "duration", "style"});
    // Media made by local models; later items use it as "media": "<as>".
    const QJsonObject generate = obj({{"kind", choice(generateKinds())}, {"prompt", string()}, {"text", string()},
                                      {"voice", choice(voiceNames())}, {"seconds", number()}, {"as", string()}},
                                     {"kind", "as"});

    return obj({{"reply", string()},
                {"generate", array(generate)},
                {"aspect", choice({"keep", "original", "16:9", "9:16", "1:1", "4:5", "4:3", "3:4", "21:9"})},
                {"main", array(clip)},
                {"overlays", array(overlay)},
                {"audio", array(audio)},
                {"texts", array(text)},
                {"background", string()},
                {"style", choice(adStyleNames() << "none")},
                {"beat_sync", choice(beatSyncNames() << "off")},
                {"captions", choice(textPresetNames() << "none")},
                {"moments", array(obj({{"effect", choice(momentNames() + effectNames())}, {"at", number()},
                                       {"duration", number()}, {"amount", number()}},
                                      {"effect", "at", "duration"}))},
                {"export", QJsonObject{{"type", "boolean"}}},
                {"export_formats", array(choice(exportFormatNames()))}},
               {"reply", "aspect", "main", "overlays", "audio", "texts", "export"});
}

Draft resizedDraft(const Draft &d, const QString &aspect)
{
    const MediaIndex index(d);
    QJsonObject plan = planFromDraft(d, index);
    plan.insert("aspect", aspect);
    PlanReport report;
    Draft out = draftFromPlan(d, plan, index, &report);
    out.name = d.name;
    return out;
}

QStringList exportFormatNames()
{
    return {"9:16", "4:5", "1:1", "16:9"};
}

}  // namespace mtg::ai
