#include "PlanGuards.h"

#include "engine/Scenes.h"
#include "engine/Speech.h"

#include "Generators.h"

#include <QColor>
#include <QFileInfo>
#include <QHash>
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
    // "remove the background and put it on a white background": the first
    // "<word> background" that is a colour.
    for (auto it = re.globalMatch(request); it.hasNext();) {
        const auto m = it.next();
        if (const QString c = parseColor(m.captured(1) + m.captured(2)); !c.isEmpty()) return c;
    }
    return {};
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
    // Quoted words are text for the screen, not instructions ('a caption "Look at this"' isn't about looks).
    QString req = request.toLower();
    req.replace(QRegularExpression(QStringLiteral("[\"\u201C][^\"\u201D]*[\"\u201D]")), QStringLiteral("\"\""));
    // Removing a clip's background styles the clip; it doesn't remove clips.
    static const QRegularExpression bgRemoval(QStringLiteral(R"(\b(remove|erase|drop|get rid of|cut out|delete|lose) (the |its |their |his |her )?(back ?ground|backdrop)\b)"));
    req.replace(bgRemoval, QStringLiteral("cutout"));
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
                       || matches(req, R"(\b(music|songs?|audio|sounds?|track|volume|mute|unmute|quiet|quieter|louder|soundtrack|beats?|voice|voice-?overs?|narrat\w*|silent|silence|jingle)\b)");
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
    restore("captions", matches(req, R"(\b(captions?|subtitles?|transcri\w*)\b)"), "the captions");
    restore("clean_speech", matches(req, R"(\b(u+m+s?|u+h+s?|fillers?|pauses?|silences?|dead air|gaps|tighten|tighter|snappier|jump ?cuts?|clean)\b)"),
            "the speech cleanup");
    // A speed change nobody asked for (small models sometimes add "speed" to
    // trim): keep each clip's current speed.
    if (!matches(req, R"(\b(speed|faster|slower|fast|slow|slow[ -]?mo\w*|time-?lapse|ramp|freeze|hold|[0-9.]+ ?x)\b)")) {
        QJsonArray items = plan->value("main").toArray();
        QHash<QString, QJsonValue> currentSpeed;
        for (const QJsonValue &v : current.value("main").toArray())
            currentSpeed.insert(v.toObject().value("media").toString(), v.toObject().value("speed"));
        bool changed = false;
        for (qsizetype i = 0; i < items.size(); ++i) {
            QJsonObject o = items[i].toObject();
            if (!o.contains("speed") && !o.contains("speed_curve")) continue;
            const QJsonValue before = currentSpeed.value(o.value("media").toString());
            if (o.value("speed") == before && !o.contains("speed_curve")) continue;
            if (before.isUndefined()) o.remove("speed");
            else o.insert("speed", before);
            o.remove("speed_curve");
            items[i] = o;
            changed = true;
        }
        if (changed) {
            plan->insert("main", items);
            kept << QStringLiteral("the speed");
        }
    }
    restore("beat_sync", styled || matches(req, R"(\b(beats?|rhythm|sync|in time|bars?|downbeats?|tempo)\b)"), "the beat sync");
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
        bool locked = false;    // text-edited clips ("skips"): left exactly as they are
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
        if (!o.value("skips").toArray().isEmpty()) {
            const double len = itemSeconds(o);
            clips.append({0, std::isnan(len) ? 0 : len, 1e9, true, speed, true});
            total += clips.last().dur;
            continue;
        }
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
        if (clips[i].locked) {
            newTotal += clips[i].dur;
            continue;
        }
        target[i] = std::max(0.3, round2(target[i]));
        QJsonObject o = main[i].toObject();
        if (clips[i].speed == 0) {  // freeze: only its hold length changes
            o.insert("duration", round2(target[i]));
        } else {
            // Back to source seconds: timeline length x speed.
            const double sp = clips[i].speed, source = target[i] * sp, fileCap = clips[i].cap * sp;
            double from = clips[i].still ? 0 : clips[i].from;
            // A clip that sits inside one shot stays in it: if the shot is too short
            // for the new length, slow it down (to half speed at most) rather than
            // run into the next shot, the way an editor would.
            const Material *mat = lookup(o.value("media").toString(), d, index);
            const SceneIndex shots = mat && mat->kind == MaterialKind::Video && sp == 1 ? cachedSceneIndex(*mat) : SceneIndex{};
            const double origTo = seconds(o.value("to"));
            const Shot *home = nullptr;
            for (const Shot &sh : shots.shots)
                if (from >= sh.start / 1e6 - 0.05 && !std::isnan(origTo) && origTo <= sh.end / 1e6 + 0.05) home = &sh;
            if (home && shots.shots.size() > 1 && source > home->length() / 1e6 + 0.05 && home->length() / 1e6 >= 0.5 * source) {
                const double len = home->length() / 1e6;
                o.insert("from", round2(home->start / 1e6));
                o.insert("to", round2(home->end / 1e6));
                o.insert("speed", round2(len / target[i]));
                o.remove("duration");
                main[i] = o;
                newTotal += target[i];
                continue;
            }
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
        const double s = itemSeconds(v.toObject());
        out << (std::isnan(s) ? 0 : s);
    }
    return out;
}

}  // namespace

