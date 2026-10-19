#include "PlanGuards.h"

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <cmath>

namespace mtg::ai {

namespace {

bool matches(const QString &text, const char *pattern)
{
    return QRegularExpression(QLatin1String(pattern), QRegularExpression::CaseInsensitiveOption).match(text).hasMatch();
}

double round2(double v)
{
    return std::round(v * 100) / 100;
}

const Material *lookup(const QString &ref, const Draft &d, const MediaIndex &index)
{
    QString id = index.materialId(ref);
    if (id.isEmpty())
        for (const QString &mid : index.materialIds())
            if (d.material(mid)->name.compare(ref.trimmed(), Qt::CaseInsensitive) == 0) id = mid;
    return id.isEmpty() ? nullptr : d.material(id);
}

}  // namespace

QString parseColor(const QString &text)
{
    QString t = text.trimmed().toLower();
    if (t.isEmpty()) return {};
    t.replace(QLatin1String("grey"), QLatin1String("gray"));
    t.replace(QLatin1String("deep "), QLatin1String("dark "));
    t.replace(QLatin1String("bright "), QString());
    t.replace(QLatin1String("pale "), QLatin1String("light "));
    QString compact = t;
    compact.remove(QLatin1Char(' '));
    for (const QString &candidate : {t, compact}) {
        const QColor c(candidate);
        if (c.isValid()) return c.name().toUpper();
    }
    // "dark purple", "light green" without an SVG name: shade the base colour.
    const QStringList words = t.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() == 2) {
        const QColor base(words[1]);
        if (base.isValid() && words[0] == QLatin1String("dark")) return base.darker(170).name().toUpper();
        if (base.isValid() && words[0] == QLatin1String("light")) return base.lighter(150).name().toUpper();
    }
    return {};
}

