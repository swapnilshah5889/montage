#include "PlanGuards.h"

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>
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

// A request about how clips look, move or play (not which clips): keep the
// current clips and take only the model's styling of them.
void keepClipsRestyle(const QJsonArray &currentMain, QJsonObject *plan, QStringList *kept)
{
    const QJsonArray model = plan->value("main").toArray();
    if (currentMain.isEmpty()) return;
    static const char *const styling[] = {"effects", "in", "out", "loop", "in_duration", "out_duration", "loop_speed", "keys",
                                          "rotation", "transition", "transition_duration", "speed", "speed_curve", "framing",
                                          "scale", "x", "y", "opacity"};
    QList<bool> taken(model.size(), false);
    QJsonArray merged;
    bool changed = model.size() != currentMain.size();
    for (qsizetype i = 0; i < currentMain.size(); ++i) {
        QJsonObject c = currentMain[i].toObject();
        const QString media = c.value("media").toString();
        qsizetype j = -1;
        if (i < model.size() && !taken[i] && model[i].toObject().value("media").toString() == media) j = i;
        for (qsizetype k = 0; j < 0 && k < model.size(); ++k)
            if (!taken[k] && model[k].toObject().value("media").toString() == media) j = k;
        if (j >= 0) {
            taken[j] = true;
            const QJsonObject m = model[j].toObject();
            for (const char *key : styling)
                if (m.contains(QLatin1String(key))) c.insert(QLatin1String(key), m.value(QLatin1String(key)));
            // A speed change may come with a matching new range ("to" halved for slow motion).
            if (m.contains("speed") && m.value("speed") != currentMain[i].toObject().value("speed")
                && m.value("media") == c.value("media")) {
                c.insert("from", m.value("from"));
                c.insert("to", m.value("to"));
            }
            changed |= j != i || (m.value("from") != c.value("from")) || (m.value("to") != c.value("to"));
        } else {
            changed = true;
        }
        merged.append(c);
    }
    if (!changed) return;
    plan->insert("main", merged);
    *kept << QStringLiteral("the clips (styled them only)");
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

    // Visual-style words: looks, filters and effects live on clips.
    const bool styled = matches(req, R"(\b(energetic|energy|hype|bold|punchy|playful|fun|elegant|classy|luxury|luxurious|premium|minimal|minimalist|clean|vibe|mood|style|restyle|ad|advert|advertisement|commercial|promo|promotional)\b)");
    const bool looks = styled || matches(req, R"(\b(looks?|filters?|grade|grading|colou?r grade|cinematic|vintage|retro|noir|black and white|b&w|golden|warm|cool|vivid|dramatic|neon|vhs|film|grain|glow|blur|vignette|lut|style|stylish|effects?|animate|animation|animated|zoom|ken burns|rotate|spin|pop|slide|bounce|motion|keyframes?)\b)");
    const bool main = namesVisual || requestedLength(req) > 0
                      || matches(req, R"(\b(clips?|shots?|scenes?|footage|order|swap|reorder|shorter|longer|shorten|lengthen|trim|cut|remove|delete|drop|replace|speed|faster|slower|slow[ -]?mo(tion)?|length|duration|long|start(s)? with|end(s)? with|then|all|both|each|every|only|best|highlights?|photos?|images?|pictures?|montage|reel|tiktok|shorts?|story|slideshow|use)\b)")
                      || looks;
    // Changes to which clips play, their order or their length - as opposed to
    // how they look, move or play back.
    const bool structural = requestedLength(req) > 0
                            || matches(req, R"(\b(order|swap|reorder|shorter|longer|shorten|lengthen|trim|cut (out|it|them|the|that)|remove|delete|drop|replace|length|duration|start(s)? with|end(s)? with|then|all (the |my |of )?(clips|videos|shots|photos|pictures|images)|both|only|just|best|highlights?|montage|reel|tiktok|shorts|story|slideshow|use|make (a|an|me)|create|new|start over|add (the|my|a|an|another|more) (clip|video|shot|photo|picture|image)s?|more (clips|shots)|fewer|instead|move|seconds?|secs?)\b)");
    const bool aspect = matches(req, R"(\b(vertical|horizontal|portrait|landscape|square|tiktok|reels?|shorts|instagram|youtube|story|stories|aspect|ratio|format|widescreen|feed)\b|9:16|16:9|1:1|4:5|4:3|3:4|21:9)");
    const bool audio = namesAudio || (styled && current.value("audio").toArray().isEmpty())
                       || matches(req, R"(\b(music|songs?|audio|sounds?|track|volume|mute|unmute|quiet|quieter|louder|soundtrack|beats?|voice|silent|silence)\b)");
    const bool texts = request.contains(QLatin1Char('"')) || request.contains(QChar(0x201C)) || looks
                       || request.contains(QLatin1Char('$')) || request.contains(QLatin1Char('%'))
                       || matches(req, R"(\b(titles?|texts?|captions?|subtitles?|labels?|lower[ -]third|end[ -]card|says|say|saying|write|written|words?|fonts?|colou?rs?|red|blue|green|yellow|white|black|heading|name|intro|outro|card|quote|bigger|smaller|buttons?|cta|call to action|prices?|badges?|sale|off|discount|offer|deal|tag|headline|hook|shop|buy|order now|sign up|subscribe|code|brand)\b)");
    const bool overlays = (namesVisual && structural) || (looks && !current.value("overlays").toArray().isEmpty())
                          || matches(req, R"(\b(overlays?|logo|watermark|corner|picture in picture|pip|sticker|on top)\b)");
    const bool moments = looks || matches(req, R"(\b(flash|shake|glitch|punch|moments?|transitions?|white-?out|fade|beat|beats|hit|impact|rgb|split)\b)");
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
    if (main && !structural) keepClipsRestyle(current.value("main").toArray(), plan, &kept);
    restore("audio", audio, "the music");
    restore("texts", texts, "the text");
    restore("overlays", overlays, "the overlays");
    restore("background", background, "the background");
    restore("moments", moments, "the effects");
    restore("style", styled, "the style");
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
        double from, dur, cap;  // timeline seconds (cap = longest the file allows at this speed)
        bool still;             // photos and freezes keep their length
        double speed;           // 0 for freezes
    };
    QList<Clip> clips;
    double total = 0;
    bool repaired = false;  // some range had to be fixed: rewrite even if the total looks right
    for (const QJsonValue &v : main) {
        const QJsonObject o = v.toObject();
        const Material *m = lookup(o.value("media").toString(), d, index);
        if (!m) return false;  // unknown media: leave it to validation
        double speed = m->kind == MaterialKind::Video && o.contains("speed") ? seconds(o.value("speed")) : 1;
        if (std::isnan(speed) || speed < 0) speed = 1;
        if (speed == 0) {  // freeze frame: fixed length, like a photo
            const double hold = o.contains("duration") ? seconds(o.value("duration")) : 2;
            clips.append({0, std::isnan(hold) ? 2 : hold, 1e9, true, 0});
            total += clips.last().dur;
            continue;
        }
        speed = std::max(0.1, speed);
        const double cap = m->isTimed() ? m->durationUs / 1e6 : 1e9;
        double f = o.contains("from") ? seconds(o.value("from")) : 0;
        double t = o.contains("to") ? seconds(o.value("to")) : f + seconds(o.value("duration"));
        // Broken or out-of-file ranges: start at the file's beginning with a
        // provisional length; the scaling below sets the real one.
        const bool badStart = std::isnan(f) || f < 0 || f >= cap;
        const bool pastEnd = !badStart && !std::isnan(t) && t > cap + 0.01;
        if (badStart) f = 0;
        else if (pastEnd) t = cap;  // the builder would cut it there; count what will really play
        const bool provisional = badStart || std::isnan(t) || t - f < 0.3;
        repaired |= provisional || pastEnd;
        const double source = provisional ? std::min(cap, 3.0) : t - f;
        // Lengths below are timeline seconds: source / speed.
        clips.append({f, source / speed, m->isTimed() ? cap / speed : 1e9, !m->isTimed(), speed});
        total += source / speed;
    }
    if (!repaired && std::abs(total - want) <= std::max(0.15, want * 0.02)) return false;

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
        QJsonObject o = main[i].toObject();
        if (clips[i].speed == 0) {  // freeze: only its hold length changes
            o.insert("duration", round2(target[i]));
        } else {
            // Back to source seconds: timeline length x speed.
            const double sp = clips[i].speed, source = target[i] * sp, fileCap = clips[i].cap * sp;
            double from = clips[i].still ? 0 : clips[i].from;
            if (from + source > fileCap) from = std::max(0.0, fileCap - source);
            o.insert("from", round2(from));
            o.insert("to", round2(from + source));
            o.remove("duration");
        }
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