bool addAdMusic(const QString &request, const QJsonObject &current, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QString req = request.toLower();
    if (plan->value("style").toString().isEmpty() || !current.value("style").toString().isEmpty()) return false;
    // A voiceover isn't music: an ad with one still gets music under it.
    auto hasMusic = [&](const QJsonArray &audio) {
        for (const QJsonValue &v : audio)
            if (const Material *m = lookup(v.toObject().value("media").toString(), d, index); !m || !isVoiceover(*m)) return true;
        return false;
    };
    if (hasMusic(plan->value("audio").toArray()) || hasMusic(current.value("audio").toArray())) return false;
    if (matches(req, R"(\b(no|without|silent|mute|muted|quiet)\b)")) return false;
    const Material *song = nullptr;
    for (const QString &id : index.materialIds())
        if (const Material *m = d.material(id); m->kind == MaterialKind::Audio && !isVoiceover(*m) && (!song || m->durationUs > song->durationUs))
            song = m;
    if (!song) return false;
    double length = 0;
    for (double l : itemLengths(plan->value("main").toArray())) length += l;
    for (const QJsonValue &v : plan->value("texts").toArray()) {
        const QJsonObject t = v.toObject();
        length = std::max(length, seconds(t.value("at")) + seconds(t.value("duration")));
    }
    if (!(length > 0)) return false;
    const double to = std::min(length, song->durationUs / 1e6);
    QJsonArray audio = plan->value("audio").toArray();
    audio.prepend(QJsonObject{{"media", index.alias(song->id)}, {"at", 0}, {"from", 0}, {"to", round2(to)}, {"volume", 0.8}});
    plan->insert("audio", audio);
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
    // "end with a Shop now button": the words after "end with" name the text meant for the end.
    QStringList named;
    if (const auto m = QRegularExpression(QStringLiteral(R"(\b(?:end|finish)(?:s|ing)? (?:with|on) (?:an? |the )?([^.,;!?]+))")).match(req); m.hasMatch())
        for (const QString &w : m.captured(1).split(QLatin1Char(' '), Qt::SkipEmptyParts))
            if (w.size() >= 3 && !QStringList{"button", "text", "title", "card", "caption", "badge", "and", "the", "with"}.contains(w)) named << w;
    QJsonArray texts = plan->value("texts").toArray();
    bool changed = false;
    for (qsizetype i = 0; i < texts.size(); ++i) {
        QJsonObject o = texts[i].toObject();
        if (before.contains(o.value("text").toString())) continue;
        const double at = seconds(o.value("at")), dur = seconds(o.value("duration"));
        if (std::isnan(at) || std::isnan(dur) || dur <= 0) continue;
        if (std::abs(at + dur - length) < 0.1 || at + dur > length + 0.1) continue;  // already ends with it (or past: fitted later)
        const QString text = o.value("text").toString().toLower();
        const bool isNamed = !named.isEmpty() && std::all_of(named.cbegin(), named.cend(), [&](const QString &w) { return text.contains(w); });
        if (at < length / 2 && !isNamed) continue;  // not meant for the end
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

    // A hit on a named cut: "a flash when the second clip starts", "shake on every cut".
    static const struct {
        const char *pattern, *name;
        double seconds;
    } hits[] = {
        {R"(\bflash(es)?\b)", "flash", 0.3},
        {R"(\b(zoom[ -]?punch(es)?|punch[ -]?ins?)\b)", "zoom-punch", 0.35},
        {R"(\b(shake|shakes|shaking)\b)", "shake", 0.5},
        {R"(\bwhite[ -]?outs?\b)", "white-out", 0.4},
        {R"(\b(rgb[ -]?split|chromatic)\b)", "rgb-split", 0.35},
        {R"(\bglitch(es)?\b)", "glitch", 0.35},
    };
    QList<double> cuts;
    {
        double cursor = 0;
        const QList<double> lengths = itemLengths(plan->value("main").toArray());
        for (qsizetype i = 0; i + 1 < lengths.size(); ++i) cuts << round2(cursor += lengths[i]);
    }
    static const QRegularExpression nthClip(QStringLiteral(
        R"(\b(?:when|as|where) the (second|2nd|third|3rd|fourth|4th|fifth|5th|next|last|final) (?:clip|shot|scene|video|photo|picture)s? (?:starts?|begins?|comes? in|comes? on|appears?|kicks? in)\b)"));
    const bool everyCut = matches(req, R"(\b(?:at|on|between) (?:every|each|all(?: the)?) (?:cuts?|clips?|shots?|transitions?)\b|\bbetween (?:the |all )?(?:clips|shots|scenes)\b|\bon (?:the )?cuts\b)");
    QList<double> targets;
    if (everyCut) {
        targets = cuts;
    } else if (const auto m = nthClip.match(req); m.hasMatch() && !cuts.isEmpty()) {
        static const QHash<QString, int> ordinal{{"second", 0}, {"2nd", 0}, {"next", 0}, {"third", 1}, {"3rd", 1},
                                                 {"fourth", 2}, {"4th", 2}, {"fifth", 3}, {"5th", 3}};
        const QString which = m.captured(1);
        const int k = which == QLatin1String("last") || which == QLatin1String("final") ? int(cuts.size()) - 1 : ordinal.value(which, -1);
        if (k >= 0 && k < cuts.size()) targets << cuts[k];
    } else if (matches(req, R"(\b(?:at|on) the cut\b)") && cuts.size() == 1) {
        targets = cuts;
    }
    if (!targets.isEmpty()) {
        QJsonArray moments = plan->value("moments").toArray();
        int added = 0;
        for (const auto &h : hits) {
            if (!matches(req, h.pattern)) continue;
            for (double t : targets) {
                bool have = false;
                for (const QJsonValue &v : moments) {
                    const QJsonObject o = v.toObject();
                    have |= o.value("effect").toString() == QLatin1String(h.name) && std::abs(seconds(o.value("at")) - t) < 0.3;
                }
                if (have) continue;
                moments.append(QJsonObject{{"effect", QLatin1String(h.name)}, {"at", t}, {"duration", h.seconds}});
                ++added;
            }
        }
        if (added) {
            plan->insert("moments", moments);
            notes << QStringLiteral("added %1 on the cut%2").arg(added == 1 ? QStringLiteral("the effect") : QStringLiteral("%1 effects").arg(added)).arg(targets.size() == 1 ? "" : "s");
        }
    }
    return notes;
}

// ---------------------------------------------------------------- generated media

namespace {

const char *const kVoiceWords = R"(\b(voice[ -]?overs?|voice|voiced|narrat\w*|narrator|spoken|speak(s|ing)?|read (it )?(out|aloud)|announcer|vo)\b)";
const char *const kMusicWords = R"(\b(music|songs?|soundtrack|beats?|jingle|tune|melody|score|background (audio|track))\b)";
const char *const kImageWords = R"(\b(images?|pictures?|photos?|illustrations?|artwork|visuals?|scenery|b-?roll|stills?|backdrop|background)\b)";

QString quotedNear(const QString &request, const char *near)
{
    // Straight or curly quotes; an apostrophe inside ("don't") doesn't close one.
    static const QRegularExpression quoted(QStringLiteral("(?<!\\w)[\"“‘']([^\"”]{2,200}?)[\"”’'](?=[\\s,.!?;:]|$)"));
    QStringList all;
    QList<qsizetype> at;
    for (auto it = quoted.globalMatch(request); it.hasNext();) {
        const auto m = it.next();
        all << m.captured(1).trimmed();
        at << m.capturedStart();
    }
    if (all.isEmpty()) return {};
    // The first quote after the voice words ("a voiceover saying 'Light up your evenings'").
    const auto v = QRegularExpression(QLatin1String(near), QRegularExpression::CaseInsensitiveOption).match(request);
    if (v.hasMatch())
        for (qsizetype i = 0; i < all.size(); ++i)
            if (at[i] >= v.capturedStart() && at[i] - v.capturedEnd() < 40) return all[i];
    return all.size() == 1 ? all.first() : QString();
}

// Video length the plan will have: main clips, else the texts.
double planLength(const QJsonObject &plan)
{
    double length = 0;
    for (double l : itemLengths(plan.value("main").toArray())) length += l;
    if (length > 0) return length;
    for (const QJsonValue &v : plan.value("texts").toArray()) {
        const QJsonObject t = v.toObject();
        const double end = seconds(t.value("at")) + seconds(t.value("duration"));
        if (!std::isnan(end)) length = std::max(length, end);
    }
    return length;
}

// "a cozy background image for my candle shop" -> "cozy candle shop scene, ..."
QString imagePrompt(const QString &request)
{
    const QString req = request.toLower();
    static const QRegularExpression of(QStringLiteral(R"(\b(?:image|picture|photo|illustration|shot|background|backdrop)\s+of\s+(?:an?\s+|the\s+)?([a-z0-9' -]{3,60}?)(?=\s+(?:and|with|for|plus|then)\b|[,.;!?"]|$))"));
    if (const auto m = of.match(req); m.hasMatch()) return m.captured(1).trimmed() + QStringLiteral(", professional photo, soft natural light");
    static const QRegularExpression forWhat(QStringLiteral(R"(\bfor\s+(?:my|our|a|an|the)\s+(?:new\s+)?([a-z0-9' -]{3,40}?)(?=\s+(?:with|that|which|and|in|on|featuring|using)\b|[,.;!?]|$))"));
    static const QRegularExpression look(QStringLiteral(R"(\b([a-z-]+(?:\s+[a-z-]+)?)\s+(?:background|backdrop)\s*(?:images?|pictures?|photos?)?\b)"));
    QString subject = forWhat.match(req).captured(1).trimmed();
    subject.remove(QRegularExpression(QStringLiteral(R"(^(\d+\s*(s|sec|second)s?\s+)?(video|ad|advert|promo|reel|tiktok|commercial)\s+(for\s+)?)")));
    QString mood = look.match(req).captured(1).trimmed();
    mood.remove(QRegularExpression(QStringLiteral(R"(^(a|an|the|with|and|some|nice|generated|new|ai)\b\s*)")));
    mood.remove(QRegularExpression(QStringLiteral(R"(^(a|an|the)\b\s*)")));
    QStringList parts;
    if (!mood.isEmpty() && parseColor(mood).isEmpty()) parts << mood;
    if (!subject.isEmpty()) parts << subject;
    if (parts.isEmpty()) return QStringLiteral("soft abstract background with gentle light and depth, professional photo");
    return parts.join(' ') + QStringLiteral(" scene, professional product photography, soft warm light, shallow depth of field");
}

// "soft music" with the "elegant" style -> instruments and mood for ACE-Step.
QString musicPrompt(const QString &request, const QString &style)
{
    const QString req = request.toLower();
    static const struct {
        const char *pattern, *music;
    } moods[] = {
        {R"(\b(lo-?fi|chill ?hop)\b)", "lo-fi hip hop, mellow keys, soft drums, relaxed"},
        {R"(\b(soft|calm|gentle|cozy|cosy|relaxing|peaceful|warm|mellow|quiet)\b)", "gentle acoustic guitar and soft piano, warm and calm"},
        {R"(\b(upbeat|happy|fun|cheerful|bright|positive)\b)", "upbeat pop, claps, bright synths and guitar, happy"},
        {R"(\b(energetic|hype|pumping|intense|powerful|workout)\b)", "energetic electronic pop, punchy drums, driving bass"},
        {R"(\b(epic|cinematic|dramatic|trailer)\b)", "cinematic orchestral, big drums, rising strings"},
        {R"(\b(jazz|jazzy)\b)", "smooth jazz, upright bass, brushed drums, piano"},
        {R"(\b(rock|guitar)\b)", "rock, electric guitar riffs, driving drums"},
        {R"(\b(electronic|edm|techno|house|synth)\b)", "electronic dance, four on the floor, synth leads"},
        {R"(\b(hip ?hop|trap|rap)\b)", "modern hip hop beat, deep bass, crisp hi-hats"},
        {R"(\b(corporate|business|tech)\b)", "light corporate pop, plucked synths, optimistic"},
        {R"(\b(elegant|classy|luxury|luxurious|premium)\b)", "elegant piano and strings, refined and slow"},
        {R"(\b(playful|kids|cute|quirky)\b)", "playful ukulele and glockenspiel, bouncy"},
    };
    for (const auto &m : moods)
        if (matches(req, m.pattern)) return QString::fromLatin1(m.music);
    static const QHash<QString, QString> byStyle{
        {"energetic", "energetic electronic pop, punchy drums, driving bass"}, {"bold", "bold hip hop beat, heavy bass, confident"},
        {"playful", "playful ukulele and glockenspiel, bouncy"},             {"elegant", "elegant piano and strings, refined"},
        {"luxury", "luxurious slow cinematic strings and piano, warm"},      {"minimal", "minimal ambient electronic, soft pads"},
    };
    return byStyle.value(style, QStringLiteral("modern upbeat background music, light percussion, positive"));
}

bool projectHasMusic(const Draft &d, const MediaIndex &index)
{
    for (const QString &id : index.materialIds())
        if (const Material *m = d.material(id); m->kind == MaterialKind::Audio && !isVoiceover(*m)) return true;
    return false;
}

bool projectHasVisuals(const Draft &d, const MediaIndex &index)
{
    for (const QString &id : index.materialIds())
        if (d.material(id)->isVisual()) return true;
    return false;
}

}  // namespace