QString requestedBackground(const QString &request)
{
    static const QRegularExpression re(
        QStringLiteral(R"(((?:dark|light|deep|bright|pale)\s+)?([a-z]+)\s+(?:background|backdrop|bg)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto m = re.match(request);
    if (!m.hasMatch()) return {};
    return parseColor(m.captured(1) + m.captured(2));
}

double requestedLength(const QString &request)
{
    const QString req = request.toLower();
    // "10 second video", "a 12s vertical tiktok", "6 second intro"; not "3 seconds of the video".
    static const QRegularExpression before(QStringLiteral(
        R"((\d+(?:\.\d+)?)\s*-?\s*(?:s|sec|secs|second|seconds)\s+(?:(?!(?:of|at|in|into|from|for|on|before|after|the|each|per)\b)[a-z]+\s+){0,2}(?:video|reel|clip|edit|short|tiktok|story|montage|intro|outro|ad|promo|trailer)\b)"));
    // "make the video 20 seconds", "video of 15 seconds", "whole thing 9 seconds"
    static const QRegularExpression after(QStringLiteral(
        R"((?:video|reel|edit|montage|whole thing|it)\s+(?:to\s+|of\s+|that is\s+|about\s+|be\s+|is\s+)?(\d+(?:\.\d+)?)\s*(?:s|sec|secs|second|seconds)\b)"));
    auto m = before.match(req);
    if (!m.hasMatch()) m = after.match(req);
    return m.hasMatch() ? m.captured(1).toDouble() : 0;
}

QStringList keepUnrequested(const QString &request, const QJsonObject &current, const Draft &d, const MediaIndex &index,
                            QJsonObject *plan)
{
    const QString req = request.toLower();
    if (matches(req, R"(\b(start over|from scratch|new video|reset|clear (it|everything|the timeline)|redo it|completely)\b)"))
        return {};

    // Mentioning a file by name ("the beach clip", "the song") counts as intent for its kind.
    bool namesVisual = false, namesAudio = false;
    for (const QString &id : index.materialIds()) {
        const Material *m = d.material(id);
        const QString stem = QFileInfo(m->name).completeBaseName().toLower();
        if (stem.size() < 3 || !QRegularExpression(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(stem))).match(req).hasMatch())
            continue;
        (m->kind == MaterialKind::Audio ? namesAudio : namesVisual) = true;
    }

    const bool main = namesVisual || requestedLength(req) > 0
                      || matches(req, R"(\b(clips?|shots?|scenes?|footage|order|swap|reorder|shorter|longer|shorten|lengthen|trim|cut|remove|delete|drop|replace|speed|faster|slower|length|duration|long|start(s)? with|end(s)? with|then|all|both|each|every|only|best|highlights?|photos?|images?|pictures?|montage|reel|tiktok|shorts?|story|slideshow|use)\b)");
    const bool aspect = matches(req, R"(\b(vertical|horizontal|portrait|landscape|square|tiktok|reels?|shorts|instagram|youtube|story|stories|aspect|ratio|format|widescreen)\b|9:16|16:9|1:1|4:3|3:4|21:9)");
    const bool audio = namesAudio
                       || matches(req, R"(\b(music|songs?|audio|sounds?|track|volume|mute|unmute|quiet|quieter|louder|soundtrack|beats?|voice|silent|silence)\b)");
    const bool texts = request.contains(QLatin1Char('"')) || request.contains(QChar(0x201C))
                       || matches(req, R"(\b(titles?|texts?|captions?|subtitles?|labels?|lower[ -]third|end[ -]card|says|say|saying|write|written|words?|fonts?|colou?rs?|red|blue|green|yellow|white|black|heading|name|intro|outro|card|quote|bigger|smaller)\b)");
    const bool overlays = namesVisual || matches(req, R"(\b(overlays?|logo|watermark|corner|picture in picture|pip|sticker|on top)\b)");
    const bool background = texts || matches(req, R"(\b(background|backdrop)\b)");

    QStringList kept;
    auto restore = [&](const char *key, bool wanted, const char *what) {
        if (wanted) return;
        const QJsonValue before = current.value(QLatin1String(key));
        if (plan->value(QLatin1String(key)) == before) return;
        if (before.isUndefined()) plan->remove(QLatin1String(key));
        else plan->insert(QLatin1String(key), before);
        kept << QString::fromLatin1(what);
    };
    restore("main", main, "the clips");
    restore("audio", audio, "the music");
    restore("texts", texts, "the text");
    restore("overlays", overlays, "the overlays");
    restore("background", background, "the background");
    if (!aspect && plan->value("aspect").toString() != QLatin1String("keep")
        && plan->value("aspect") != current.value("aspect")) {
        plan->insert("aspect", QStringLiteral("keep"));
        kept << QStringLiteral("the shape");
    }
    return kept;
}

bool fitToLength(double want, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    QJsonArray main = plan->value("main").toArray();
    if (main.isEmpty() || want <= 0) return false;

    struct Clip {
        double from, dur, cap;
        bool still;
    };
    QList<Clip> clips;
    double total = 0;
    for (const QJsonValue &v : main) {
        const QJsonObject o = v.toObject();
        const Material *m = lookup(o.value("media").toString(), d, index);
        if (!m) return false;  // unknown media: leave it to validation
        const double cap = m->isTimed() ? m->durationUs / 1e6 : 1e9;
        double f = o.contains("from") ? seconds(o.value("from")) : 0;
        double t = o.contains("to") ? seconds(o.value("to")) : f + seconds(o.value("duration"));
        // Broken or out-of-file ranges: start at the file's beginning with a
        // provisional length; the scaling below sets the real one.
        if (std::isnan(f) || f < 0 || f >= cap) f = 0;
        double dur = (std::isnan(t) || t - f < 0.3) ? std::min(cap, 3.0) : t - f;
        clips.append({f, dur, cap, !m->isTimed()});
        total += dur;
    }
    if (std::abs(total - want) <= std::max(0.15, want * 0.02)) return false;

    // Photos and logos keep the length the plan gave them ("logo for 2
    // seconds"); videos absorb the difference, unless only stills remain.
    double fixed = 0, variable = 0;
    for (const Clip &c : clips) (c.still ? fixed : variable) += c.dur;
    const bool scaleAll = variable <= 0 || want - fixed < 0.3 * clips.size();
    const double scale = scaleAll ? want / total : (want - fixed) / variable;
    QList<double> target;
    for (const Clip &c : clips) target << ((scaleAll || !c.still) ? c.dur * scale : c.dur);

    // Respect file lengths; hand any shortfall to clips that still have room.
    for (int pass = 0; pass < clips.size(); ++pass) {
        double shortfall = 0, room = 0;
        for (int i = 0; i < clips.size(); ++i) {
            if (target[i] > clips[i].cap) {
                shortfall += target[i] - clips[i].cap;
                target[i] = clips[i].cap;
            }
        }
        if (shortfall < 0.01) break;
        for (int i = 0; i < clips.size(); ++i)
            if (target[i] < clips[i].cap - 0.01 && (scaleAll || !clips[i].still)) room += target[i];
        if (room <= 0) break;
        for (int i = 0; i < clips.size(); ++i)
            if (target[i] < clips[i].cap - 0.01 && (scaleAll || !clips[i].still)) target[i] += shortfall * target[i] / room;
    }

    double newTotal = 0;
    for (int i = 0; i < clips.size(); ++i) {
        target[i] = std::max(0.3, round2(target[i]));
        double from = clips[i].still ? 0 : clips[i].from;
        if (from + target[i] > clips[i].cap) from = std::max(0.0, clips[i].cap - target[i]);
        QJsonObject o = main[i].toObject();
        o.insert("from", round2(from));
        o.insert("to", round2(from + target[i]));
        o.remove("duration");
        main[i] = o;
        newTotal += target[i];
    }
    plan->insert("main", main);

    // Music, overlays and texts that ended with the video keep ending with it.
    auto refit = [&](const char *key, bool isText) {
        QJsonArray items = plan->value(QLatin1String(key)).toArray();
        for (int i = 0; i < items.size(); ++i) {
            QJsonObject o = items[i].toObject();
            const double at = seconds(o.value("at"));
            if (std::isnan(at)) continue;
            if (isText) {
                const double dur = seconds(o.value("duration"));
                if (std::isnan(dur)) continue;
                if (std::abs(at + dur - total) < 0.3 || at + dur > newTotal) o.insert("at", round2(std::max(0.0, newTotal - dur)));
            } else {
                const double f = seconds(o.value("from")), t = seconds(o.value("to"));
                if (std::isnan(f) || std::isnan(t)) continue;
                if (std::abs(at + (t - f) - total) < 0.3 || at + (t - f) > newTotal) {
                    const Material *m = lookup(o.value("media").toString(), d, index);
                    const double cap = m && m->isTimed() ? m->durationUs / 1e6 : 1e9;
                    o.insert("to", round2(std::min(cap, f + std::max(0.1, newTotal - at))));
                }
            }
            items[i] = o;
        }
        plan->insert(QLatin1String(key), items);
    };
    refit("audio", false);
    refit("overlays", false);
    refit("texts", true);
    return true;
}

}  // namespace mtg::ai