namespace {

// Seconds each main item lasts on the timeline.
QList<double> itemLengths(const QJsonArray &main)
{
    QList<double> out;
    for (const QJsonValue &v : main) {
        const QJsonObject o = v.toObject();
        const double speed = o.contains("speed") ? seconds(o.value("speed")) : 1;
        if (speed == 0) {
            const double d = seconds(o.value("duration"));
            out << (std::isnan(d) ? 2 : d);
            continue;
        }
        const double f = seconds(o.value("from")), t = seconds(o.value("to"));
        out << ((std::isnan(f) || std::isnan(t)) ? 0 : std::max(0.0, (t - f) / (std::isnan(speed) || speed <= 0 ? 1 : speed)));
    }
    return out;
}

}  // namespace

bool addAdMusic(const QString &request, const QJsonObject &current, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QString req = request.toLower();
    if (plan->value("style").toString().isEmpty() || !current.value("style").toString().isEmpty()) return false;
    if (!plan->value("audio").toArray().isEmpty() || !current.value("audio").toArray().isEmpty()) return false;
    if (matches(req, R"(\b(no|without|silent|mute|muted|quiet|voice ?over|voiceover|narration)\b)")) return false;
    const Material *song = nullptr;
    for (const QString &id : index.materialIds())
        if (const Material *m = d.material(id); m->kind == MaterialKind::Audio && (!song || m->durationUs > song->durationUs)) song = m;
    if (!song) return false;
    double length = 0;
    for (double l : itemLengths(plan->value("main").toArray())) length += l;
    for (const QJsonValue &v : plan->value("texts").toArray()) {
        const QJsonObject t = v.toObject();
        length = std::max(length, seconds(t.value("at")) + seconds(t.value("duration")));
    }
    if (!(length > 0)) return false;
    const double to = std::min(length, song->durationUs / 1e6);
    plan->insert("audio", QJsonArray{QJsonObject{{"media", index.alias(song->id)}, {"at", 0}, {"from", 0},
                                                 {"to", round2(to)}, {"volume", 0.8}}});
    return true;
}