QString voiceoverScript(const QString &request)
{
    return matches(request.toLower(), kVoiceWords) ? quotedNear(request, kVoiceWords) : QString();
}

QStringList requestedGeneration(const QString &request, const QJsonObject &current, const Draft &d, const MediaIndex &index,
                                QJsonObject *plan)
{
    const QString req = request.toLower();
    QStringList notes;
    const bool removing = matches(req, R"(\b(remove|delete|without|get rid|take (it |them )?(off|out)|no more|mute|drop|stop)\b)");
    // Asking for new media, not just a video: "generate a background image", "compose some new upbeat music".
    const bool asksToMake = matches(req, R"(\b(generate|generated|create|compose|make|new|original|custom|ai)\b(\s+[a-z-]+){0,3}?\s+(music|songs?|tracks?|tunes?|jingle|soundtrack|images?|pictures?|photos?|illustrations?|backgrounds?|voice[ -]?overs?)\b)");
    // A voiceover the user already has ("with the voiceover") is used, not remade.
    bool haveVoiceFile = false;
    for (const QString &id : index.materialIds())
        if (const Material *m = d.material(id); m && m->kind == MaterialKind::Audio && isVoiceover(*m)) haveVoiceFile = true;
    const bool wantsVoice = !removing && matches(req, kVoiceWords) && (!haveVoiceFile || asksToMake);
    const bool wantsMusic = !removing && matches(req, kMusicWords) && (!projectHasMusic(d, index) || asksToMake);
    // An image when the request asks for one and isn't about the user's own pictures.
    const bool ownPictures = projectHasVisuals(d, index)
                             && matches(req, R"(\b(my|our|the|this|these|those)\s+(own\s+)?(photos?|pictures?|images?|clips?|videos?|footage|shots?)\b)");
    const bool imageWords = matches(req, R"(\b(images?|pictures?|photos?|illustrations?|artwork|visuals?|scenery|b-?roll|stills?)\b)")
                            || (requestedBackground(request).isEmpty() && matches(req, R"(\b(background|backdrop)\b)") && !projectHasVisuals(d, index));
    const bool wantsImage = !removing && imageWords && !ownPictures && (!projectHasVisuals(d, index) || asksToMake);

    // Keep only what was asked for.
    QJsonArray kept;
    QStringList dropped;
    bool haveVoice = false, haveMusic = false, haveImage = false;
    const QString quote = voiceoverScript(request);
    const QString voice = voiceFor(request);
    for (const QJsonValue &v : plan->value("generate").toArray()) {
        QJsonObject o = v.toObject();
        QString kind = o.value("kind").toString().trimmed().toLower();
        if (kind == QLatin1String("voice") || kind == QLatin1String("narration") || kind == QLatin1String("speech")) kind = QStringLiteral("voiceover");
        const bool wanted = kind == QLatin1String("voiceover") ? wantsVoice : kind == QLatin1String("music") ? wantsMusic : wantsImage;
        if (!wanted) {
            dropped << kind;
            continue;
        }
        if (kind == QLatin1String("voiceover")) {
            haveVoice = true;
            // The user's exact words, not the model's paraphrase.
            if (!quote.isEmpty() && o.value("text").toString().trimmed() != quote) {
                o.insert("text", quote);
                notes << QStringLiteral("the voiceover says \"%1\" as written").arg(quote);
            }
            if (!voice.isEmpty()) o.insert("voice", voice);
        }
        haveMusic |= kind == QLatin1String("music");
        haveImage |= kind == QLatin1String("image");
        kept.append(o);
    }
    if (!dropped.isEmpty()) notes << QStringLiteral("skipped generating %1 the request didn't ask for").arg(dropped.join(" and "));

    // What the plan already plays.
    bool playsVoice = false, playsMusic = false, showsImage = false;
    for (const QJsonObject &p : {current, *plan})
        for (const char *key : {"main", "overlays", "audio"})
            for (const QJsonValue &v : p.value(QLatin1String(key)).toArray()) {
                const Material *m = lookup(v.toObject().value("media").toString(), d, index);
                if (!m) continue;
                if (m->kind == MaterialKind::Audio) (isVoiceover(*m) ? playsVoice : playsMusic) = true;
                else if (generatedKind(m->name) == QLatin1String("image")) showsImage = true;
            }
    // Names for added items: the ones the plan already uses where they are
    // (models often skip the list but write {"media": "g1"}). An image takes
    // a picture's name; music takes a sound that runs the whole video or sits
    // low, a voiceover one that doesn't. No match: a fresh name, and the item
    // is placed later.
    static const QRegularExpression genName(QStringLiteral("^g\\d+$"));
    QSet<QString> used;
    for (const QJsonValue &v : kept) used << v.toObject().value("as").toString().toLower();
    QStringList pictureRefs, musicRefs, voiceRefs;
    const double videoLength = planLength(*plan);
    for (const char *key : {"main", "overlays", "audio"})
        for (const QJsonValue &v : plan->value(QLatin1String(key)).toArray()) {
            const QJsonObject o = v.toObject();
            const QString ref = o.value("media").toString().trimmed().toLower();
            if (!genName.match(ref).hasMatch() || used.contains(ref) || pictureRefs.contains(ref) || musicRefs.contains(ref)
                || voiceRefs.contains(ref))
                continue;
            if (QLatin1String(key) != QLatin1String("audio")) {
                pictureRefs << ref;
                continue;
            }
            const double volume = o.contains("volume") ? seconds(o.value("volume")) : 1.0;
            const double span = seconds(o.value("to")) - seconds(o.value("from"));
            const bool musicLike = volume < 0.9 || (videoLength > 0 && span >= 0.8 * videoLength);
            (musicLike ? musicRefs : voiceRefs) << ref;
        }
    auto nameFor = [&](const QString &kind) {
        QStringList &refs = kind == QLatin1String("image") ? pictureRefs : kind == QLatin1String("music") ? musicRefs : voiceRefs;
        QString name = refs.isEmpty() ? QString() : refs.takeFirst();
        for (int i = 1; name.isEmpty(); ++i) {
            const QString g = QStringLiteral("g%1").arg(i);
            if (!used.contains(g) && !pictureRefs.contains(g) && !musicRefs.contains(g) && !voiceRefs.contains(g)) name = g;
        }
        used << name;
        return name;
    };

    if (wantsImage && !haveImage && !showsImage) {
        const QString prompt = imagePrompt(request);
        kept.append(QJsonObject{{"kind", "image"}, {"prompt", prompt}, {"as", nameFor("image")}});
        notes << QStringLiteral("added a generated image (%1)").arg(prompt.section(',', 0, 0));
    }
    // The same words already playing (an earlier answer, or a retry) aren't asked for again.
    bool saysQuote = false;
    if (!quote.isEmpty()) {
        GenRequest same;
        same.kind = QStringLiteral("voiceover");
        same.text = quote;
        const QString stem = same.fileName().section(QLatin1Char('-'), 0, -2);
        for (const QJsonValue &v : plan->value("audio").toArray())
            if (const Material *m = lookup(v.toObject().value("media").toString(), d, index))
                saysQuote |= m->name.section(QLatin1Char('-'), 0, -2) == stem;
    }
    if (wantsVoice && !haveVoice && !saysQuote && (!quote.isEmpty() || !playsVoice)) {
        QString script = quote;
        if (script.isEmpty()) {  // read the on-screen words
            QStringList lines;
            for (const QJsonValue &v : plan->value("texts").toArray()) {
                QString t = v.toObject().value("text").toString().simplified();
                if (t.isEmpty() || lines.contains(t)) continue;
                if (!t.endsWith('.') && !t.endsWith('!') && !t.endsWith('?')) t += '.';
                lines << t;
            }
            script = lines.join(' ');
        }
        if (!script.isEmpty()) {
            QJsonObject o{{"kind", "voiceover"}, {"text", script}, {"as", nameFor("voiceover")}};
            if (!voice.isEmpty()) o.insert("voice", voice);
            kept.append(o);
            notes << QStringLiteral("added a voiceover saying \"%1\"").arg(script);
        }
    }
    if (wantsMusic && !haveMusic && !playsMusic) {
        const double want = requestedLength(request);
        const double length = want > 0 ? want : planLength(*plan);
        const QString style = plan->value("style").toString(current.value("style").toString());
        const QString prompt = musicPrompt(request, style);
        kept.append(QJsonObject{{"kind", "music"}, {"prompt", prompt}, {"seconds", std::ceil(length > 0 ? length : 15)}, {"as", nameFor("music")}});
        notes << QStringLiteral("added generated music (%1)").arg(prompt.section(',', 0, 0));
    }

    if (kept.isEmpty()) plan->remove("generate");
    else plan->insert("generate", kept);
    return notes;
}

QStringList placeGenerated(const QStringList &generated, const QString &request, const Draft &d, const MediaIndex &index,
                           QJsonObject *plan)
{
    QSet<QString> used;
    for (const char *key : {"main", "overlays", "audio"})
        for (const QJsonValue &v : plan->value(QLatin1String(key)).toArray())
            if (const Material *m = lookup(v.toObject().value("media").toString(), d, index)) used << index.alias(m->id);

    // How long the video should be: asked for, else long enough for the texts and the voiceover.
    double voiceEnd = 0;
    for (const QString &alias : generated)
        if (const Material *m = d.material(index.materialId(alias)); m && isVoiceover(*m)) voiceEnd = std::max(voiceEnd, 0.5 + m->durationUs / 1e6 + 0.7);
    const double want = requestedLength(request);
    double length = 0;
    for (double l : itemLengths(plan->value("main").toArray())) length += l;
    if (length <= 0) length = want > 0 ? want : std::max({5.0, voiceEnd, planLength(*plan)});

    QStringList notes;
    QJsonArray main = plan->value("main").toArray(), audio = plan->value("audio").toArray();
    // A generated voiceover always plays whole: the model can't know how long
    // it is. One made for this request is heard at full volume.
    const bool aboutVolume = matches(request.toLower(), R"(\b(louder|quieter|softer|volume|turn (it |the [a-z]+ )?(up|down))\b)");
    bool trimmed = false;
    for (qsizetype i = 0; i < audio.size(); ++i) {
        QJsonObject o = audio[i].toObject();
        const Material *m = lookup(o.value("media").toString(), d, index);
        if (!m || generatedKind(m->name) != QLatin1String("voiceover")) continue;
        const double len = std::floor(m->durationUs / 1e4) / 100;
        const double volume = o.contains("volume") ? seconds(o.value("volume")) : 1.0;
        const bool quiet = generated.contains(index.alias(m->id)) && !aboutVolume && !(volume >= 0.9);
        if (seconds(o.value("from")) == 0 && seconds(o.value("to")) == len && !quiet) continue;
        o.insert("from", 0);
        o.insert("to", len);
        if (quiet) o.insert("volume", 1);
        audio[i] = o;
        trimmed = true;
    }
    for (const QString &alias : generated) {
        const Material *m = d.material(index.materialId(alias));
        if (!m || used.contains(alias)) continue;
        if (m->kind == MaterialKind::Image) {
            const bool first = main.isEmpty();
            main.append(QJsonObject{{"media", alias}, {"from", 0}, {"to", round2(first ? length : 3)}});
            notes << (first ? QStringLiteral("used the generated image as the picture") : QStringLiteral("added the generated image as a clip"));
        } else if (isVoiceover(*m)) {
            // Start with the on-screen words it reads, if they're there.
            double at = length > 3 ? 0.5 : 0;
            const QString stem = QFileInfo(m->name).completeBaseName();
            for (const QJsonValue &v : plan->value("texts").toArray()) {
                const QJsonObject t = v.toObject();
                QString words = t.value("text").toString().toLower();
                words.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
                if (words.size() >= 4 && stem.contains(words)) {
                    at = std::max(0.0, seconds(t.value("at")));
                    break;
                }
            }
            const double len = m->durationUs / 1e6;
            audio.append(QJsonObject{{"media", alias}, {"at", round2(at)}, {"from", 0}, {"to", round2(len)}, {"volume", 1}});
            notes << QStringLiteral("put the voiceover in at %1s").arg(round2(at));
        } else if (m->kind == MaterialKind::Audio) {
            audio.prepend(QJsonObject{{"media", alias}, {"at", 0}, {"from", 0},
                                      {"to", round2(std::min(length, m->durationUs / 1e6))}, {"volume", 0.8}});
            notes << QStringLiteral("put the generated music under the video");
        }
    }
    if (!notes.isEmpty() || trimmed) {
        plan->insert("main", main);
        plan->insert("audio", audio);
    }
    return notes;
}

bool duckUnderVoiceover(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    if (matches(request.toLower(), R"(\b(louder|quieter|softer|volume|turn (it |the music |the song )?(up|down)|mute|unmute)\b)")) return false;
    QJsonArray audio = plan->value("audio").toArray();
    bool voice = false;
    for (const QJsonValue &v : audio)
        if (const Material *m = lookup(v.toObject().value("media").toString(), d, index); m && isVoiceover(*m)) voice = true;
    if (!voice) return false;
    // The voice is marked as speech; music keeps its level between phrases
    // and dips to a quarter (or to where the model already put it) while
    // the voice talks, like an editor riding the fader.
    bool changed = false;
    for (qsizetype i = 0; i < audio.size(); ++i) {
        QJsonObject o = audio[i].toObject();
        const Material *m = lookup(o.value("media").toString(), d, index);
        if (!m) continue;
        if (isVoiceover(*m)) {
            if (!o.value("speech").toBool()) o.insert("speech", true), changed = true;
            audio[i] = o;
            continue;
        }
        if (o.contains("duck") && seconds(o.value("duck")) > 0) continue;
        double vol = o.contains("volume") ? seconds(o.value("volume")) : 1;
        if (std::isnan(vol)) vol = 1;
        // Lowered for the voice already: bring it back up and duck instead.
        const double level = vol <= 0.4 ? 0.8 : vol;
        o.insert("volume", round2(level));
        o.insert("duck", round2(std::clamp(0.25 / level, 0.1, 0.6)));
        audio[i] = o;
        changed = true;
    }
    if (changed) plan->insert("audio", audio);
    return changed;
}