QStringList missingFeatures(const QString &request, const QJsonObject &plan)
{
    const QString req = request.toLower();
    QStringList out;
    if (matches(req, R"(\b(remove|delete|without|undo|get rid|take (it |them )?(off|out)|no more|stop)\b)")) return out;

    const QJsonArray main = plan.value("main").toArray();
    const QList<double> lengths = itemLengths(main);
    QStringList cuts;
    double cursor = 0;
    for (qsizetype i = 0; i + 1 < lengths.size(); ++i) cuts << QString::number(round2(cursor += lengths[i]));
    const QString cutNote = cuts.isEmpty() ? QString() : QStringLiteral(" The cuts between clips are at %1 seconds.").arg(cuts.join(", "));

    // Every name the plan uses for an effect, motion or transition.
    QSet<QString> names;
    double minSpeed = 1, maxSpeed = 1;
    bool curve = false;
    auto collect = [&](const QJsonObject &o) {
        for (const QJsonValue &e : o.value("effects").toArray()) names << e.toObject().value("name").toString();
        for (const char *k : {"in", "out", "loop", "transition", "effect"}) names << o.value(QLatin1String(k)).toString();
        if (o.contains("keys")) names << QStringLiteral("keys");
    };
    for (const char *key : {"main", "overlays", "texts", "moments"})
        for (const QJsonValue &v : plan.value(QLatin1String(key)).toArray()) collect(v.toObject());
    for (const QJsonValue &v : main) {
        const QJsonObject o = v.toObject();
        if (o.contains("speed")) {
            const double s = seconds(o.value("speed"));
            if (!std::isnan(s)) minSpeed = std::min(minSpeed, s), maxSpeed = std::max(maxSpeed, s);
        }
        curve |= !o.value("speed_curve").toString().isEmpty() && o.value("speed_curve").toString() != QLatin1String("none");
    }

    if (matches(req, R"(\b(slow[ -]?mo|slow[ -]?motion|slow (it |this |that )?down|slowed)\b)") && !(minSpeed > 0 && minSpeed < 1) && !curve)
        out << QStringLiteral("The user wants slow motion: add \"speed\": 0.5 to that clip in \"main\". A clip with speed 0.5 lasts "
                              "(to - from) / 0.5 seconds, so to keep the video length make (to - from) half as long.");
    if (matches(req, R"(\b(speed (it |this |that |the [a-z]+ (clip |shot )?)?up|faster|fast[ -]forward|time-?lapse|double speed|[2-4] ?x)\b)")
        && !(maxSpeed > 1) && !curve)
        out << QStringLiteral("The user wants that clip faster: add \"speed\": 2 to it in \"main\" (it then lasts (to - from) / 2 seconds).");

    struct Want {
        const char *pattern;
        const char *name;
        bool moment;  // a hit on the whole picture at one time
    };
    static const Want wants[] = {
        {R"(\bflash(es)?\b)", "flash", true},
        {R"(\b(shake|shaking|camera shake)\b)", "shake", true},
        {R"(\b(zoom[ -]?punch|punch[ -]?in|punch[ -]?zoom|zoom punches)\b)", "zoom-punch", true},
        {R"(\b(white[ -]?out)\b)", "white-out", true},
        {R"(\bglitch(y|es)?\b)", "glitch", false},
        {R"(\b(rgb[ -]?split|chromatic)\b)", "rgb-split", false},
        {R"(\b(black and white|black-and-white|b&w|b/w|monochrome|grayscale|greyscale)\b)", "black-white", false},
        {R"(\bvintage\b)", "vintage", false},
        {R"(\bcinematic\b)", "cinematic", false},
        {R"(\bvhs\b)", "vhs", false},
        {R"(\b(film )?grain\b)", "grain", false},
        {R"(\b(glow|glowing)\b)", "glow", false},
        {R"(\bvignette\b)", "vignette", false},
        {R"(\bneon\b)", "neon", false},
        {R"(\blight[ -]?leaks?\b)", "light-leak", false},
        {R"(\bgolden[ -]hour\b)", "golden-hour", false},
    };
    for (const Want &w : wants) {
        if (!matches(req, w.pattern) || names.contains(QLatin1String(w.name))) continue;
        if (QLatin1String(w.name) == QLatin1String("black-white") && names.contains(QStringLiteral("noir"))) continue;
        if (QLatin1String(w.name) == QLatin1String("vintage") && (names.contains(QStringLiteral("vhs")) || names.contains(QStringLiteral("grain"))))
            continue;
        if (w.moment)
            out << QStringLiteral("The user asked for %1 but the plan has none. Add it to \"moments\": "
                                  "{\"effect\": \"%1\", \"at\": <seconds into the video>, \"duration\": 0.3}.%2")
                       .arg(QLatin1String(w.name), cutNote);
        else
            out << QStringLiteral("The user asked for %1 but the plan has none. Add \"effects\": [{\"name\": \"%1\"}] to the clips "
                                  "or texts it is for (every clip in \"main\" if it is for the whole video).")
                       .arg(QLatin1String(w.name));
    }

    bool hasTransition = false;
    for (qsizetype i = 1; i < main.size(); ++i) {
        const QString t = main[i].toObject().value("transition").toString();
        hasTransition |= !t.isEmpty() && t != QLatin1String("none") && t != QLatin1String("cut");
    }
    if (matches(req, R"(\btransitions?\b)") && !matches(req, R"(\b(no|hard)\b)") && !hasTransition && main.size() > 1)
        out << QStringLiteral("The user asked for transitions: add \"transition\": \"crossfade\" (or whip, zoom, fade-black, glitch, push-left) "
                              "to every clip in \"main\" after the first.");
    if (matches(req, R"(\b(hard cuts?|no transitions?|without transitions?|straight cuts?)\b)") && hasTransition)
        out << QStringLiteral("The user wants hard cuts: set \"transition\": \"none\" on every clip in \"main\".");

    const auto entrance = QRegularExpression(QStringLiteral(R"(\b(pop|slide|fade|bounce|drop|spin|zoom|blur|type)s? ?(in|up)\b)")).match(req);
    if (entrance.hasMatch() && matches(req, R"(\b(title|text|caption|headline|words?|logo|name|badge|button|price)\b)")) {
        bool any = false;
        for (const QJsonValue &v : plan.value("texts").toArray()) any |= !v.toObject().value("in").toString().isEmpty() && v.toObject().value("in") != QLatin1String("none");
        for (const QJsonValue &v : plan.value("overlays").toArray()) any |= !v.toObject().value("in").toString().isEmpty() && v.toObject().value("in") != QLatin1String("none");
        static const QHash<QString, QString> motion{{"pop", "pop"}, {"slide", "slide-up"}, {"fade", "fade"}, {"bounce", "drop"},
                                                     {"drop", "drop"}, {"spin", "spin"}, {"zoom", "zoom-in"}, {"blur", "blur-in"}, {"type", "typewriter"}};
        if (!any)
            out << QStringLiteral("The user wants it to %1 in: add \"in\": \"%2\" to that item in \"texts\" (or \"overlays\").")
                       .arg(entrance.captured(1), motion.value(entrance.captured(1)));
    }
    if (matches(req, R"(\b(ken burns|slowly zoom|slow zoom|zoom (slowly )?(in)?to|zoom in on|push in)\b)") && !names.contains(QStringLiteral("ken-burns"))
        && !names.contains(QStringLiteral("keys")) && !names.contains(QStringLiteral("zoom-in")))
        out << QStringLiteral("The user wants a slow zoom: add \"loop\": \"ken-burns\" to that clip.");
    return out;
}