QString musicOffer(const QString &request, const QJsonObject &plan, const Draft &d, const MediaIndex &index)
{
    const QString req = request.toLower();
    const QString style = plan.value("style").toString();
    if (style.isEmpty() && !matches(req, R"(\b(ad|advert|advertisement|commercial|promo|promotional)\b)")) return {};
    if (matches(req, R"(\b(no|without|silent|mute|muted|quiet)\b)") || projectHasMusic(d, index)) return {};
    for (const QJsonValue &v : plan.value("generate").toArray())
        if (v.toObject().value("kind").toString() == QLatin1String("music")) return {};
    for (const QJsonValue &v : plan.value("audio").toArray())
        if (const Material *m = lookup(v.toObject().value("media").toString(), d, index); m && !isVoiceover(*m)) return {};
    return QStringLiteral("There's no music yet. Say \"add music\" and I'll compose %1 track for it.")
        .arg(style.isEmpty() ? QStringLiteral("a") : (QStringLiteral("aeiou").contains(style.at(0)) ? "an " : "a ") + style);
}

QStringList applySmartRequests(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QString req = request.toLower();
    const bool removing = matches(req, R"(\b(remove|delete|turn off|switch off|get rid of|no more|without|stop|don't|do not)\b)");
    QStringList notes;

    // Cut to the beat.
    const bool beatWords = matches(req, R"(\b(on|to|with) the (beat|beats|rhythm|music|song|drop)|\bbeat[- ]?(sync|synced|matched)|\bsync(ed)? (it |the cuts |everything )?(to|with) the (music|song|beat|track)|\bin time with the (music|song)|\brhythm(ic)?\b|\bcut(s)? (it )?to the (music|song|beat))");
    if (beatWords) {
        const QString mode = removing && matches(req, R"(\b(beat|rhythm|sync)\b)") ? QStringLiteral("off")
                             : matches(req, R"(\b(bars?|downbeats?|every (bar|measure)|slower cuts?)\b)") ? QStringLiteral("bars")
                                                                                                      : QStringLiteral("beats");
        if (plan->value("beat_sync").toString() != mode) {
            plan->insert("beat_sync", mode);
            notes << (mode == QLatin1String("off") ? QStringLiteral("stopped cutting to the beat")
                                                   : QStringLiteral("cutting to the %1").arg(mode == QLatin1String("bars") ? "bars" : "beat"));
        }
    }

    // Auto captions: "add captions", "subtitle it", "caption what she says" - not
    // "add a caption that says 'Hi'", which is a text.
    const bool quoted = request.contains(QLatin1Char('"')) || request.contains(QChar(0x201C)) || request.contains(QLatin1Char('\''));
    const bool autoCaptions = matches(req, R"(\b(captions|subtitles|subtitle (it|this|the video)|closed captions|transcribe|auto[- ]?captions?|caption (it|this|the video|the voice ?over|the voice|what (he|she|they|i|it) says?|the (speech|dialogue|words|audio|narration)))\b)")
                              && !(quoted && matches(req, R"(\b(a|the) caption\b)"));
    if (autoCaptions) {
        if (removing) {
            if (!plan->value("captions").toString().isEmpty() && plan->value("captions") != QLatin1String("none")) {
                plan->insert("captions", "none");
                notes << QStringLiteral("removed the captions");
            }
        } else if (plan->value("captions").toString().isEmpty() || plan->value("captions") == QLatin1String("none")) {
            const QString style = plan->value("style").toString();
            const bool lively = matches(req, R"(\b(tiktok|reels?|shorts|kinetic|animated|word by word|pop)\b)") || style == QLatin1String("energetic")
                                || style == QLatin1String("bold") || style == QLatin1String("playful");
            plan->insert("captions", lively ? "kinetic-caption" : "caption");
            notes << QStringLiteral("captioning what's said");
        }
    }

    // Duck music under talking.
    if (!removing && matches(req, R"(\b(duck(ing)?|(lower|quieter|soften|dip|drop) the (music|song|track) (when|while|whenever|under)|music (down|lower|quieter) (when|while|whenever|under)|under the (voice|voiceover|narration|talking|speech))\b)")) {
        QJsonArray audio = plan->value("audio").toArray();
        bool changed = false;
        for (qsizetype i = 0; i < audio.size(); ++i) {
            QJsonObject o = audio[i].toObject();
            const Material *m = lookup(o.value("media").toString(), d, index);
            if (!m) continue;
            if (isVoiceover(*m)) {
                if (!o.value("speech").toBool()) o.insert("speech", true), changed = true;
            } else if (!(o.contains("duck") && seconds(o.value("duck")) > 0)) {
                o.insert("duck", 0.3);
                changed = true;
            }
            audio[i] = o;
        }
        // The people talking are in the clips: mark main video with sound as speech.
        bool voiceInAudio = false;
        for (const QJsonValue &v : audio) voiceInAudio |= v.toObject().value("speech").toBool();
        if (!voiceInAudio) {
            QJsonArray main = plan->value("main").toArray();
            for (qsizetype i = 0; i < main.size(); ++i) {
                QJsonObject o = main[i].toObject();
                const Material *m = lookup(o.value("media").toString(), d, index);
                if (m && m->kind == MaterialKind::Video && m->hasAudio && !o.value("speech").toBool()) o.insert("speech", true), changed = true;
                main[i] = o;
            }
            plan->insert("main", main);
        }
        if (changed) {
            plan->insert("audio", audio);
            notes << QStringLiteral("the music dips whenever someone talks");
        }
    }

    // Remove the background.
    if (matches(req, R"(\b(remove|erase|drop|get rid of|cut out|delete|lose) (the |its |their |his |her )?(back ?ground|backdrop)|\bcut ?out\b|\bisolate (the )?(subject|product|person)|\bbackground removal\b|\bno background\b)")) {
        const bool photos = matches(req, R"(\b(photos?|pictures?|images?|stills?|products?|logo)\b)");
        QJsonArray main = plan->value("main").toArray();
        int added = 0;
        int visual = 0;
        for (const QJsonValue &v : main)
            if (const Material *m = lookup(v.toObject().value("media").toString(), d, index); m && m->isVisual()) ++visual;
        for (qsizetype i = 0; i < main.size(); ++i) {
            QJsonObject o = main[i].toObject();
            const Material *m = lookup(o.value("media").toString(), d, index);
            if (!m || !m->isVisual() || (photos && visual > 1 && m->kind != MaterialKind::Image)) continue;
            QJsonArray fx = o.value("effects").toArray();
            bool has = false;
            for (const QJsonValue &e : fx) has |= e.toObject().value("name").toString() == QLatin1String("cutout");
            if (has) continue;
            fx.prepend(QJsonObject{{"name", "cutout"}});
            o.insert("effects", fx);
            if (!o.contains("framing")) o.insert("framing", "fit");  // show the whole subject
            main[i] = o;
            ++added;
        }
        if (added) {
            plan->insert("main", main);
            notes << QStringLiteral("removed the background from %1 clip%2").arg(added).arg(added == 1 ? "" : "s");
        }
    }
    return notes;
}

namespace {

// The shot list of a media reference, building it if needed (fast: no model).
SceneIndex shotsOf(const Material *m)
{
    if (!m || (m->kind != MaterialKind::Video && m->kind != MaterialKind::Image)) return {};
    return sceneIndex(*m);
}

double itemLength(const QJsonObject &o, double fallback)
{
    const double f = seconds(o.value("from")), t = seconds(o.value("to"));
    if (!std::isnan(f) && !std::isnan(t) && t - f >= 0.3) return t - f;
    const double d = seconds(o.value("duration"));
    return std::isnan(d) || d < 0.3 ? fallback : d;
}

// Sets from/to to the best stretch of `shot` with `len` seconds (photos: just the length).
void placeInShot(QJsonObject &o, const Material &m, const Shot &shot, double len)
{
    if (m.kind == MaterialKind::Image) {
        o.insert("from", 0);
        o.insert("to", round2(len));
        return;
    }
    const qint64 want = qint64(len * 1e6);
    QPair<qint64, qint64> w = shot.bestWindow(want);
    if (want > shot.length()) {  // longer than the shot: start at the shot and run on
        const qint64 from = std::max<qint64>(0, std::min<qint64>(shot.start, m.durationUs - want));
        w = {from, std::min<qint64>(m.durationUs, from + want)};
    }
    o.insert("from", round2(w.first / 1e6));
    o.insert("to", round2(w.second / 1e6));
}

}  // namespace