bool snapEndTexts(const QString &request, const QJsonObject &current, QJsonObject *plan)
{
    const QString req = request.toLower();
    if (!matches(req, R"(\b(at the (very )?end|end (with|on)|ending|finish (with|on)|final (seconds?|shot|frame)|last (seconds?|shot|frame)|closing)\b)"))
        return false;
    double length = 0;
    for (double l : itemLengths(plan->value("main").toArray())) length += l;
    if (!(length > 0)) return false;
    QSet<QString> before;
    for (const QJsonValue &v : current.value("texts").toArray()) before << v.toObject().value("text").toString();
    QJsonArray texts = plan->value("texts").toArray();
    bool changed = false;
    for (qsizetype i = 0; i < texts.size(); ++i) {
        QJsonObject o = texts[i].toObject();
        if (before.contains(o.value("text").toString())) continue;
        const double at = seconds(o.value("at")), dur = seconds(o.value("duration"));
        if (std::isnan(at) || std::isnan(dur) || dur <= 0) continue;
        if (std::abs(at + dur - length) < 0.1 || at + dur > length + 0.1) continue;  // already ends with it (or past: fitted later)
        if (at < length / 2) continue;  // not meant for the end
        const double d = std::min(dur, length);
        o.insert("at", round2(length - d));
        o.insert("duration", round2(d));
        texts[i] = o;
        changed = true;
    }
    if (changed) plan->insert("texts", texts);
    return changed;
}