QStringList resolveShots(const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    QStringList notes;
    for (const char *key : {"main", "overlays"}) {
        QJsonArray items = plan->value(QLatin1String(key)).toArray();
        bool changed = false;
        for (qsizetype i = 0; i < items.size(); ++i) {
            QJsonObject o = items[i].toObject();
            if (!o.contains("shot") && !o.contains("moment")) continue;
            const Material *m = lookup(o.value("media").toString(), d, index);
            const SceneIndex x = shotsOf(m);
            int shot = -1;
            if (o.contains("shot")) {
                const int n = o.value("shot").toInt(int(seconds(o.value("shot"))));
                if (n >= 1 && n <= x.shots.size()) shot = n - 1;
            }
            if (shot < 0 && o.contains("moment")) {
                const QList<ShotMatch> found = findShots(x, o.value("moment").toString(), 0.25);
                if (!found.isEmpty()) shot = found.first().shot;
            }
            o.remove("shot");
            o.remove("moment");
            if (shot >= 0 && m) {
                const double len = itemLength(o, std::min(3.0, x.shots[shot].length() > 0 ? x.shots[shot].length() / 1e6 : 3.0));
                placeInShot(o, *m, x.shots[shot], len);
                o.remove("duration");
                notes << QStringLiteral("used shot %1 of %2").arg(shot + 1).arg(m->name);
            }
            items[i] = o;
            changed = true;
        }
        if (changed) plan->insert(QLatin1String(key), items);
    }
    return notes;
}

QStringList pickBestMoments(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QString req = request.toLower();
    if (!matches(req, R"(\b(best|nicest|strongest|coolest|prettiest|most (beautiful|interesting|exciting|dramatic)|highlights?|good (parts|bits|shots|moments)|keepers|skip the (bad|blurry|shaky|boring)|without the (blurry|shaky|bad)|sharp(er)? (one|shot|part|bit|take)|steadier|less shaky|in focus|better (one|shot|part|bit|take)|not (so )?(blurry|shaky))\b)"))
        return {};
    QJsonArray main = plan->value("main").toArray();
    QHash<QString, QList<int>> usedShots;  // per file, so a file used twice gives two different shots
    int changed = 0;
    for (qsizetype i = 0; i < main.size(); ++i) {
        QJsonObject o = main[i].toObject();
        const Material *m = lookup(o.value("media").toString(), d, index);
        if (!m || m->kind != MaterialKind::Video) continue;
        const SceneIndex x = shotsOf(m);
        if (x.isEmpty()) continue;
        const double len = itemLength(o, 3);
        QList<int> &used = usedShots[m->id];
        int best = -1;
        double bestValue = -1;
        for (qsizetype k = 0; k < x.shots.size(); ++k) {
            const Shot &s = x.shots[k];
            if (used.contains(int(k)) && used.size() < x.shots.size()) continue;
            // A shot much shorter than the clip can't hold it: discount it.
            const double fit = std::min(1.0, (s.length() / 1e6) / std::max(0.3, len));
            const double value = s.score() * (0.5 + 0.5 * fit);
            if (value > bestValue) bestValue = value, best = int(k);
        }
        if (best < 0) continue;
        used << best;
        const double before = seconds(o.value("from"));
        placeInShot(o, *m, x.shots[best], len);
        if (std::isnan(before) || std::abs(seconds(o.value("from")) - before) > 0.05) ++changed;
        main[i] = o;
    }
    if (!changed) return {};
    plan->insert("main", main);
    return {QStringLiteral("picked the best moments (sharp, steady shots)")};
}

namespace {

struct ShotCandidate {
    QString id;  // material
    int shot;
    double weight, score;
};

// Shots that what the request names would point to, strongest first.
QList<ShotCandidate> namedShots(const QString &request, const Draft &d, const MediaIndex &index)
{
    // Words that say what to show, not how to edit.
    static const QSet<QString> generic{
        "make", "add", "put", "use", "using", "video", "second", "seconds", "start", "end", "first", "last", "then",
        "vertical", "horizontal", "square", "music", "song", "title", "text", "caption", "captions", "clip", "clips", "shot",
        "shots", "part", "parts", "with", "and", "the", "a", "an", "of", "for", "my", "our", "it", "this", "that", "to", "in",
        "on", "at", "from", "show", "showing", "ad", "reel", "tiktok", "story", "only", "just", "best", "good", "nice", "some",
        "long", "short", "quick", "fast", "slow", "one", "two", "three", "four", "five", "into", "where", "when", "like", "want",
        "need", "please", "can", "you", "me", "is", "are", "be", "do", "does", "around", "about", "after", "before", "same",
        "also", "intro", "outro", "welcome", "instead", "swap", "replace", "sharp", "blurry", "shaky"};
    auto stem = [](QString w) {
        for (const char *suffix : {"ing", "es", "s", "ed"})
            if (w.size() > 4 && w.endsWith(QLatin1String(suffix))) {
                w.chop(int(qstrlen(suffix)));
                break;
            }
        return w;
    };
    auto tokens = [&](const QString &text) {
        QSet<QString> out;
        for (const QString &w : text.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts))
            if (w.size() >= 3) out << stem(w);
        return out;
    };
    // Quoted words are text for the screen, not footage.
    QString req = request;
    req.replace(QRegularExpression(QStringLiteral("[\"“][^\"”]*[\"”]")), QStringLiteral(" "));
    QStringList want;
    for (const QString &w : req.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts))
        if (w.size() >= 3 && !generic.contains(w)) want << stem(w);
    // Words naming files are about files, which other checks handle.
    QSet<QString> fileWords;
    for (const QString &id : index.materialIds()) fileWords += tokens(QFileInfo(d.material(id)->name).completeBaseName());
    want.removeIf([&](const QString &w) { return fileWords.contains(w); });
    if (want.isEmpty()) return {};

    QList<ShotCandidate> found;
    for (const QString &id : index.materialIds()) {
        const Material *m = d.material(id);
        const SceneIndex x = cachedSceneIndex(*m);
        for (qsizetype k = 0; k < x.shots.size(); ++k) {
            const Shot &s = x.shots[k];
            const QSet<QString> about = tokens(s.description), subject = tokens(s.subjects.join(' ') + QLatin1Char(' ') + s.tags.join(' '));
            double weight = 0;
            for (const QString &w : want) weight += subject.contains(w) ? 1.5 : about.contains(w) ? 1.0 : 0.0;
            // A subject, two words of the description, or half of what the request names.
            if (weight >= 1.5 || (weight >= 1.0 && weight >= 0.5 * want.size())) found.append({id, int(k), weight, s.score()});
        }
    }
    if (qEnvironmentVariableIsSet("MONTAGE_DEBUG")) {
        QStringList f;
        for (const ShotCandidate &c : found) f << QStringLiteral("%1#%2=%3").arg(c.id).arg(c.shot + 1).arg(c.weight);
        qInfo("missingShots: want [%s] found [%s]", qPrintable(want.join(',')), qPrintable(f.join(' ')));
    }
    std::sort(found.begin(), found.end(), [](const ShotCandidate &a, const ShotCandidate &b) {
        return a.weight != b.weight ? a.weight > b.weight : a.score > b.score;
    });
    return found;
}

// Whether the plan uses a main or overlay item inside this shot.
bool planUsesShot(const QJsonObject &plan, const ShotCandidate &c, const Draft &d, const MediaIndex &index)
{
    const Material *m = d.material(c.id);
    const Shot s = cachedSceneIndex(*m).shots.value(c.shot);
    for (const char *key : {"main", "overlays"})
        for (const QJsonValue &v : plan.value(QLatin1String(key)).toArray()) {
            const QJsonObject o = v.toObject();
            if (lookup(o.value("media").toString(), d, index) != m) continue;
            if (o.contains("shot") && o.value("shot").toInt() == c.shot + 1) return true;
            const double f = seconds(o.value("from")), t = seconds(o.value("to"));
            if (m->kind == MaterialKind::Image || (!std::isnan(f) && !std::isnan(t) && std::min(t, s.end / 1e6) - std::max(f, s.start / 1e6) > 0.3))
                return true;
        }
    return false;
}

}  // namespace

QStringList missingShots(const QString &request, const QJsonObject &plan, const Draft &d, const MediaIndex &index)
{
    const QList<ShotCandidate> found = namedShots(request, d, index);
    if (found.isEmpty()) return {};
    const ShotCandidate best = found.first();
    // Fine if the plan uses this shot, or another that matches as well.
    for (const ShotCandidate &c : found)
        if (c.weight >= best.weight - 1e-9 && planUsesShot(plan, c, d, index)) return {};
    const Shot s = cachedSceneIndex(*d.material(best.id)).shots.value(best.shot);
    return {QStringLiteral("The request asks for footage your plan doesn't use: %1 shot %2 (%3-%4s, \"%5\"). Add {\"media\": \"%1\", \"shot\": %2} to \"main\".")
                .arg(index.alias(best.id))
                .arg(best.shot + 1)
                .arg(round2(s.start / 1e6))
                .arg(round2(s.end / 1e6))
                .arg(s.description.isEmpty() ? s.tags.mid(0, 3).join(", ") : s.description)};
}

QStringList applyNamedShots(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QList<ShotCandidate> found = namedShots(request, d, index);
    if (found.isEmpty()) return {};
    const ShotCandidate best = found.first();
    // Only a clear winner: twice as strong as the next, and naming two things or a subject.
    if (best.weight < 2 || (found.size() > 1 && found[1].weight * 1.5 > best.weight)) return {};
    if (planUsesShot(*plan, best, d, index)) return {};
    const Material *m = d.material(best.id);
    const SceneIndex x = cachedSceneIndex(*m);
    // Replace the item of that file that matches the request least.
    QJsonArray main = plan->value("main").toArray();
    int target = -1;
    double weakest = 1e9;
    for (qsizetype i = 0; i < main.size(); ++i) {
        const QJsonObject o = main[i].toObject();
        if (lookup(o.value("media").toString(), d, index) != m) continue;
        const double f = seconds(o.value("from"));
        double w = 0;
        for (const ShotCandidate &c : found)
            if (c.id == best.id && !std::isnan(f) && f >= x.shots.value(c.shot).start / 1e6 - 0.05 && f < x.shots.value(c.shot).end / 1e6) w = std::max(w, c.weight);
        if (w < weakest) weakest = w, target = int(i);
    }
    if (target < 0) return {};
    QJsonObject o = main[target].toObject();
    placeInShot(o, *m, x.shots[best.shot], itemLength(o, std::min(3.0, x.shots[best.shot].length() / 1e6)));
    main[target] = o;
    plan->insert("main", main);
    return {QStringLiteral("used shot %1 of %2 (%3)").arg(best.shot + 1).arg(m->name, x.shots[best.shot].description.isEmpty()
                                                                                          ? x.shots[best.shot].tags.mid(0, 2).join(", ")
                                                                                          : x.shots[best.shot].description)};
}