QStringList requestedFormats(const QString &request)
{
    const QString req = request.toLower();
    if (!matches(req, R"(\b(export|render|save|download|deliver|make (versions|copies|cuts)|versions?|formats?|sizes?|cutdowns?)\b)"))
        return {};
    if (matches(req, R"(\b(all|every|each)\s+(the\s+)?(platforms?|formats?|sizes?|shapes?|ratios?|socials?|channels?)\b|\b(multi[- ]?format|every social)\b)"))
        return exportFormatNames();
    QStringList out;
    auto want = [&](const char *pattern, const char *shape) {
        if (matches(req, pattern) && !out.contains(QLatin1String(shape))) out << QString::fromLatin1(shape);
    };
    want(R"(\b(tiktok|reels?|shorts|stories|story|vertical|portrait|snapchat)\b|9:16)", "9:16");
    want(R"(\b(feed|4:5)\b|4:5)", "4:5");
    want(R"(\b(square|1:1)\b|1:1)", "1:1");
    want(R"(\b(youtube|landscape|horizontal|widescreen|website|tv)\b|16:9)", "16:9");
    if (out.size() < 2) return {};
    // Same order every time.
    QStringList ordered;
    for (const QString &f : exportFormatNames())
        if (out.contains(f)) ordered << f;
    return ordered;
}