namespace {

struct LineHit {
    QString materialId;
    int line = -1;
    double weight = 0;
};

QString stemWord(QString w)
{
    for (const char *suffix : {"ing", "es", "s", "ed"})
        if (w.size() > 4 && w.endsWith(QLatin1String(suffix))) {
            w.chop(int(qstrlen(suffix)));
            break;
        }
    return w;
}

QSet<QString> stems(const QString &text)
{
    QSet<QString> out;
    for (const QString &w : text.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9']+")), Qt::SkipEmptyParts))
        if (w.size() >= 3) out << stemWord(w);
    return out;
}

// Lines of the given files that match what `phrase` names, strongest first.
QList<LineHit> findLines(const QString &phrase, const Draft &d, const QStringList &materialIds)
{
    static const QSet<QString> generic{"the", "and", "part", "parts", "bit", "line", "lines", "where", "when", "say", "says", "said",
                                       "saying", "talk", "talks", "talking", "about", "mention", "mentions", "that", "this", "cut",
                                       "remove", "delete", "drop", "take", "out", "get", "rid", "with", "start", "end", "then",
                                       "first", "last", "use", "put", "make", "video", "clip", "quote", "quotes", "sentence"};
    QStringList want;
    for (const QString &w : phrase.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9']+")), Qt::SkipEmptyParts))
        if (w.size() >= 3 && !generic.contains(w)) want << stemWord(w);
    // People say "my name" for "I'm Sam".
    const bool name = phrase.toLower().contains(QLatin1String("my name")) || phrase.toLower().contains(QLatin1String("introduce"));
    if (want.isEmpty() && !name) return {};
    QList<LineHit> out;
    for (const QString &id : materialIds) {
        const Material *m = d.material(id);
        if (!m) continue;
        const QList<Line> lines = transcriptLines(cachedFileTranscript(*m));
        for (qsizetype i = 0; i < lines.size(); ++i) {
            const QSet<QString> have = stems(lines[i].text);
            double hits = 0;
            for (const QString &w : want) hits += have.contains(w) ? 1 : 0;
            if (name && QRegularExpression(QStringLiteral(R"(\b(?i:i'm|i am|my name is|this is)\s+[A-Z])")).match(lines[i].text).hasMatch()) hits += 2;
            const double need = want.size() >= 2 ? std::max(2.0, want.size() * 0.5) : 1;
            if (hits >= need) out.append({id, int(i), hits});
        }
    }
    std::sort(out.begin(), out.end(), [](const LineHit &a, const LineHit &b) { return a.weight > b.weight; });
    return out;
}

// The [from, to) of a line in seconds, with a little air either side.
QPair<double, double> lineSpan(const Material &m, const Line &l)
{
    const double from = std::max(0.0, l.start / 1e6 - 0.06);
    const double to = std::min(m.durationUs / 1e6, l.end / 1e6 + 0.15);
    return {round2(from), round2(to)};
}

}  // namespace

QStringList resolveLines(const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    QStringList notes;
    for (const char *key : {"main", "overlays"}) {
        QJsonArray items = plan->value(QLatin1String(key)).toArray();
        bool changed = false;
        for (qsizetype i = 0; i < items.size(); ++i) {
            QJsonObject o = items[i].toObject();
            if (!o.contains("line") && !o.contains("lines") && !o.contains("says") && !o.contains("cut_lines")) continue;
            const Material *m = lookup(o.value("media").toString(), d, index);
            const QList<Line> lines = m ? transcriptLines(cachedFileTranscript(*m)) : QList<Line>{};
            int first = -1, last = -1;
            if (o.contains("line")) first = last = o.value("line").toInt() - 1;
            if (o.contains("lines")) {
                const QJsonArray r = o.value("lines").toArray();
                first = r.at(0).toInt() - 1;
                last = r.size() > 1 ? r.at(r.size() - 1).toInt() - 1 : first;
            }
            if (first < 0 && o.contains("says") && m) {
                const QList<LineHit> hits = findLines(o.value("says").toString(), d, {m->id});
                if (!hits.isEmpty()) first = last = hits.first().line;
            }
            if (first > last) std::swap(first, last);
            if (m && first >= 0 && last < lines.size()) {
                const auto a = lineSpan(*m, lines[first]), b = lineSpan(*m, lines[last]);
                o.insert("from", a.first);
                o.insert("to", b.second);
                o.remove("duration");
                notes << (first == last ? QStringLiteral("used line %1 of %2").arg(first + 1).arg(m->name)
                                        : QStringLiteral("used lines %1-%2 of %3").arg(first + 1).arg(last + 1).arg(m->name));
            }
            // Lines to take out of the item.
            if (m && o.contains("cut_lines")) {
                const double f = seconds(o.value("from")), t = seconds(o.value("to"));
                QJsonArray skips = o.value("skips").toArray();
                int cut = 0;
                for (const QJsonValue &v : o.value("cut_lines").toArray()) {
                    const int n = v.toInt() - 1;
                    if (n < 0 || n >= lines.size()) continue;
                    const auto span = lineSpan(*m, lines[n]);
                    if (!std::isnan(f) && !std::isnan(t) && (span.second <= f || span.first >= t)) continue;
                    skips.append(QJsonArray{span.first, span.second});
                    ++cut;
                }
                if (cut) {
                    o.insert("skips", skips);
                    notes << QStringLiteral("cut %1 line%2 from %3").arg(cut).arg(cut == 1 ? "" : "s").arg(m->name);
                }
            }
            o.remove("line");
            o.remove("lines");
            o.remove("says");
            o.remove("cut_lines");
            items[i] = o;
            changed = true;
        }
        if (changed) plan->insert(QLatin1String(key), items);
    }
    return notes;
}

QStringList applySpeechRequests(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QString req = request.toLower();
    QStringList notes;
    const bool keep = matches(req, R"(\b(keep|leave|don't (cut|remove)|do not (cut|remove)|put back|restore)\b)");
    if (matches(req, R"(\b(u+m+s?|u+h+s?|erm+s?|fillers?|filler words|pauses|silences?|dead air|gaps|tighten|tighter|snappier|jump ?cuts?|breathing room)\b)")) {
        const bool on = !keep;
        if (plan->value("clean_speech").toBool() != on) {
            plan->insert("clean_speech", on);
            notes << (on ? QStringLiteral("cutting the ums and long pauses") : QStringLiteral("keeping the ums and pauses"));
        }
    }
    // "Cut the part where I say my name": take that line out of the clip that has it.
    if (!keep && matches(req, R"(\b(cut|remove|delete|drop|lose|get rid of|take out|skip)\b)")
        && matches(req, R"(\b(part|bit|line|sentence|where|when|says?|said|mentions?|talks? about|my name|introduce)\b)")) {
        QStringList used;
        for (const QJsonValue &v : plan->value("main").toArray())
            if (const Material *m = lookup(v.toObject().value("media").toString(), d, index); m && !used.contains(m->id)) used << m->id;
        const QList<LineHit> hits = findLines(request, d, used);
        if (!hits.isEmpty() && (hits.size() == 1 || hits[1].weight < hits.first().weight)) {
            const Material *m = d.material(hits.first().materialId);
            const Line line = transcriptLines(cachedFileTranscript(*m)).value(hits.first().line);
            const auto span = lineSpan(*m, line);
            QJsonArray main = plan->value("main").toArray();
            for (qsizetype i = 0; i < main.size(); ++i) {
                QJsonObject o = main[i].toObject();
                if (lookup(o.value("media").toString(), d, index) != m) continue;
                const double f = seconds(o.value("from")), t = seconds(o.value("to"));
                if (std::isnan(f) || std::isnan(t) || span.second <= f || span.first >= t) continue;
                bool already = false;
                for (const QJsonValue &v : o.value("skips").toArray())
                    already |= seconds(v.toArray().at(0)) <= span.first + 0.05 && seconds(v.toArray().at(1)) >= span.second - 0.05;
                if (already) break;
                if (span.first <= f + 0.05 && span.second >= t - 0.05) {  // the whole item is that line
                    main.removeAt(i);
                } else {
                    QJsonArray skips = o.value("skips").toArray();
                    skips.append(QJsonArray{std::max(span.first, f), std::min(span.second, t)});
                    o.insert("skips", skips);
                    main[i] = o;
                }
                plan->insert("main", main);
                notes << QStringLiteral("cut \"%1\"").arg(line.text.left(50));
                break;
            }
        }
    }
    return notes;
}

namespace {

// "start with X, then Y" -> ["start with X", "Y"].
QStringList requestParts(const QString &request)
{
    QStringList parts;
    for (const QString &p : request.toLower().split(QRegularExpression(QStringLiteral(R"(\b(?:and then|then|followed by|after that|finally|next)\b|[,;])")), Qt::SkipEmptyParts))
        if (!p.trimmed().isEmpty()) parts << p.trimmed();
    return parts;
}

QStringList talkingFiles(const Draft &d, const MediaIndex &index)
{
    QStringList ids;
    for (const QString &id : index.materialIds())
        if (const Material *m = d.material(id); m && m->hasAudio && m->kind != MaterialKind::Audio && cachedFileTranscript(*m).done
                                                 && !cachedFileTranscript(*m).words.isEmpty())
            ids << id;
    return ids;
}

}  // namespace

QStringList arrangeLines(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QString req = request.toLower();
    QStringList notes;
    // "Remove the ums" isn't "remove these lines".
    const bool cutsLines = matches(req, R"(\b(cut|remove|delete|drop|lose|skip|take out|get rid of)\b.*\b(part|bit|line|lines|sentence|where|when|about|says?|said|mentions?|my name|introduce)\b)");
    if (!cutsLines) {
        QJsonArray main = plan->value("main").toArray();
        bool dropped = false;
        for (qsizetype i = 0; i < main.size(); ++i) {
            QJsonObject o = main[i].toObject();
            if (!o.contains("cut_lines")) continue;
            o.remove("cut_lines");
            main[i] = o;
            dropped = true;
        }
        if (dropped) {
            plan->insert("main", main);
            notes << QStringLiteral("kept every line");
        }
    }

    // An arrangement of lines, each part naming one line clearly.
    if (cutsLines || !matches(req, R"(\b(start|starts|begin|open|first|then|followed by|after that|end with|finish)\b)")) return notes;
    const QStringList files = talkingFiles(d, index);
    if (files.isEmpty()) return notes;
    QList<LineHit> picked;
    for (const QString &part : requestParts(request)) {
        const QList<LineHit> hits = findLines(part, d, files);
        if (hits.isEmpty()) continue;
        if (hits.size() > 1 && hits[1].weight >= hits[0].weight) return notes;  // ambiguous: leave it to the model
        picked << hits.first();
    }
    if (picked.size() < 2) return notes;
    // Only when the plan is made of those talking clips (don't throw away other footage).
    for (const QJsonValue &v : plan->value("main").toArray())
        if (const Material *m = lookup(v.toObject().value("media").toString(), d, index); m && !files.contains(m->id)) return notes;
    QJsonArray main;
    QStringList order;
    for (const LineHit &h : picked) {
        const Material *m = d.material(h.materialId);
        const Line line = transcriptLines(cachedFileTranscript(*m)).value(h.line);
        const auto span = lineSpan(*m, line);
        main.append(QJsonObject{{"media", index.alias(h.materialId)}, {"from", span.first}, {"to", span.second}});
        order << QStringLiteral("\"%1\"").arg(line.text.left(30));
    }
    if (main == plan->value("main").toArray()) return notes;
    plan->insert("main", main);
    notes << QStringLiteral("arranged the lines: %1").arg(order.join(QStringLiteral(", then ")));
    return notes;
}

namespace { QStringList missingLine(const LineHit &best, const QJsonObject &plan, const Draft &d, const MediaIndex &index); }

QStringList missingLines(const QString &request, const QJsonObject &plan, const Draft &d, const MediaIndex &index)
{
    // Only requests about what's said.
    if (!matches(request.toLower(), R"(\b(line|lines|quote|quotes|says?|said|saying|talks? about|mentions?|part where|bit where|soundbites?)\b)")) return {};
    if (matches(request.toLower(), R"(\b(cut|remove|delete|drop|lose|get rid of|take out|skip)\b)")) return {};
    const QStringList ids = talkingFiles(d, index);
    QList<LineHit> wanted;
    for (const QString &part : requestParts(request))
        if (const QList<LineHit> hits = findLines(part, d, ids); !hits.isEmpty()) wanted << hits.first();
    for (const LineHit &best : wanted) {
        const QStringList miss = missingLine(best, plan, d, index);
        if (!miss.isEmpty()) return miss;
    }
    return {};
}

namespace {
QStringList missingLine(const LineHit &best, const QJsonObject &plan, const Draft &d, const MediaIndex &index)
{
    const Material *m = d.material(best.materialId);
    const Line line = transcriptLines(cachedFileTranscript(*m)).value(best.line);
    for (const char *key : {"main", "overlays"})
        for (const QJsonValue &v : plan.value(QLatin1String(key)).toArray()) {
            const QJsonObject o = v.toObject();
            if (lookup(o.value("media").toString(), d, index) != m) continue;
            if (o.value("line").toInt() == best.line + 1) return {};
            const double f = seconds(o.value("from")), t = seconds(o.value("to"));
            if (!std::isnan(f) && !std::isnan(t) && f <= line.start / 1e6 + 0.3 && t >= line.end / 1e6 - 0.3) return {};
        }
    return {QStringLiteral("The request asks for what's said in %1 line %2 (\"%3\"), which your plan doesn't use. Add {\"media\": \"%1\", \"line\": %2} to \"main\".")
                .arg(index.alias(best.materialId))
                .arg(best.line + 1)
                .arg(line.text.left(60))};
}
}  // namespace

}  // namespace mtg::ai