QStringList applyObviousFeatures(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QString req = request.toLower();
    QStringList notes;
    if (matches(req, R"(\b(remove|delete|without|undo|get rid|no more|stop)\b)")) return notes;
    QJsonArray main = plan->value("main").toArray();
    bool changed = false;

    // "Slowly zoom into the photo(s)".
    if (matches(req, R"(\b(ken burns|slowly zoom|slow zoom|zoom (slowly )?(in)?to|zoom in on|push in|pan across|movement|move)\b)")
        && matches(req, R"(\b(photos?|pictures?|images?|stills?)\b)")) {
        int added = 0;
        for (qsizetype i = 0; i < main.size(); ++i) {
            QJsonObject o = main[i].toObject();
            const Material *m = lookup(o.value("media").toString(), d, index);
            if (!m || m->kind != MaterialKind::Image || o.contains("keys")) continue;
            if (!o.value("loop").toString().isEmpty() && o.value("loop") != QLatin1String("none")) continue;
            o.insert("loop", "ken-burns");
            main[i] = o;
            ++added;
        }
        if (added) {
            changed = true;
            notes << QStringLiteral("added a slow zoom to %1 photo%2").arg(added).arg(added == 1 ? "" : "s");
        }
    }

    // A look for "the whole video" / "everything" / "all the clips".
    static const struct {
        const char *pattern, *name;
    } looks[] = {
        {R"(\b(black and white|black-and-white|b&w|monochrome|grayscale|greyscale)\b)", "black-white"},
        {R"(\bvintage\b)", "vintage"}, {R"(\bcinematic\b)", "cinematic"}, {R"(\bnoir\b)", "noir"},
        {R"(\bvhs\b)", "vhs"}, {R"(\bgolden[ -]hour\b)", "golden-hour"}, {R"(\bwarm(er)?\b)", "warm"},
        {R"(\b(cool|cold)(er)?\b)", "cool"}, {R"(\bvivid\b)", "vivid"}, {R"(\bdramatic\b)", "dramatic"}, {R"(\bfaded\b)", "faded"},
    };
    if (matches(req, R"(\b(whole|entire|full) (video|thing|edit|ad|reel)|\beverything\b|\ball (the |my |of the )?(clips|shots|footage|videos)\b|\b(the|this|my) (video|ad|edit|reel)\b|\bmake it\b|\bgive it\b)")) {
        for (const auto &look : looks) {
            if (!matches(req, look.pattern)) continue;
            int added = 0;
            for (qsizetype i = 0; i < main.size(); ++i) {
                QJsonObject o = main[i].toObject();
                QJsonArray fx = o.value("effects").toArray();
                bool has = false;
                for (const QJsonValue &e : fx) has |= e.toObject().value("name").toString() == QLatin1String(look.name);
                if (has) continue;
                fx.append(QJsonObject{{"name", QLatin1String(look.name)}});
                o.insert("effects", fx);
                main[i] = o;
                ++added;
            }
            if (added) {
                changed = true;
                notes << QStringLiteral("applied %1 to every clip").arg(QLatin1String(look.name));
            }
            break;  // one look per request
        }
    }
    if (changed) plan->insert("main", main);
    return notes;
}

}  // namespace mtg::ai
