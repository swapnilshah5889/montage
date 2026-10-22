// AI-layer tests that need no real model:
//  - edit plans: round trip, validation, leniency
//  - both chat adapters against a local mock server replaying streamed responses
//  - generated media: plan field, resolution with fake generators, guards

#include "ai/AdStyles.h"
#include "ai/Brand.h"
#include "engine/Scenes.h"
#include "engine/Speech.h"
#include "ai/Finishing.h"
#include "ai/ChatProvider.h"
#include "ai/EditPlan.h"
#include "ai/Generators.h"
#include "ai/PlanGuards.h"
#include "engine/Animation.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cmath>
#include <cstdio>

using namespace mtg;
using namespace mtg::ai;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static Draft sampleDraft()
{
    Draft d;
    auto add = [&](const char *id, MaterialKind k, const char *name, double secs, bool audio, int w, int h) {
        Material m;
        m.id = id;
        m.kind = k;
        m.name = name;
        m.durationUs = qint64(secs * 1e6);
        m.hasAudio = audio;
        m.hasVideo = k != MaterialKind::Audio;
        m.width = w;
        m.height = h;
        d.materials.append(m);
    };
    add("a", MaterialKind::Video, "beach.mp4", 12, true, 1920, 1080);
    add("b", MaterialKind::Video, "city.mov", 8, true, 1080, 1920);
    add("c", MaterialKind::Audio, "song.mp3", 95, true, 0, 0);
    add("d", MaterialKind::Image, "logo.png", 0, false, 800, 800);
    return d;
}

static void animationTests()
{
    CHECK(ease("linear", 0.25) == 0.25 && ease("ease-out", 0) == 0 && ease("ease-out", 1) == 1);
    CHECK(std::abs(ease("bounce", 1) - 1) < 1e-9 && std::abs(ease("elastic", 1) - 1) < 1e-9 && ease("back", 0.7) > 1);

    Segment seg;
    seg.duration = 4'000'000;
    seg.transform.scale = 0.5;
    seg.motion.in = "pop";
    seg.motion.out = "fade";
    FrameState a = animate(seg, 0);
    CHECK(a.opacity == 0 && std::abs(a.scale - 0.2) < 1e-6);         // starts small and invisible
    FrameState b = animate(seg, 2'000'000);
    CHECK(std::abs(b.scale - 0.5) < 1e-6 && b.opacity == 1);          // settled at its resting size
    FrameState c = animate(seg, 3'800'000);
    CHECK(c.opacity > 0.45 && c.opacity < 0.55);                       // halfway through the 0.4s fade-out

    Segment keyed;
    keyed.duration = 2'000'000;
    keyed.keys["scale"] = {KeyPoint{0, 1, "linear"}, KeyPoint{2'000'000, 2, "linear"}};
    keyed.keys["rotation"] = {KeyPoint{1'000'000, 30, "linear"}};
    CHECK(std::abs(animate(keyed, 1'000'000).scale - 1.5) < 1e-6);
    CHECK(animate(keyed, 0).rotation == 30 && animate(keyed, 2'000'000).scale == 2);

    Segment kb;
    kb.duration = 3'000'000;
    kb.motion.loop = "ken-burns";
    CHECK(std::abs(animate(kb, 3'000'000).scale - 1.12) < 1e-6);

    Segment tw;
    tw.duration = 3'000'000;
    tw.motion.in = "typewriter";
    tw.motion.inUs = 1'000'000;
    const FrameState t = animate(tw, 250'000);
    CHECK(t.textMode == "typewriter" && std::abs(t.textProgress - 0.25) < 1e-6);
    CHECK(motionNames("in").contains("kinetic") && motionNames("loop").contains("pulse"));

    // Plans carry motions, rotation, keyframes, shadows and gradients losslessly.
    Draft base = sampleDraft();
    const QJsonObject plan = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 4, "in": "zoom-out",
        "loop": "ken-burns", "rotation": 5, "keys": {"scale": [{"t": 0, "v": 1, "ease": "linear"}, [4, 1.3, "back"]]}}],
        "overlays": [{"media": "m4", "at": 0.5, "from": 0, "to": 3, "position": "top-right", "scale": 0.25, "in": "pop",
        "out": "spin", "out_duration": 0.6, "loop": "pulse", "loop_speed": 1.5}],
        "texts": [{"text": "SALE", "at": 0, "duration": 3, "style": "title", "in": "kinetic", "in_duration": 1.2,
        "shadow": "black", "gradient": "#ffd60a"}, {"text": "x", "at": 0, "duration": 1, "style": "caption", "in": "wobble"}]})").object();
    PlanReport r;
    const Draft d = draftFromPlan(base, plan, MediaIndex(base), &r);
    CHECK(r.ok() && r.warnings.size() == 1 && r.warnings.first().contains("unknown \"in\" animation \"wobble\""));
    const Segment &clip = d.tracks[0].segments[0];
    CHECK(clip.motion.in == "zoom-out" && clip.motion.loop == "ken-burns" && clip.transform.rotation == 5);
    CHECK(clip.keys.value("scale").size() == 2 && clip.keys.value("scale")[1].ease == "back" && clip.keys.value("scale")[1].t == 4'000'000);
    bool sawText = false, sawLogo = false;
    for (const Track &tr : d.tracks)
        for (const Segment &sg : tr.segments) {
            if (sg.text.text == "SALE") sawText = sg.text.shadow == "#000000" && sg.text.gradient == "#FFD60A" && sg.motion.in == "kinetic" && sg.motion.inUs == 1'200'000;
            if (sg.materialId == "d") sawLogo = sg.motion.out == "spin" && sg.motion.outUs == 600'000 && sg.motion.loopSpeed == 1.5;
        }
    CHECK(sawText && sawLogo);
    const QJsonObject back = planFromDraft(d, MediaIndex(d));
    PlanReport r2;
    const Draft d2 = draftFromPlan(d, back, MediaIndex(d), &r2);
    CHECK(r2.ok() && r2.warnings.isEmpty() && planFromDraft(d2, MediaIndex(d2)) == back);
    CHECK(Draft::fromJson(d.toJson()).toJson() == d.toJson());

    // Effects on clips and effect moments (adjustment layers on top).
    const QJsonObject fxPlan = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 6,
        "effects": ["cinematic", {"name": "glow", "amount": 0.5}, {"name": "sparkle"}]}],
        "texts": [{"text": "SALE", "at": 0, "duration": 3, "style": "title"}],
        "moments": [{"effect": "flash", "at": 2, "duration": 0.4}, {"effect": "shake", "at": 4, "duration": 0.6, "amount": 0.7},
                    {"effect": "vintage", "at": 0, "duration": 6}]})").object();
    PlanReport rf;
    const Draft df = draftFromPlan(base, fxPlan, MediaIndex(base), &rf);
    CHECK(rf.ok() && rf.warnings.size() == 1 && rf.warnings.first().contains("unknown effect \"sparkle\""));
    CHECK(df.tracks[0].segments[0].effects.size() == 2 && df.tracks[0].segments[0].effects[1].amount == 0.5);
    int momentsSeen = 0;
    bool momentsOnTop = true, textBelow = false;
    for (int i = 0; i < df.tracks.size(); ++i)
        for (const Segment &sg : df.tracks[i].segments) {
            if (sg.materialId == kEffectMaterialId) ++momentsSeen;
            if (sg.materialId == kTextMaterialId) {
                textBelow = true;
                for (int j = 0; j < i; ++j)
                    for (const Segment &o : df.tracks[j].segments) momentsOnTop &= o.materialId != kEffectMaterialId;
            }
        }
    CHECK(momentsSeen == 3 && momentsOnTop && textBelow);
    const QJsonObject fxBack = planFromDraft(df, MediaIndex(df));
    CHECK(fxBack.value("moments").toArray().size() == 3 && fxBack.value("main").toArray()[0].toObject().value("effects").toArray().size() == 2);
    PlanReport rf2;
    const Draft df2 = draftFromPlan(df, fxBack, MediaIndex(df), &rf2);
    CHECK(rf2.ok() && rf2.warnings.isEmpty() && planFromDraft(df2, MediaIndex(df2)) == fxBack);
    CHECK(MediaIndex(df).materialIds().size() == 4);  // built-in text and effect materials stay hidden

    // Ad presets bring style, position and motion; edits to them round-trip.
    const QJsonObject adPlan = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 6}], "texts": [
        {"text": "SHOP NOW", "at": 3, "duration": 3, "style": "cta"},
        {"text": "50% OFF", "at": 0, "duration": 6, "style": "badge", "loop": "none"},
        {"text": "$29", "at": 1, "duration": 4, "style": "price", "shape": "tag", "box": "#FFFFFF", "shadow": "none"},
        {"text": "New", "at": 0, "duration": 2, "style": "title", "shape": "hexagon"}]})").object();
    PlanReport ra;
    const Draft da = draftFromPlan(base, adPlan, MediaIndex(base), &ra);
    CHECK(ra.ok() && ra.warnings.size() == 1 && ra.warnings.first().contains("unknown shape \"hexagon\""));
    for (const Track &tr : da.tracks)
        for (const Segment &sg : tr.segments) {
            if (sg.text.text == "SHOP NOW") CHECK(sg.text.shape == "pill" && sg.motion.in == "pop" && sg.motion.loop == "pulse");
            if (sg.text.text == "50% OFF") CHECK(sg.text.shape == "burst" && sg.motion.in == "spin" && sg.motion.loop.isEmpty());
            if (sg.text.text == "$29") CHECK(sg.text.shape == "tag" && sg.text.shadow.isEmpty() && sg.transform.rotation == -8);
        }
    const QJsonObject adBack = planFromDraft(da, MediaIndex(da));
    PlanReport ra2;
    const Draft da2 = draftFromPlan(da, adBack, MediaIndex(da), &ra2);
    CHECK(ra2.ok() && ra2.warnings.isEmpty() && planFromDraft(da2, MediaIndex(da2)) == adBack);
    for (const QJsonValue &v : adBack.value("texts").toArray())
        if (v.toObject().value("text") == "50% OFF") CHECK(v.toObject().value("loop").toString() == "none");
    CHECK(textPresetNames().contains("cta") && textShapeNames().contains("burst"));

    // Transitions sit on the incoming main-track clip.
    PlanReport rt;
    const Draft dt = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [
        {"media": "m1", "from": 0, "to": 3, "transition": "whip"},
        {"media": "m2", "from": 0, "to": 3, "transition": "whip", "transition_duration": 0.8},
        {"media": "m1", "from": 3, "to": 6, "transition": "teleport"}]})").object(), MediaIndex(base), &rt);
    CHECK(rt.ok() && rt.warnings.size() == 2);  // first clip can't transition; unknown name
    CHECK(dt.tracks[0].segments[0].transition.name.isEmpty() && dt.tracks[0].segments[1].transition.name == "whip");
    CHECK(dt.tracks[0].segments[1].transition.us == 800'000 && dt.tracks[0].segments[2].transition.name.isEmpty());
    const QJsonObject tBack = planFromDraft(dt, MediaIndex(dt));
    PlanReport rt2;
    CHECK(planFromDraft(draftFromPlan(dt, tBack, MediaIndex(dt), &rt2), MediaIndex(dt)) == tBack && rt2.warnings.isEmpty());
    CHECK(Draft::fromJson(dt.toJson()).toJson() == dt.toJson() && transitionNames().size() == 19);

    // Speed: source mapping, freezes and ramps.
    Segment fast;
    fast.duration = 3'000'000;
    fast.sourceStart = 1'000'000;
    fast.speed = 2;
    CHECK(fast.sourceAt(1'000'000) == 3'000'000 && fast.sourceSpan() == 6'000'000);
    Segment hold = fast;
    hold.speed = 0;
    CHECK(hold.sourceAt(0) == 1'000'000 && hold.sourceAt(2'500'000) == 1'000'000);
    for (const QString &curve : speedCurveNames()) {
        Segment r = fast;
        r.curve = curve;
        CHECK(r.sourceAt(0) == 1'000'000 && std::abs(r.sourceAt(3'000'000) - 7'000'000) < 2);
        qint64 last = -1;
        bool monotonic = true;
        for (int k = 0; k <= 30; ++k) {
            const qint64 v = r.sourceAt(k * 100'000);
            monotonic &= v >= last;
            last = v;
        }
        CHECK(monotonic);
    }
    Segment hero = fast;
    hero.curve = "hero";
    CHECK(hero.sourceAt(1'650'000) - hero.sourceAt(1'350'000) < 300'000);  // slow motion in the middle

    PlanReport rs;
    const Draft ds = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [
        {"media": "m1", "from": 0, "to": 6, "speed": 2, "speed_curve": "bullet"},
        {"media": "m1", "from": 6.5, "to": 6.5, "speed": 0, "duration": 1.5},
        {"media": "m2", "from": 0, "to": 4}]})").object(), MediaIndex(base), &rs);
    CHECK(rs.ok() && rs.warnings.isEmpty());
    CHECK(ds.tracks[0].segments[0].duration == 3'000'000 && ds.tracks[0].segments[0].curve == "bullet");
    CHECK(ds.tracks[0].segments[1].speed == 0 && ds.tracks[0].segments[1].duration == 1'500'000 && ds.tracks[0].segments[1].sourceStart == 6'500'000);
    CHECK(ds.duration() == 8'500'000);
    const QJsonObject sBack = planFromDraft(ds, MediaIndex(ds));
    PlanReport rs2;
    CHECK(planFromDraft(draftFromPlan(ds, sBack, MediaIndex(ds), &rs2), MediaIndex(ds)) == sBack && rs2.warnings.isEmpty());

    QJsonObject fitSpeed = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 6, "speed": 2},
        {"media": "m2", "from": 0, "to": 4}, {"media": "m1", "from": 8, "to": 8, "speed": 0, "duration": 1}]})").object();
    CHECK(fitToLength(12, base, MediaIndex(base), &fitSpeed));
    PlanReport rfs;
    CHECK(std::abs(draftFromPlan(base, fitSpeed, MediaIndex(base), &rfs).duration() / 1e6 - 12) < 0.06 && rfs.ok());
}

static void guardTests()
{
    const Draft base = sampleDraft();  // m1 beach 12s, m2 city 8s, m3 song 95s, m4 logo (photo)
    const MediaIndex index(base);

    CHECK(requestedLength("turn these into a 12s tiktok with the song") == 12);
    CHECK(requestedLength("Make a 10 second vertical video with the city first") == 10);
    CHECK(requestedLength("make the whole thing 9 seconds but keep all three shots") == 9);
    CHECK(requestedLength("Add a title for 3 seconds of the video") == 0);
    CHECK(requestedLength("Show my name as a lower third during the first 4 seconds") == 0);

    // A text-only request keeps the clips even if the model changed them.
    const QJsonObject current = QJsonDocument::fromJson(R"({"aspect": "original", "main": [{"media": "m1", "from": 0, "to": 6},
        {"media": "m2", "from": 0, "to": 6}], "overlays": [], "audio": [], "texts": []})").object();
    QJsonObject plan = QJsonDocument::fromJson(R"({"aspect": "16:9", "main": [{"media": "m1", "from": 0, "to": 4},
        {"media": "m2", "from": 0, "to": 4}], "overlays": [], "audio": [],
        "texts": [{"text": "Swapnil Shah", "at": 0, "duration": 4, "style": "lower-third"}]})").object();
    const QStringList kept = keepUnrequested("Show my name \"Swapnil Shah\" as a lower third during the first 4 seconds",
                                             current, base, index, &plan);
    CHECK(plan.value("main") == current.value("main"));
    CHECK(plan.value("aspect").toString() == "keep");
    CHECK(plan.value("texts").toArray().size() == 1);
    CHECK(kept.contains("the clips") && kept.contains("the shape"));
    // ...while a request about the clips may change them.
    QJsonObject plan2 = plan;
    plan2.insert("main", QJsonDocument::fromJson(R"([{"media": "m2", "from": 0, "to": 4}])").array());
    CHECK(keepUnrequested("remove the beach clip", current, base, index, &plan2).isEmpty() || plan2.value("main").toArray().size() == 1);
    CHECK(keepUnrequested("start over with just the city", current, base, index, &plan2).isEmpty());
    // Words inside quotes are on-screen text, not instructions: "Look at this" isn't a request about looks.
    QJsonObject quoted = plan;
    quoted.insert("main", QJsonDocument::fromJson(R"([{"media": "m1", "from": 0, "to": 6}, {"media": "m2", "from": 0, "to": 6},
        {"media": "m1", "from": 6, "to": 12}])").array());
    keepUnrequested("Add a caption \"Look at this\" from second 4 to second 8", current, base, index, &quoted);
    CHECK(quoted.value("main") == current.value("main"));

    // Fit to a requested length: videos absorb the change, music follows the end.
    QJsonObject fit = QJsonDocument::fromJson(R"({"main": [{"media": "m2", "from": 0, "to": 5}, {"media": "m1", "from": 0, "to": 6},
        {"media": "m1", "from": 6, "to": 12}], "audio": [{"media": "m3", "at": 0, "from": 0, "to": 17, "volume": 0.3}],
        "texts": [{"text": "Bye", "at": 14, "duration": 3, "style": "caption"}]})").object();
    CHECK(fitToLength(12, base, index, &fit));
    PlanReport rf;
    const Draft df = draftFromPlan(base, fit, index, &rf);
    CHECK(rf.ok() && std::abs(df.duration() / 1e6 - 12) < 0.06);
    bool musicCovers = false, captionEnds = false;
    for (const Track &t : df.tracks)
        for (const Segment &s : t.segments) {
            if (t.role == TrackRole::Audio) musicCovers = std::abs(s.end() / 1e6 - df.tracks[0].segments.last().end() / 1e6) < 0.06;
            if (s.materialId == kTextMaterialId) captionEnds = std::abs(s.end() - df.tracks[0].segments.last().end()) < 60'000;
        }
    CHECK(musicCovers && captionEnds);

    // A logo "for 2 seconds" keeps its 2 seconds; the videos take up the rest.
    QJsonObject logo = QJsonDocument::fromJson(R"({"main": [{"media": "m4", "from": 0, "to": 2}, {"media": "m1", "from": 0, "to": 12},
        {"media": "m2", "from": 0, "to": 8}]})").object();
    CHECK(fitToLength(14, base, index, &logo));
    const QJsonArray lm = logo.value("main").toArray();
    CHECK(lm[0].toObject().value("to").toDouble() == 2);
    double sum = 0;
    for (const QJsonValue &v : lm) sum += v.toObject().value("to").toDouble() - v.toObject().value("from").toDouble();
    CHECK(std::abs(sum - 14) < 0.06);
    CHECK(requestedBackground("make a 5 second intro on a dark blue background") == "#00008B");
    CHECK(requestedBackground("with a red backdrop") == "#FF0000");
    CHECK(requestedBackground("add a title in the background music") == "");
    CHECK(parseColor("Navy") == "#000080" && !parseColor("deep purple").isEmpty() && parseColor("sky").isEmpty());

    // A range running past the file's end ("to": 9 on an 8s file) still reaches the length.
    QJsonObject pastEnd = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 6}, {"media": "m2", "from": 3, "to": 9}]})").object();
    CHECK(fitToLength(12, base, index, &pastEnd));
    PlanReport rp;
    CHECK(std::abs(draftFromPlan(base, pastEnd, index, &rp).duration() / 1e6 - 12) < 0.06 && rp.warnings.isEmpty());

    // Already the right length: untouched.
    CHECK(!fitToLength(14, base, index, &logo));

    // Broken ranges (zero length, start past the file's end) still reach the requested length.
    QJsonObject broken = QJsonDocument::fromJson(R"({"main": [{"media": "m4", "from": 0, "to": 2}, {"media": "m1", "from": 2, "to": 12},
        {"media": "m2", "from": 14, "to": 14}]})").object();
    CHECK(fitToLength(20, base, index, &broken));
    PlanReport rb;
    const Draft db = draftFromPlan(base, broken, index, &rb);
    CHECK(rb.ok() && std::abs(db.duration() / 1e6 - 20) < 0.06);

    // "The logo at the end for 2 seconds" placed at the very end moves back to end with the video.
    PlanReport rl;
    const Draft dl = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 10}],
        "overlays": [{"media": "m4", "at": 10, "from": 0, "to": 2, "position": "center"}]})").object(), index, &rl);
    CHECK(rl.ok() && dl.duration() == 10'000'000);
    bool logoAtEnd = false;
    for (const Track &t : dl.tracks)
        for (const Segment &s : t.segments)
            if (t.role == TrackRole::Overlay) logoAtEnd = s.start == 8'000'000 && s.duration == 2'000'000;
    CHECK(logoAtEnd);
}

static void planTests()
{
    const Draft base = sampleDraft();
    const MediaIndex index(base);
    CHECK(index.alias("c") == "m3" && index.materialId("M2") == "b" && index.materialId("m9").isEmpty());

    const QJsonObject plan = QJsonDocument::fromJson(R"({
        "reply": "ok", "aspect": "9:16",
        "main": [{"media": "m2", "from": 0, "to": 5}, {"media": "m1", "from": "2", "to": "0:07"}, {"media": "m4", "from": 0, "to": 2}],
        "overlays": [{"media": "m4", "at": 1, "from": 0, "to": 3, "position": "top-right", "scale": 0.3}],
        "audio": [{"media": "m3", "at": 0, "from": 0, "to": 30, "volume": 0.3}],
        "texts": [{"text": "Weekend", "at": 0, "duration": 3, "style": "title", "color": "#FF3B30"},
                  {"text": "Bye", "at": 10, "duration": 3, "style": "caption"}],
        "export": false})").object();
    PlanReport report;
    const Draft d = draftFromPlan(base, plan, index, &report);
    CHECK(report.ok());
    CHECK(d.canvas.width == 1080 && d.canvas.height == 1920 && d.canvasLocked);
    CHECK(d.tracks[0].segments.size() == 3);
    CHECK(d.duration() == 12'000'000);  // 5 + 5 + 2; music and text trimmed to the video
    CHECK(d.tracks[0].segments[1].sourceStart == 2'000'000);
    CHECK(report.warnings.size() == 2);  // music cut to 12s, caption cut to end with the video
    int texts = 0, audio = 0, overlays = 0;
    for (const Track &t : d.tracks)
        for (const Segment &s : t.segments) {
            if (s.materialId == kTextMaterialId) ++texts;
            else if (t.role == TrackRole::Audio) ++audio;
            else if (t.role == TrackRole::Overlay) ++overlays;
        }
    CHECK(texts == 2 && audio == 1 && overlays == 1);

    // Round trip: plan -> draft -> plan -> draft gives the same timeline.
    const QJsonObject again = planFromDraft(d, MediaIndex(d));
    PlanReport r2;
    const Draft d2 = draftFromPlan(d, again, MediaIndex(d), &r2);
    CHECK(r2.ok() && r2.warnings.isEmpty());
    auto shape = [](const Draft &x) {
        QJsonObject j = x.toJson();
        QJsonArray tracks;
        for (const QJsonValue &t : j.value("tracks").toArray()) {
            QJsonArray segs;
            for (const QJsonValue &s : t.toObject().value("segments").toArray()) {
                QJsonObject o = s.toObject();
                o.remove("id");
                segs.append(o);
            }
            tracks.append(QJsonObject{{"role", t.toObject().value("role")}, {"segments", segs}});
        }
        j.insert("tracks", tracks);
        return j;
    };
    CHECK(shape(d) == shape(d2));
    if (shape(d) != shape(d2))
        std::fprintf(stderr, "%s\n---\n%s\n", QJsonDocument(shape(d)).toJson().constData(), QJsonDocument(shape(d2)).toJson().constData());

    // Errors that must be fixed by the model, with actionable messages.
    PlanReport r3;
    draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "m7", "from": 0, "to": 3},
        {"media": "m3", "from": 0, "to": 3}, {"media": "m1", "from": 13, "to": 14}, {"media": "m1", "from": 4, "to": 4}]})").object(),
                  index, &r3);
    CHECK(r3.errors.size() == 3);  // unknown id, audio in main, zero length; a start past the end is repaired
    CHECK(r3.text().contains("unknown media \"m7\". Use one of: m1, m2, m3, m4"));
    CHECK(r3.text().contains("m3 is an audio file"));
    CHECK(r3.text().contains("past the end of m1 (12s long); used its first 1s instead"));

    // Leniency: file names instead of ids, "to" beyond the file clamps with a warning.
    PlanReport r4;
    const Draft d4 = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "beach.mp4", "from": 10, "to": 20}]})").object(),
                                   index, &r4);
    CHECK(r4.ok() && r4.warnings.size() == 1 && d4.duration() == 2'000'000);

    // Repair: from/to written as times in the video (0-5, 5-10, 10-15) instead of file positions.
    PlanReport r5;
    const Draft d5 = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 5},
        {"media": "m2", "from": 5, "to": 10}, {"media": "m1", "from": 10, "to": 15}]})").object(), index, &r5);
    CHECK(r5.ok() && r5.warnings.size() == 1 && d5.duration() == 15'000'000);
    CHECK(d5.tracks[0].segments[2].sourceStart == 0 && d5.tracks[0].segments[1].sourceStart == 0);
    // ...but a valid plan that happens to be cumulative within the files is left alone.
    PlanReport r6;
    const Draft d6 = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 3},
        {"media": "m1", "from": 3, "to": 6}]})").object(), index, &r6);
    CHECK(r6.ok() && r6.warnings.isEmpty() && d6.tracks[0].segments[1].sourceStart == 3'000'000);

    // Text-only videos: no media needed, background colour round-trips.
    PlanReport r7;
    const Draft d7 = draftFromPlan(Draft(), QJsonDocument::fromJson(R"({"background": "#1e3a8a", "main": [],
        "texts": [{"text": "Intro", "at": 0, "duration": 4, "style": "title"}]})").object(), MediaIndex(Draft()), &r7);
    CHECK(r7.ok() && r7.warnings.isEmpty() && d7.duration() == 4'000'000 && d7.canvas.background == "#1E3A8A");
    CHECK(planFromDraft(d7, MediaIndex(d7)).value("background").toString() == "#1E3A8A");
    CHECK(Draft::fromJson(d7.toJson()).canvas.background == "#1E3A8A");

    CHECK(seconds(QJsonValue("1:30")) == 90 && seconds(QJsonValue("2.5s")) == 2.5 && std::isnan(seconds(QJsonValue("abc"))));
    CHECK(planSchema().value("required").toArray().size() == 7);
}

// ------------------------------------------------------------ mock server

struct Canned {
    int status = 200;
    QByteArray contentType = "text/event-stream";
    QByteArray body;
};

class MockServer : public QTcpServer
{
public:
    QList<Canned> replies;
    QList<QByteArray> requests;  // raw header block + body of each request
    QList<QJsonObject> bodies;

    MockServer() { listen(QHostAddress::LocalHost, 0); connect(this, &QTcpServer::newConnection, this, &MockServer::accept); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }

private:
    void accept()
    {
        while (QTcpSocket *s = nextPendingConnection()) {
            auto *buf = new QByteArray;
            connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                *buf += s->readAll();
                const int headerEnd = buf->indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                int length = 0;
                for (const QByteArray &line : buf->left(headerEnd).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (buf->size() < headerEnd + 4 + length) return;
                requests << *buf;
                bodies << QJsonDocument::fromJson(buf->mid(headerEnd + 4, length)).object();
                const Canned c = replies.isEmpty() ? Canned{500, "text/plain", "no reply queued"} : replies.takeFirst();
                QByteArray resp = "HTTP/1.1 " + QByteArray::number(c.status) + (c.status == 200 ? " OK" : " Error")
                                  + "\r\nContent-Type: " + c.contentType + "\r\nContent-Length: " + QByteArray::number(c.body.size())
                                  + "\r\nConnection: close\r\n\r\n" + c.body;
                s->write(resp);
                s->disconnectFromHost();
                buf->clear();
            });
            connect(s, &QTcpSocket::disconnected, s, [s, buf] {
                delete buf;
                s->deleteLater();
            });
        }
    }
};

static QByteArray sse(const QList<QJsonObject> &events, bool named)
{
    QByteArray out;
    for (const QJsonObject &e : events) {
        if (named) out += "event: " + e.value("type").toString().toUtf8() + "\n";
        out += "data: " + QJsonDocument(e).toJson(QJsonDocument::Compact) + "\n\n";
    }
    return out;
}

template <typename T, typename Signal>
static bool waitFor(T *obj, Signal sig, int ms = 5000)
{
    QEventLoop loop;
    bool fired = false;
    QObject::connect(obj, sig, &loop, [&] { fired = true; loop.quit(); });
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    return fired;
}

static void anthropicTests()
{
    MockServer server;
    const QJsonObject delta = QJsonObject{{"type", "content_block_delta"}};
    auto d = [&](int index, QJsonObject inner) {
        QJsonObject e = delta;
        e.insert("index", index);
        e.insert("delta", inner);
        return e;
    };
    server.replies << Canned{200, "text/event-stream", sse({
        QJsonObject{{"type", "message_start"}, {"message", QJsonObject{{"id", "msg_1"}}}},
        QJsonObject{{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "thinking"}, {"thinking", ""}}}},
        d(0, {{"type", "thinking_delta"}, {"thinking", "Checking the clips first."}}),
        d(0, {{"type", "signature_delta"}, {"signature", "sig-abc"}}),
        QJsonObject{{"type", "content_block_stop"}, {"index", 0}},
        QJsonObject{{"type", "content_block_start"}, {"index", 1}, {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}},
        d(1, {{"type", "text_delta"}, {"text", "Let me look."}}),
        QJsonObject{{"type", "content_block_stop"}, {"index", 1}},
        QJsonObject{{"type", "content_block_start"}, {"index", 2},
                    {"content_block", QJsonObject{{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "look_at_media"}, {"input", QJsonObject{}}}}},
        d(2, {{"type", "input_json_delta"}, {"partial_json", "{\"med"}}),
        d(2, {{"type", "input_json_delta"}, {"partial_json", "ia\": \"m1\", \"cou"}}),
        d(2, {{"type", "input_json_delta"}, {"partial_json", "nt\": 4}"}}),
        QJsonObject{{"type", "content_block_stop"}, {"index", 2}},
        QJsonObject{{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", "tool_use"}}}},
        QJsonObject{{"type", "message_stop"}}}, true)};
    server.replies << Canned{200, "text/event-stream", sse({
        QJsonObject{{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}},
        d(0, {{"type", "text_delta"}, {"text", "Done!"}}),
        QJsonObject{{"type", "content_block_stop"}, {"index", 0}},
        QJsonObject{{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", "end_turn"}}}}}, true)};

    ChatProvider *p = ChatProvider::create("anthropic", nullptr);
    ModelProfile prof;
    prof.kind = "anthropic";
    prof.baseUrl = server.url();
    prof.model = "claude-opus-5-5";
    p->setProfile(prof, "test-key");
    p->reset("SYSTEM", {ToolSpec{"look_at_media", "desc", QJsonObject{{"type", "object"}}}});
    p->addUserMessage("make a video");

    TurnResult got;
    QStringList notes;
    QObject::connect(p, &ChatProvider::turnFinished, [&](const TurnResult &r) { got = r; });
    QObject::connect(p, &ChatProvider::progressNote, [&](const QString &n) { notes << n; });
    p->run();
    CHECK(waitFor(p, &ChatProvider::turnFinished));
    CHECK(got.stop == StopReason::ToolUse && got.text == "Let me look.");
    CHECK(got.calls.size() == 1 && got.calls[0].id == "toolu_1" && got.calls[0].input.value("count").toInt() == 4);
    CHECK(notes == QStringList{"Checking the clips first."});

    const QByteArray head1 = server.requests.value(0).toLower();  // header names are case-insensitive
    CHECK(head1.contains("x-api-key: test-key") && head1.contains("anthropic-version: 2023-06-01"));
    CHECK(head1.contains("server-side-fallback-2026-07-01") && head1.contains("thinking-display-updates-2026-08-18"));
    const QJsonObject b1 = server.bodies.value(0);
    CHECK(b1.value("model").toString() == "claude-opus-5-5" && b1.value("stream").toBool());
    CHECK(b1.value("fallbacks").toString() == "default");
    CHECK(b1.value("thinking").toObject().value("display").toString() == "updates");
    CHECK(b1.value("output_config").toObject().value("effort").toString() == "medium");
    CHECK(b1.value("system").toArray().at(0).toObject().contains("cache_control"));
    CHECK(b1.value("tools").toArray().at(0).toObject().value("eager_input_streaming").toBool());

    p->addToolResults({ToolResult{"toolu_1", "look_at_media", "4 frames", {QByteArray("JPEGDATA")}, false}});
    p->run();
    CHECK(waitFor(p, &ChatProvider::turnFinished));
    CHECK(got.stop == StopReason::EndTurn && got.text == "Done!");

    // The assistant turn is replayed verbatim (thinking + signature, parsed tool input),
    // followed by one user message holding the tool_result with its image.
    const QJsonArray msgs = server.bodies.value(1).value("messages").toArray();
    CHECK(msgs.size() == 3);
    const QJsonArray assistant = msgs.at(1).toObject().value("content").toArray();
    CHECK(assistant.at(0).toObject().value("type") == "thinking" && assistant.at(0).toObject().value("signature") == "sig-abc");
    CHECK(assistant.at(2).toObject().value("input").toObject().value("media") == "m1");
    const QJsonObject result = msgs.at(2).toObject().value("content").toArray().at(0).toObject();
    CHECK(result.value("type") == "tool_result" && result.value("tool_use_id") == "toolu_1");
    CHECK(result.value("content").toArray().at(1).toObject().value("type") == "image");
    delete p;
}

static void openAiTests()
{
    MockServer server;
    auto chunk = [](QJsonObject delta, QJsonValue finish = QJsonValue()) {
        return QJsonObject{{"choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}}}};
    };
    // Tool-call arguments arrive split across two chunks.
    auto toolChunk = [&](QJsonObject call) {
        call.insert("index", 0);
        return chunk(QJsonObject{{"tool_calls", QJsonArray{call}}});
    };
    QByteArray body = sse({
        chunk({{"role", "assistant"}, {"content", "Sure, "}}),
        chunk({{"content", "editing."}}),
        toolChunk({{"id", "call_9"}, {"type", "function"},
                   {"function", QJsonObject{{"name", "apply_plan"}, {"arguments", "{\"plan\": {\"ma"}}}}),
        toolChunk({{"function", QJsonObject{{"arguments", "in\": []}}"}}}}),
        chunk({}, "tool_calls")}, false);
    body += "data: [DONE]\n\n";
    server.replies << Canned{200, "text/event-stream", body};
    server.replies << Canned{200, "text/event-stream", sse({chunk({{"content", "All set."}}), chunk({}, "stop")}, false) + "data: [DONE]\n\n"};
    // requestJson: schema rejected once, then a fenced JSON answer.
    server.replies << Canned{400, "application/json", R"({"error": {"message": "response_format json_schema not supported"}})"};
    server.replies << Canned{200, "application/json",
                             QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", QJsonObject{
                                 {"content", "Here you go:\n```json\n{\"reply\": \"ok\", \"main\": []}\n```"}}}}}}}).toJson()};

    ChatProvider *p = ChatProvider::create("openai", nullptr);
    ModelProfile prof;
    prof.baseUrl = server.url() + "/v1";
    prof.model = "test-model";
    prof.vision = true;
    p->setProfile(prof, "sk-test");
    p->reset("SYSTEM", {ToolSpec{"apply_plan", "desc", QJsonObject{{"type", "object"}}}});
    p->addUserMessage("hi");
    TurnResult got;
    QString streamed;
    QObject::connect(p, &ChatProvider::turnFinished, [&](const TurnResult &r) { got = r; });
    QObject::connect(p, &ChatProvider::textDelta, [&](const QString &t) { streamed += t; });
    p->run();
    CHECK(waitFor(p, &ChatProvider::turnFinished));
    CHECK(got.stop == StopReason::ToolUse && streamed == "Sure, editing.");
    CHECK(got.calls.size() == 1 && got.calls[0].id == "call_9" && got.calls[0].inputOk);
    CHECK(got.calls[0].input.value("plan").toObject().contains("main"));
    CHECK(server.requests.value(0).toLower().contains("authorization: bearer sk-test"));
    CHECK(server.bodies.value(0).value("tools").toArray().at(0).toObject().value("type") == "function");

    p->addToolResults({ToolResult{"call_9", "apply_plan", "Applied.", {QByteArray("IMG")}, false}});
    p->run();
    CHECK(waitFor(p, &ChatProvider::turnFinished));
    CHECK(got.stop == StopReason::EndTurn && got.text == "All set.");
    const QJsonArray msgs = server.bodies.value(1).value("messages").toArray();
    // system, user, assistant(tool_calls), tool, user(images)
    CHECK(msgs.size() == 5);
    CHECK(msgs.at(2).toObject().value("tool_calls").toArray().size() == 1);
    CHECK(msgs.at(3).toObject().value("role") == "tool" && msgs.at(3).toObject().value("tool_call_id") == "call_9");
    CHECK(msgs.at(4).toObject().value("content").toArray().at(2).toObject().value("type") == "image_url");

    QJsonObject json;
    QString jsonErr;
    QObject::connect(p, &ChatProvider::jsonFinished, [&](const QJsonObject &o, const QString &, const QString &e) {
        json = o;
        jsonErr = e;
    });
    p->requestJson("SYS", "USER", planSchema());
    CHECK(waitFor(p, &ChatProvider::jsonFinished));
    CHECK(jsonErr.isEmpty() && json.value("reply") == "ok");
    CHECK(server.bodies.value(2).value("response_format").toObject().value("type") == "json_schema");
    CHECK(server.bodies.value(3).value("response_format").toObject().value("type") == "json_object");
    delete p;
}

static void styleTests()
{
    const Draft base = sampleDraft();  // m1 beach 12s, m2 city 8s, m3 song 95s, m4 logo (photo)
    const MediaIndex index(base);
    const QJsonObject empty = planFromDraft(base, index);

    // Energetic: transitions on every cut, a look on the clips, Ken Burns on the
    // photo, a flash on the first cut and punch-ins on the others, text motion.
    QJsonObject plan = QJsonDocument::fromJson(R"({"style": "Energetic", "main": [{"media": "m1", "from": 0, "to": 3},
        {"media": "m2", "from": 0, "to": 3}, {"media": "m4", "from": 0, "to": 2}, {"media": "m1", "from": 5, "to": 7,
        "transition": "none"}], "texts": [{"text": "SALE", "at": 0, "duration": 2, "style": "headline"},
        {"text": "plain", "at": 3, "duration": 2, "style": "caption", "in": "typewriter"}]})").object();
    CHECK(!applyAdStyle(empty, base, index, &plan).isEmpty());
    QJsonArray main = plan.value("main").toArray();
    CHECK(plan.value("style") == "energetic");
    CHECK(!main[0].toObject().contains("transition"));
    CHECK(main[1].toObject().value("transition") == "whip" && main[2].toObject().value("transition") == "zoom");
    CHECK(main[3].toObject().value("transition") == "none");  // explicit choice kept
    CHECK(main[0].toObject().value("effects").toArray()[0].toObject().value("name") == "vivid");
    CHECK(main[2].toObject().value("loop") == "ken-burns");
    const QJsonArray moments = plan.value("moments").toArray();
    CHECK(moments.size() == 3 && moments[0].toObject().value("effect") == "flash" && moments[0].toObject().value("at").toDouble() == 3);
    CHECK(moments[1].toObject().value("effect") == "zoom-punch" && moments[2].toObject().value("at").toDouble() == 8);
    const QJsonArray texts = plan.value("texts").toArray();
    CHECK(texts[1].toObject().value("in") == "typewriter" && texts[1].toObject().value("out") == "fade");

    PlanReport r;
    Draft styled = draftFromPlan(base, plan, index, &r);
    CHECK(r.ok() && styled.style == "energetic");
    CHECK(styled.tracks[0].segments[1].transition.name == "whip");
    const QJsonObject round = planFromDraft(styled, MediaIndex(styled));
    CHECK(round.value("style") == "energetic");

    // Same style again: nothing new is added, removed transitions stay removed.
    QJsonObject again = round;
    QJsonArray am = again.value("main").toArray();
    QJsonObject second = am[1].toObject();
    second.remove("transition");
    second.remove("transition_duration");
    am[1] = second;
    again.insert("main", am);
    CHECK(applyAdStyle(round, styled, MediaIndex(styled), &again).isEmpty());
    CHECK(!again.value("main").toArray()[1].toObject().contains("transition"));
    // ...but a clip added at the end gets the treatment.
    QJsonObject more = round;
    QJsonArray mm = more.value("main").toArray();
    mm.append(QJsonObject{{"media", "m2"}, {"from", 4}, {"to", 6}});
    more.insert("main", mm);
    applyAdStyle(round, styled, MediaIndex(styled), &more);
    CHECK(more.value("main").toArray().last().toObject().contains("transition"));
    CHECK(more.value("main").toArray().last().toObject().value("effects").toArray().size() == 1);

    // Switching to luxury replaces the energetic choices, keeps explicit ones.
    QJsonObject lux = round;
    lux.insert("style", "luxury");
    applyAdStyle(round, styled, MediaIndex(styled), &lux);
    const QJsonArray lm = lux.value("main").toArray();
    CHECK(lm[1].toObject().value("transition") == "fade-black" && lm[1].toObject().value("transition_duration").toDouble() > 0.8);
    CHECK(lm[3].toObject().value("transition") == "none");
    CHECK(lm[0].toObject().value("effects").toArray()[0].toObject().value("name") == "cinematic");
    CHECK(lux.value("moments").toArray().isEmpty());
    CHECK(lux.value("texts").toArray()[1].toObject().value("in") == "typewriter");
    CHECK(lux.value("texts").toArray()[1].toObject().value("out") == "fade");

    // Style "none" clears the style but keeps the edit's choices as they are.
    QJsonObject off = round;
    off.insert("style", "none");
    applyAdStyle(round, styled, MediaIndex(styled), &off);
    PlanReport ro;
    CHECK(draftFromPlan(styled, off, MediaIndex(styled), &ro).style.isEmpty());

    // Short clips get transitions that fit.
    QJsonObject tight = QJsonDocument::fromJson(R"({"style": "luxury", "main": [{"media": "m1", "from": 0, "to": 0.5},
        {"media": "m2", "from": 0, "to": 3}]})").object();
    applyAdStyle(empty, base, index, &tight);
    CHECK(tight.value("main").toArray()[1].toObject().value("transition_duration").toDouble() <= 0.4);

    // Feature checks name what's missing with exact JSON.
    const QJsonObject twoClips = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 5}, {"media": "m2", "from": 0, "to": 5}]})").object();
    QStringList miss = missingFeatures("Add a flash when the second clip starts", twoClips);
    CHECK(miss.size() == 1 && miss[0].contains("\"flash\"") && miss[0].contains("at 5 seconds"));
    CHECK(missingFeatures("Make the first clip slow motion", twoClips).value(0).contains("\"speed\": 0.5"));
    CHECK(missingFeatures("make the whole video black and white", twoClips).value(0).contains("black-white"));
    CHECK(missingFeatures("remove the flash", twoClips).isEmpty());
    QJsonObject flashed = twoClips;
    flashed.insert("moments", QJsonArray{QJsonObject{{"effect", "flash"}, {"at", 5}, {"duration", 0.3}}});
    CHECK(missingFeatures("Add a flash when the second clip starts", flashed).isEmpty());
    CHECK(missingFeatures("make the title pop in", QJsonDocument::fromJson(R"({"texts": [{"text": "Hi", "at": 0, "duration": 2}]})").object())
              .value(0).contains("\"in\": \"pop\""));

    // A styling request keeps the clips and takes only the styling.
    QJsonObject restyle = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 5, "effects": [{"name": "black-white"}]}]})").object();
    keepUnrequested("make the whole video black and white", twoClips, base, index, &restyle);
    CHECK(restyle.value("main").toArray().size() == 2);
    CHECK(restyle.value("main").toArray()[0].toObject().value("effects").toArray().size() == 1);
    // ...but a structural request may change them.
    QJsonObject shorter = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 3}]})").object();
    keepUnrequested("remove the city clip and make it black and white", twoClips, base, index, &shorter);
    CHECK(shorter.value("main").toArray().size() == 1);

    // New ads get music; edits and "no music" don't.
    QJsonObject newAd = QJsonDocument::fromJson(R"({"style": "bold", "main": [{"media": "m1", "from": 0, "to": 6}]})").object();
    CHECK(addAdMusic("make a bold ad for my gym", QJsonObject{}, base, index, &newAd));
    CHECK(newAd.value("audio").toArray()[0].toObject().value("media") == "m3" && newAd.value("audio").toArray()[0].toObject().value("to").toDouble() == 6);
    QJsonObject silentAd = QJsonDocument::fromJson(R"({"style": "bold", "main": [{"media": "m1", "from": 0, "to": 6}]})").object();
    CHECK(!addAdMusic("make a bold ad without music", QJsonObject{}, base, index, &silentAd));

    // Unambiguous targets are applied directly.
    QJsonObject photoPlan = QJsonDocument::fromJson(R"({"main": [{"media": "m4", "from": 0, "to": 3}, {"media": "m1", "from": 0, "to": 5}]})").object();
    CHECK(applyObviousFeatures("Slowly zoom into the photo", base, index, &photoPlan).size() == 1);
    CHECK(photoPlan.value("main").toArray()[0].toObject().value("loop") == "ken-burns");
    CHECK(!photoPlan.value("main").toArray()[1].toObject().contains("loop"));
    QJsonObject bw = twoClips;
    CHECK(applyObviousFeatures("make the whole video black and white", base, index, &bw).size() == 1);
    CHECK(bw.value("main").toArray()[1].toObject().value("effects").toArray()[0].toObject().value("name") == "black-white");
    QJsonObject warmTitle = twoClips;
    CHECK(applyObviousFeatures("make the title warm yellow", base, index, &warmTitle).isEmpty());

    // Hits on a named cut are placed by the app.
    QJsonObject cutFlash = twoClips;
    CHECK(applyObviousFeatures("Add a flash when the second clip starts", base, index, &cutFlash).size() == 1);
    CHECK(cutFlash.value("moments").toArray()[0].toObject().value("at").toDouble() == 5);
    QJsonObject everyCut = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 3}, {"media": "m2", "from": 0, "to": 3},
        {"media": "m1", "from": 4, "to": 7}]})").object();
    applyObviousFeatures("add a zoom punch on every cut", base, index, &everyCut);
    CHECK(everyCut.value("moments").toArray().size() == 2 && everyCut.value("moments").toArray()[1].toObject().value("at").toDouble() == 6);
    QJsonObject flashTitle = twoClips;
    CHECK(applyObviousFeatures("make the title flash", base, index, &flashTitle).isEmpty());

    // Multi-format exports.
    CHECK(requestedFormats("export it for tiktok and youtube") == QStringList({"9:16", "16:9"}));
    CHECK(requestedFormats("export for all platforms").size() == 4);
    CHECK(requestedFormats("make it vertical for tiktok").isEmpty());
    CHECK(requestedFormats("export the video").isEmpty());
    {
        Draft wide = sampleDraft();
        PlanReport rw;
        wide = draftFromPlan(wide, QJsonDocument::fromJson(R"({"aspect": "16:9", "main": [{"media": "m1", "from": 0, "to": 4}],
            "overlays": [{"media": "m4", "at": 0, "from": 0, "to": 4, "position": "top-right", "scale": 0.2}],
            "texts": [{"text": "Hi", "at": 0, "duration": 2, "style": "title"}]})").object(), MediaIndex(wide), &rw);
        const Draft tall = resizedDraft(wide, "9:16");
        CHECK(tall.canvas.width == 1080 && tall.canvas.height == 1920 && tall.duration() == wide.duration());
        const QJsonObject tp = planFromDraft(tall, MediaIndex(tall));
        CHECK(tp.value("overlays").toArray()[0].toObject().value("position") == "top-right");
        CHECK(tp.value("texts").toArray().size() == 1);
        CHECK(resizedDraft(wide, "4:5").canvas.height == 1350);
    }

    // "At the end" texts finish with the video.
    QJsonObject cta = twoClips;
    cta.insert("texts", QJsonArray{QJsonObject{{"text", "Shop now"}, {"at", 6}, {"duration", 2}, {"style", "cta"}}});
    CHECK(snapEndTexts("Add a Shop now button at the end", twoClips, &cta));
    CHECK(cta.value("texts").toArray()[0].toObject().value("at").toDouble() == 8);
    QJsonObject named = twoClips;  // named after "end with", wherever the model put it
    named.insert("texts", QJsonArray{QJsonObject{{"text", "Shop now"}, {"at", 2}, {"duration", 3}, {"style", "cta"}}});
    CHECK(snapEndTexts("Make an ad and end with a Shop now button", twoClips, &named));
    CHECK(named.value("texts").toArray()[0].toObject().value("at").toDouble() == 7);
    QJsonObject early = twoClips;
    early.insert("texts", QJsonArray{QJsonObject{{"text", "Hi"}, {"at", 0}, {"duration", 2}}});
    CHECK(!snapEndTexts("add Hi at the start and end with a fade", twoClips, &early));

    // Speed nobody asked for is dropped.
    QJsonObject fast = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 4, "speed": 2}]})").object();
    keepUnrequested("use the best 2 seconds of each clip", QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 4}]})").object(),
                    styled, MediaIndex(styled), &fast);
    CHECK(!fast.value("main").toArray()[0].toObject().contains("speed"));

    // The guards keep a style the request didn't ask to change.
    QJsonObject keep = round;
    keep.remove("style");
    keepUnrequested("move the title up", round, styled, MediaIndex(styled), &keep);
    CHECK(keep.value("style") == "energetic");
    QJsonObject change = round;
    change.insert("style", "elegant");
    keepUnrequested("make it feel more elegant", round, styled, MediaIndex(styled), &change);
    CHECK(change.value("style") == "elegant");
}

static void brandTests()
{
    Draft base = sampleDraft();  // m4 is logo.png
    base.materials[3].path = "/brand/logo.png";
    const MediaIndex index(base);
    BrandKit kit;
    kit.primary = "#FF5A1F";
    kit.accent = "#FFE14D";
    kit.font = "Avenir Next";
    kit.logo = "/brand/logo.png";
    kit.style = "bold";

    CHECK(!brandApplies("make a 10 second ad", kit));
    CHECK(brandApplies("make it on-brand", kit));
    kit.enabled = true;
    CHECK(brandApplies("make a 10 second ad", kit));
    CHECK(!brandApplies("anything", BrandKit{}));

    QJsonObject plan = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 6}],
        "texts": [{"text": "Big Sale", "at": 0, "duration": 2, "style": "headline", "color": "#00FF00"},
                  {"text": "Shop now", "at": 4, "duration": 2, "style": "cta"},
                  {"text": "50% OFF", "at": 2, "duration": 2, "style": "badge"}]})").object();
    const QStringList notes = applyBrand("make a 6 second ad for my shop", kit, QJsonObject{}, base, index, &plan);
    CHECK(notes.size() == 3);
    CHECK(plan.value("style") == "bold");
    const QJsonArray texts = plan.value("texts").toArray();
    CHECK(texts[0].toObject().value("color") == "#FFFFFF" && texts[0].toObject().value("font") == "Avenir Next");
    CHECK(texts[1].toObject().value("box") == "#FF5A1F" && texts[1].toObject().value("color") == "#FFFFFF");
    CHECK(texts[2].toObject().value("box") == "#FFE14D" && texts[2].toObject().value("color") == "#111111");  // dark on yellow
    const QJsonArray overlays = plan.value("overlays").toArray();
    CHECK(overlays.size() == 1 && overlays[0].toObject().value("media") == "m4" && overlays[0].toObject().value("to").toDouble() == 6);

    // A colour the user names wins; an existing edit keeps its logo choice.
    QJsonObject red = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 6}],
        "texts": [{"text": "Hi", "at": 0, "duration": 2, "style": "cta", "color": "#FF0000"}]})").object();
    applyBrand("add a red Hi button", kit, QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 6}]})").object(), base, index, &red);
    CHECK(red.value("texts").toArray()[0].toObject().value("color") == "#FF0000");
    CHECK(red.value("overlays").toArray().isEmpty());

    // Text-only videos sit on the brand colour.
    QJsonObject card = QJsonDocument::fromJson(R"({"main": [], "texts": [{"text": "Hello", "at": 0, "duration": 3, "style": "title"}]})").object();
    applyBrand("make an intro card", kit, QJsonObject{}, base, index, &card);
    CHECK(card.value("background") == "#FF5A1F");
}

// Stands in for ComfyUI / Kokoro / ACE-Step: writes a placeholder file, or
// holds until cancelled.
class FakeBackend : public GeneratorBackend
{
public:
    using GeneratorBackend::GeneratorBackend;
    QStringList made;
    bool hold = false;
    QString failWith;

    void generate(const GenRequest &r, const QString &out) override
    {
        made << r.label();
        emit progress(0.5, "halfway");
        if (hold) return;
        if (!failWith.isEmpty()) {
            emit finished({false, false, {}, failWith});
            return;
        }
        QFile f(out);
        f.open(QIODevice::WriteOnly);
        f.write("x");
        emit finished({true, false, out, {}});
    }
    void cancel() override
    {
        if (hold) emit finished({false, true, {}, "Cancelled"});
    }
};

static void generationTests()
{
    // The plan field.
    const QJsonObject schema = planSchema();
    const QJsonObject gen = schema.value("properties").toObject().value("generate").toObject().value("items").toObject();
    CHECK(gen.value("properties").toObject().value("kind").toObject().value("enum").toArray().size() == 3);
    CHECK(gen.value("properties").toObject().value("voice").toObject().value("enum").toArray().contains("bm_george"));

    QStringList warnings;
    const QList<GenRequest> reqs = generateRequests(QJsonDocument::fromJson(R"({"generate": [
        {"kind": "image", "prompt": "candles on a wooden table"},
        {"kind": "voice", "text": "Light up your evenings", "voice": "british man", "as": "vo"},
        {"kind": "music", "prompt": "soft guitar", "seconds": 400, "as": "m2"},
        {"kind": "video", "prompt": "x"},
        {"kind": "voiceover"}]})").object(), &warnings);
    CHECK(reqs.size() == 3 && warnings.size() == 2);
    CHECK(reqs[0].as == "g1" && reqs[1].as == "vo" && reqs[2].as == "g3");  // "m2" would clash with media ids
    CHECK(reqs[1].kind == "voiceover" && reqs[1].voice == "bm_george" && reqs[2].seconds == 180);
    CHECK(voiceFor("a deep male voice") == "am_michael" && voiceFor("af_bella") == "af_bella" && voiceFor("hello").isEmpty());

    // File names are stable, describe the request and identify generated files.
    const QString name = reqs[1].fileName();
    CHECK(name.startsWith("voiceover-light-up-your-evenings-") && name.endsWith(".wav") && name == reqs[1].fileName());
    GenRequest other = reqs[1];
    other.voice = "af_heart";
    CHECK(other.fileName() != name);
    CHECK(generatedKind(name) == "voiceover" && generatedKind(reqs[0].fileName()) == "image" && generatedKind("song.mp3").isEmpty());
    Material vo;
    vo.kind = MaterialKind::Audio;
    vo.name = name;
    Material narration = vo;
    narration.name = "Narration take 2.wav";
    Material song = vo;
    song.name = "song.mp3";
    CHECK(isVoiceover(vo) && isVoiceover(narration) && !isVoiceover(song));

    // Binding: g-names become media ids, voiceovers play in full, failed items drop out.
    Draft d = sampleDraft();  // m1 beach, m2 city, m3 song, m4 logo
    auto addMaterial = [&](const QString &file, MaterialKind kind, double secs) {
        Material m;
        m.id = Draft::newId();
        m.kind = kind;
        m.path = "/gen/" + file;
        m.name = file;
        m.durationUs = qint64(secs * 1e6);
        m.hasAudio = kind == MaterialKind::Audio;
        m.hasVideo = kind == MaterialKind::Image;
        m.width = kind == MaterialKind::Image ? 1344 : 0;
        m.height = kind == MaterialKind::Image ? 768 : 0;
        d.materials.append(m);
        return MediaIndex(d).alias(m.id);
    };
    const QString img = addMaterial(reqs[0].fileName(), MaterialKind::Image, 0);
    const QString voice = addMaterial(name, MaterialKind::Audio, 1.9);
    CHECK(img == "m5" && voice == "m6");
    QJsonObject bound = QJsonDocument::fromJson(R"({"generate": [{"kind": "image", "prompt": "candles", "as": "g1"},
        {"kind": "voiceover", "text": "Light up", "as": "g2"}, {"kind": "music", "prompt": "soft", "as": "g3"}],
        "main": [{"media": "g1", "from": 0, "to": 10}],
        "audio": [{"media": "G2", "at": 0.5, "from": 0, "to": 3, "volume": 1}, {"media": "g3", "at": 0, "from": 0, "to": 10, "volume": 0.3}]})").object();
    bindGenerated({{"g1", img}, {"g2", voice}}, d, &bound);  // the music failed
    CHECK(!bound.contains("generate"));
    CHECK(bound.value("main").toArray()[0].toObject().value("media") == "m5");
    const QJsonArray boundAudio = bound.value("audio").toArray();
    CHECK(boundAudio.size() == 1 && boundAudio[0].toObject().value("media") == "m6" && boundAudio[0].toObject().value("to").toDouble() == 1.9);

    // A name the model never listed takes the unused made file of the right kind.
    QJsonObject stray = QJsonDocument::fromJson(R"({"generate": [{"kind": "voiceover", "text": "Hi", "as": "g1"}],
        "audio": [{"media": "g2", "at": 5, "from": 0, "to": 5, "volume": 1}], "main": [{"media": "g3", "from": 0, "to": 2}]})").object();
    bindGenerated({{"g1", voice}}, d, &stray);
    CHECK(stray.value("audio").toArray()[0].toObject().value("media") == "m6" && stray.value("audio").toArray()[0].toObject().value("at") == 5);
    CHECK(stray.value("main").toArray().isEmpty());
    // Names used in each other's places swap: the voiceover's name in "main", the image's in "audio".
    QJsonObject swapped = QJsonDocument::fromJson(R"({"main": [{"media": "g1", "from": 0, "to": 5}],
        "audio": [{"media": "g2", "at": 0, "from": 0, "to": 2, "volume": 1}]})").object();
    bindGenerated({{"g1", voice}, {"g2", img}}, d, &swapped);
    CHECK(swapped.value("main").toArray()[0].toObject().value("media") == "m5" && swapped.value("audio").toArray()[0].toObject().value("media") == "m6");

    // The runner with fake backends: progress, files on disk, reuse, failures.
    QTemporaryDir dir;
    GeneratorConfig config = GeneratorConfig::load();
    config.outputDir = dir.path();
    config.freeMemory = false;
    Generators gens(config, nullptr);
    auto *image = new FakeBackend;
    auto *speech = new FakeBackend;
    auto *music = new FakeBackend;
    gens.setBackend("image", image);
    gens.setBackend("voiceover", speech);
    gens.setBackend("music", music);
    QList<double> progress;
    QObject::connect(&gens, &Generators::itemProgress, [&](int, double f, const QString &) { progress << f; });
    QList<GenResult> results;
    bool cancelled = true;
    QObject::connect(&gens, &Generators::finished, [&](const QList<GenResult> &r, bool c) {
        results = r;
        cancelled = c;
    });
    music->failWith = "ACE-Step crashed";
    gens.run(reqs);
    CHECK(waitFor(&gens, &Generators::finished));
    CHECK(!cancelled && results.size() == 3 && results[0].ok && results[1].ok && !results[2].ok);
    CHECK(results[2].error == "ACE-Step crashed" && progress.size() == 3);
    CHECK(QFileInfo(results[1].path).fileName() == name && QFileInfo::exists(results[1].path));
    gens.run(reqs.mid(0, 2));  // same requests: the files are reused, nothing runs again
    CHECK(waitFor(&gens, &Generators::finished));
    CHECK(image->made.size() == 1 && speech->made.size() == 1 && results[0].ok && results[1].ok);

    // Cancel stops the running item and skips the rest.
    GenRequest slow = reqs[0];
    slow.prompt = "a slow one";
    image->hold = true;
    gens.run({slow, reqs[2]});
    QTimer::singleShot(50, &gens, [&] { gens.cancel(); });
    CHECK(waitFor(&gens, &Generators::finished));
    CHECK(cancelled && results[0].cancelled && results[1].cancelled && music->made.size() == 1);
    image->hold = false;
    music->failWith.clear();

    // Guards: the candle-shop request with a model that forgot most of it.
    const QString candle = "make a 10 second ad for my candle shop with a cozy background image, a voiceover saying "
                           "'Light up your evenings' and soft music";
    Draft empty;
    const MediaIndex none(empty);
    QJsonObject plan = QJsonDocument::fromJson(R"({"reply": "Made it.", "aspect": "keep", "style": "elegant", "main": [], "audio": [],
        "texts": [{"text": "Cozy Candles", "at": 0, "duration": 3, "style": "headline"}, {"text": "Shop now", "at": 7, "duration": 3, "style": "cta"}],
        "generate": [{"kind": "voiceover", "text": "Light up your evening!", "as": "g1"}]})").object();
    const QStringList notes = requestedGeneration(candle, QJsonObject{}, empty, none, &plan);
    const QList<GenRequest> wanted = generateRequests(plan);
    CHECK(wanted.size() == 3 && notes.size() == 3);
    CHECK(wanted[0].kind == "voiceover" && wanted[0].text == "Light up your evenings");  // the user's words, not the model's
    CHECK(wanted[1].kind == "image" && wanted[1].prompt.startsWith("cozy candle shop") && wanted[1].as == "g2");
    CHECK(wanted[2].kind == "music" && wanted[2].seconds == 10 && wanted[2].prompt.contains("acoustic guitar"));
    CHECK(voiceoverScript(candle) == "Light up your evenings");
    // No list, but references like the prompt's example: the added items take those names.
    QJsonObject refs = QJsonDocument::fromJson(R"({"main": [{"media": "g1", "from": 0, "to": 10}],
        "audio": [{"media": "g3", "at": 0, "from": 0, "to": 10, "volume": 0.5}, {"media": "g2", "at": 0.5, "from": 0, "to": 3, "volume": 1}]})").object();
    requestedGeneration(candle, QJsonObject{}, empty, none, &refs);
    const QList<GenRequest> byRef = generateRequests(refs);
    CHECK(byRef.size() == 3 && byRef[0].kind == "image" && byRef[0].as == "g1" && byRef[1].kind == "voiceover" && byRef[1].as == "g2"
          && byRef[2].kind == "music" && byRef[2].as == "g3");
    // Only the music is referenced: the voiceover gets a fresh name instead of the music's slot.
    QJsonObject musicOnly = QJsonDocument::fromJson(R"({"main": [{"media": "g1", "from": 0, "to": 10}],
        "audio": [{"media": "g3", "at": 0, "from": 0, "to": 10, "volume": 0.5}]})").object();
    requestedGeneration(candle, QJsonObject{}, empty, none, &musicOnly);
    const QList<GenRequest> one = generateRequests(musicOnly);
    CHECK(one.size() == 3 && one[1].kind == "voiceover" && one[1].as == "g2" && one[2].kind == "music" && one[2].as == "g3");

    // Resolution with the fakes, then placement, ducking and the build: a working timeline.
    Draft project;
    auto import = [&](const QStringList &paths) -> const Draft & {
        for (const QString &p : paths) {
            if (std::any_of(project.materials.cbegin(), project.materials.cend(), [&](const Material &m) { return m.path == p; })) continue;
            Material m;
            m.id = Draft::newId();
            m.path = p;
            m.name = QFileInfo(p).fileName();
            const QString kind = generatedKind(m.name);
            m.kind = kind == "image" ? MaterialKind::Image : MaterialKind::Audio;
            m.durationUs = kind == "voiceover" ? 1'850'000 : kind == "music" ? 10'000'000 : 0;
            m.hasAudio = m.kind == MaterialKind::Audio;
            m.hasVideo = !m.hasAudio;
            m.width = m.hasVideo ? 1344 : 0;
            m.height = m.hasVideo ? 768 : 0;
            project.materials.append(m);
        }
        return project;
    };
    GenResolution res;
    bool resolved = false;
    gens.resolve(plan, project, import, [&](const GenResolution &r) {
        res = r;
        resolved = true;
    });
    QEventLoop loop;
    QTimer::singleShot(3000, &loop, &QEventLoop::quit);
    QObject::connect(&gens, &Generators::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
    if (!resolved) loop.exec();
    CHECK(resolved && !res.cancelled && res.errors.isEmpty() && res.generated == QStringList({"m1", "m2", "m3"}));
    CHECK(project.materials.size() == 3 && image->made.last().contains("cozy candle shop"));
    // Items named like media ("as": "m1") before anything is imported keep their references.
    QJsonObject named = QJsonDocument::fromJson(R"({"generate": [{"kind": "voiceover", "text": "Light up your evenings", "as": "m1"}],
        "audio": [{"media": "m1", "at": 2, "from": 0, "to": 9, "volume": 1}]})").object();
    Draft fresh;
    GenResolution res2;
    gens.resolve(named, fresh, [&](const QStringList &paths) -> const Draft & {
        fresh.materials.append(project.materials[0]);  // the cached voiceover, now m1
        fresh.materials[0].path = paths.value(0);
        return fresh;
    }, [&](const GenResolution &r) { res2 = r; });
    waitFor(&gens, &Generators::finished, 3000);
    QCoreApplication::processEvents();
    CHECK(res2.generated == QStringList({"m1"}) && res2.plan.value("audio").toArray()[0].toObject().value("at") == 2);
    QJsonObject built = res.plan;
    const MediaIndex pindex(project);
    const QStringList placed = placeGenerated(res.generated, candle, project, pindex, &built);
    CHECK(placed.size() == 3);
    CHECK(duckUnderVoiceover(candle, project, pindex, &built));
    CHECK(!duckUnderVoiceover("make the music louder", project, pindex, &built));
    QJsonObject quiet = built;  // a model that put the new voiceover at 0.5
    QJsonArray qa = quiet.value("audio").toArray();
    for (qsizetype i = 0; i < qa.size(); ++i) {
        QJsonObject o = qa[i].toObject();
        if (o.value("volume").toDouble() != 1) continue;
        o.insert("volume", 0.5);
        o.insert("to", 9);
        qa[i] = o;
    }
    quiet.insert("audio", qa);
    placeGenerated(res.generated, candle, project, pindex, &quiet);
    bool loud = false;
    for (const QJsonValue &v : quiet.value("audio").toArray())
        loud |= v.toObject().value("volume").toDouble() == 1 && v.toObject().value("to").toDouble() == 1.85;
    CHECK(loud);
    PlanReport report;
    const Draft ad = draftFromPlan(project, built, pindex, &report);
    CHECK(report.ok() && std::abs(ad.duration() / 1e6 - 10) < 0.01);
    double musicVol = -1, musicDuck = 0, voiceVol = -1, voiceStart = -1, voiceLen = 0;
    bool voiceIsSpeech = false;
    for (const Track &t : ad.tracks)
        for (const Segment &s : t.segments) {
            if (t.role != TrackRole::Audio) continue;
            const Material *m = ad.material(s.materialId);
            if (isVoiceover(*m)) voiceVol = s.volume, voiceStart = s.start / 1e6, voiceLen = s.duration / 1e6, voiceIsSpeech = s.speech;
            else musicVol = s.volume, musicDuck = s.duck;
        }
    // Music keeps its level between phrases and dips to about 0.25 under the voice.
    CHECK(std::abs(musicVol * musicDuck - 0.25) < 0.02 && voiceIsSpeech);
    CHECK(voiceVol == 1 && voiceStart == 0.5 && std::abs(voiceLen - 1.85) < 0.01);
    CHECK(ad.material(ad.tracks[0].segments[0].materialId)->kind == MaterialKind::Image);

    // Not asked for: the model's extra generation is dropped; the user's own song is used instead of composing.
    const Draft base = sampleDraft();
    const MediaIndex bindex(base);
    QJsonObject extra = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 5}],
        "generate": [{"kind": "music", "prompt": "upbeat", "seconds": 5}, {"kind": "image", "prompt": "a beach"},
                     {"kind": "voiceover", "text": "Summer"}]})").object();
    requestedGeneration("make a 5 second video with music and the title \"Summer\"", QJsonObject{}, base, bindex, &extra);
    CHECK(!extra.contains("generate"));
    QJsonObject compose = QJsonObject{{"main", QJsonArray{QJsonObject{{"media", "m1"}, {"from", 0}, {"to", 5}}}}};
    requestedGeneration("compose some new upbeat music for it", QJsonObject{}, base, bindex, &compose);
    CHECK(generateRequests(compose).size() == 1 && generateRequests(compose)[0].seconds == 5);
    // A voiceover with no quote reads the on-screen words.
    QJsonObject read = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 5}],
        "texts": [{"text": "Summer Sale", "at": 0, "duration": 2, "style": "headline"}, {"text": "Shop now", "at": 2, "duration": 3, "style": "cta"}]})").object();
    requestedGeneration("add a female voiceover", QJsonObject{}, base, bindex, &read);
    CHECK(generateRequests(read).size() == 1 && generateRequests(read)[0].text == "Summer Sale. Shop now." && generateRequests(read)[0].voice == "af_heart");
    // Removing isn't asking.
    QJsonObject removing = read;
    requestedGeneration("remove the voiceover", QJsonObject{}, base, bindex, &removing);
    CHECK(!removing.contains("generate"));

    // Ads: a song file still goes under a voiceover; without any music, the app offers to compose some.
    Draft withVoice = sampleDraft();
    Material spoken = vo;
    spoken.id = "v";
    spoken.path = "/gen/" + name;
    spoken.durationUs = 1'900'000;
    spoken.hasAudio = true;
    withVoice.materials.append(spoken);  // m5
    const MediaIndex vindex(withVoice);
    QJsonObject vad = QJsonDocument::fromJson(R"({"style": "energetic", "main": [{"media": "m1", "from": 0, "to": 6}],
        "audio": [{"media": "m5", "at": 0.5, "from": 0, "to": 1.9, "volume": 1}]})").object();
    CHECK(addAdMusic("make an ad with a voiceover", QJsonObject{}, withVoice, vindex, &vad));
    CHECK(vad.value("audio").toArray().size() == 2 && vad.value("audio").toArray()[0].toObject().value("media") == "m3");
    CHECK(duckUnderVoiceover("make an ad with a voiceover", withVoice, vindex, &vad));
    CHECK(vad.value("audio").toArray()[0].toObject().value("duck").toDouble() > 0.2);
    CHECK(vad.value("audio").toArray()[1].toObject().value("speech").toBool());
    CHECK(musicOffer("make a 6 second ad", QJsonObject{{"style", "bold"}}, empty, none).contains("a bold track"));
    CHECK(musicOffer("make a 6 second ad", QJsonObject{{"style", "bold"}}, base, bindex).isEmpty());  // song.mp3 is there
    CHECK(musicOffer("make a silent 6 second ad", QJsonObject{{"style", "bold"}}, empty, none).isEmpty());
}

static void smartTests()
{
    const Draft base = sampleDraft();  // m1 beach 12s, m2 city 8s, m3 song 95s, m4 logo (photo)
    const MediaIndex index(base);
    const QJsonObject two = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 5}, {"media": "m4", "from": 0, "to": 3}],
        "audio": [{"media": "m3", "at": 0, "from": 0, "to": 8, "volume": 0.8}]})").object();

    QJsonObject p = two;
    CHECK(applySmartRequests("cut it to the beat of the song", base, index, &p).size() == 1 && p.value("beat_sync") == "beats");
    p = two;
    applySmartRequests("sync the cuts to the music, every bar", base, index, &p);
    CHECK(p.value("beat_sync") == "bars");
    p = two;
    p.insert("beat_sync", "beats");
    applySmartRequests("stop syncing to the beat", base, index, &p);
    CHECK(p.value("beat_sync") == "off");

    p = two;
    applySmartRequests("add captions", base, index, &p);
    CHECK(p.value("captions") == "caption");
    p = two;
    p.insert("style", "energetic");
    applySmartRequests("subtitle it for tiktok", base, index, &p);
    CHECK(p.value("captions") == "kinetic-caption");
    p = two;
    CHECK(applySmartRequests("add a caption that says \"Fresh\"", base, index, &p).isEmpty() && !p.contains("captions"));
    p = two;
    p.insert("captions", "caption");
    applySmartRequests("remove the captions", base, index, &p);
    CHECK(p.value("captions") == "none");

    p = two;
    applySmartRequests("lower the music when she talks", base, index, &p);
    CHECK(p.value("audio").toArray()[0].toObject().value("duck").toDouble() == 0.3);
    CHECK(p.value("main").toArray()[0].toObject().value("speech").toBool());  // the talking is in the clip

    p = two;
    CHECK(applySmartRequests("remove the background from the photo", base, index, &p).size() == 1);
    CHECK(p.value("main").toArray()[1].toObject().value("effects").toArray()[0].toObject().value("name") == "cutout");
    CHECK(!p.value("main").toArray()[0].toObject().contains("effects"));

    // Round trip: beat sync, captions, speech and duck survive; auto captions stay out of "texts".
    QJsonObject full = two;
    full.insert("beat_sync", "bars");
    full.insert("captions", "kinetic-caption");
    QJsonArray audio = full.value("audio").toArray();
    QJsonObject song = audio[0].toObject();
    song.insert("duck", 0.3);
    audio[0] = song;
    full.insert("audio", audio);
    PlanReport r;
    Draft d = draftFromPlan(base, full, index, &r);
    CHECK(r.ok() && d.beatSync == "bars" && d.captions == "kinetic-caption" && needsFinishing(d));
    d.ensureTextMaterial();
    Segment auto_;
    auto_.id = "c1";
    auto_.materialId = kTextMaterialId;
    auto_.origin = "captions";
    auto_.text.text = "said";
    auto_.duration = 1'000'000;
    d.tracks.append(Track{"t", TrackRole::Overlay, {auto_}});
    const QJsonObject back = planFromDraft(d, MediaIndex(d));
    CHECK(back.value("beat_sync") == "bars" && back.value("captions") == "kinetic-caption");
    CHECK(back.value("texts").toArray().isEmpty());
    CHECK(back.value("audio").toArray()[0].toObject().value("duck").toDouble() == 0.3);
    const Draft reloaded = Draft::fromJson(d.toJson());
    CHECK(reloaded.beatSync == "bars" && reloaded.captions == "kinetic-caption" && reloaded.segment("c1")->origin == "captions");
    full.insert("captions", "none");
    CHECK(draftFromPlan(d, full, MediaIndex(d), &r).captions.isEmpty());

    // A styled edit with music cuts on the music by default.
    QJsonObject styled = two;
    styled.insert("style", "elegant");
    applyAdStyle(QJsonObject{}, base, index, &styled);
    CHECK(styled.value("beat_sync") == "bars");

    // Kinetic captions reveal across the spoken words.
    const QList<QList<Word>> chunks = captionChunks({{0, 300'000, "Fresh"}, {300'000, 600'000, "bread"}, {600'000, 900'000, "every"},
                                                     {900'000, 1'300'000, "morning."}, {1'500'000, 1'800'000, "Order"}});
    CHECK(chunks.size() == 2 && chunks[0].size() == 4 && chunks[1].first().text == "Order");
}

static void sceneTests()
{
    Draft d = sampleDraft();  // m1 beach 12s, m2 city 8s, m3 song, m4 logo
    d.materials[0].path = "/nowhere/beach.mp4";
    const MediaIndex index(d);
    auto shot = [](double a, double b, const char *what, double sharp, double steady, QStringList subjects = {}) {
        Shot s;
        s.start = qint64(a * 1e6);
        s.end = qint64(b * 1e6);
        s.description = what;
        s.sharpness = sharp;
        s.stability = steady;
        s.exposure = 1;
        s.subjects = subjects;
        s.framing = "wide";
        for (double t = a; t < b - 1e-9; t += 0.2) s.quality << float(sharp * steady * (t > a + 1 && t < a + 2.6 ? 1.0 : 0.6));
        return s;
    };
    SceneIndex x;
    x.shots = {shot(0, 4, "waves breaking on a sandy beach", 0.9, 0.9), shot(4, 8, "dog running along the shore", 0.8, 0.85, {"dog"}),
               shot(8, 12, "blurry close-up of sand", 0.1, 0.9)};
    x.described = true;
    putSceneIndex(d.materials[0], x);

    const QString media = describeMedia(d, index);
    CHECK(media.contains("shots: 1) 0-4s waves breaking on a sandy beach (wide, best)"));
    CHECK(media.contains("3) 8-12s blurry close-up of sand (wide, blurry)"));

    // "shot" and "moment" become exact ranges inside the shot, keeping the length.
    QJsonObject plan = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "shot": 2, "from": 0, "to": 2},
        {"media": "m1", "moment": "the dog on the beach", "from": 0, "to": 3}]})").object();
    CHECK(resolveShots(d, index, &plan).size() == 2);
    const QJsonArray m = plan.value("main").toArray();
    CHECK(m[0].toObject().value("from").toDouble() >= 4 && m[0].toObject().value("to").toDouble() <= 8);
    CHECK(std::abs(m[0].toObject().value("to").toDouble() - m[0].toObject().value("from").toDouble() - 2) < 0.01);
    CHECK(m[1].toObject().value("from").toDouble() >= 4 && !m[1].toObject().contains("moment"));

    // "The best parts" avoid the blurry shot, and a file used twice gets two different shots.
    QJsonObject best = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 8, "to": 10}, {"media": "m1", "from": 9, "to": 11}]})").object();
    CHECK(!pickBestMoments("use the best bits of the beach clip", d, index, &best).isEmpty());
    const QJsonArray b = best.value("main").toArray();
    CHECK(b[0].toObject().value("to").toDouble() <= 8.01 && b[1].toObject().value("to").toDouble() <= 8.01);
    CHECK(std::abs(b[0].toObject().value("from").toDouble() - b[1].toObject().value("from").toDouble()) >= 2);
    CHECK(pickBestMoments("make it vertical", d, index, &best).isEmpty());

    // Content the request names but the plan skips.
    const QJsonObject wavesOnly = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 3}]})").object();
    const QStringList miss = missingShots("make a video with the dog", wavesOnly, d, index);
    CHECK(miss.size() == 1 && miss[0].contains("\"shot\": 2"));
    // Partial words don't count ("sand" isn't "sandy"), and a plan using an equally good match is fine.
    CHECK(missingShots("show the sand", wavesOnly, d, index).size() == 1);  // "sand" is in shot 3's description
    CHECK(missingShots("show the sandwich", wavesOnly, d, index).isEmpty());
    CHECK(missingShots("make a vertical video with music", wavesOnly, d, index).isEmpty());

    // A clearly named shot replaces the weaker one the model chose, keeping its length.
    QJsonObject swap = wavesOnly;
    CHECK(applyNamedShots("start with the dog running along the shore", d, index, &swap).size() == 1);
    CHECK(swap.value("main").toArray()[0].toObject().value("from").toDouble() >= 4);
    CHECK(std::abs(swap.value("main").toArray()[0].toObject().value("to").toDouble() - swap.value("main").toArray()[0].toObject().value("from").toDouble() - 3) < 0.01);
    QJsonObject vague = wavesOnly;
    CHECK(applyNamedShots("make it nicer", d, index, &vague).isEmpty());

    // Matching words to shots.
    const QList<ShotMatch> dogs = findShots(x, "dogs running");
    CHECK(!dogs.isEmpty() && dogs.first().shot == 1);
    CHECK(x.shots[0].bestWindow(1'000'000).first >= 1'000'000);  // inside the best-quality stretch
}

static void textPlanTests()
{
    Draft d = sampleDraft();  // m1 beach 12s with sound
    d.materials[0].path = "/nowhere/interview.mp4";
    const MediaIndex index(d);
    Transcript t;
    auto add = [&](double a, double b, const char *w) { t.words << Word{qint64(a * 1e6), qint64(b * 1e6), w}; };
    add(0.2, 0.5, "Hi,"); add(0.6, 0.9, "I'm"); add(1.0, 1.4, "Sam.");
    add(2.0, 2.3, "Um,"); add(2.5, 2.8, "our"); add(2.9, 3.2, "bread"); add(3.3, 3.6, "is"); add(3.7, 4.2, "fresh.");
    add(6.0, 6.3, "Order"); add(6.4, 6.8, "online"); add(6.9, 7.5, "today.");
    putTranscript(d.materials[0], t);

    CHECK(describeSpeech(d.materials[0]).contains("3) 6-7.50s \"Order online today.\""));
    CHECK(describeMedia(d, index).contains("says: 1) 0.20-1.40s \"Hi, I'm Sam.\""));

    // "line" and "says" become exact ranges; "cut_lines" becomes skips.
    QJsonObject plan = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "line": 3, "from": 0, "to": 1},
        {"media": "m1", "says": "our bread is fresh", "from": 0, "to": 1}, {"media": "m1", "from": 0, "to": 12, "cut_lines": [1]}]})").object();
    CHECK(resolveLines(d, index, &plan).size() == 3);
    const QJsonArray m = plan.value("main").toArray();
    CHECK(std::abs(m[0].toObject().value("from").toDouble() - 5.94) < 0.01 && std::abs(m[0].toObject().value("to").toDouble() - 7.65) < 0.01);
    CHECK(m[1].toObject().value("from").toDouble() < 2.1 && m[1].toObject().value("to").toDouble() > 4.2);
    CHECK(m[2].toObject().value("skips").toArray().size() == 1 && std::abs(itemSeconds(m[2].toObject()) - (12 - 1.41)) < 0.05);

    // Skips build jump-cut pieces and fold back into one item.
    PlanReport r;
    const Draft built = draftFromPlan(d, plan, index, &r);
    CHECK(r.ok() && built.tracks[0].segments.size() == 4);  // two lines, then the clip in two pieces
    const QJsonObject back = planFromDraft(built, MediaIndex(built));
    CHECK(back.value("main").toArray().size() == 3 && back.value("main").toArray()[2].toObject().value("skips").toArray().size() == 1);
    CHECK(std::abs(built.duration() / 1e6 - (1.71 + itemSeconds(m[1].toObject()) + 12 - 1.41)) < 0.06);

    // Requests: clean speech, and cutting a line by what it says.
    QJsonObject whole = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 12}]})").object();
    CHECK(!applySpeechRequests("remove the ums and tighten the pauses", d, index, &whole).isEmpty() && whole.value("clean_speech").toBool());
    QJsonObject noName = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 12}]})").object();
    CHECK(!applySpeechRequests("cut the part where I say my name", d, index, &noName).isEmpty());
    CHECK(noName.value("main").toArray()[0].toObject().value("skips").toArray().size() == 1);
    QJsonObject bread = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 12}]})").object();
    applySpeechRequests("remove the line about bread", d, index, &bread);
    CHECK(bread.value("main").toArray()[0].toObject().value("skips").toArray()[0].toArray()[0].toDouble() < 2.6);

    // "Remove the ums" never removes lines; an arrangement of named lines is built as asked.
    QJsonObject cutByMistake = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 12, "cut_lines": [2]}]})").object();
    CHECK(arrangeLines("remove the ums", d, index, &cutByMistake).contains("kept every line"));
    CHECK(!cutByMistake.value("main").toArray()[0].toObject().contains("cut_lines"));
    QJsonObject order = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 5}]})").object();
    CHECK(arrangeLines("start with the line about ordering online, then the bread line", d, index, &order).size() == 1);
    const QJsonArray arranged = order.value("main").toArray();
    CHECK(arranged.size() == 2 && arranged[0].toObject().value("from").toDouble() > 5.9 && arranged[1].toObject().value("from").toDouble() < 2.1);

    // Named lines the plan skipped.
    const QJsonObject first = QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 1.5}]})").object();
    const QStringList miss = missingLines("start with the line about ordering online", first, d, index);
    CHECK(miss.size() == 1 && miss[0].contains("\"line\": 3"));
    CHECK(missingLines("make it vertical", first, d, index).isEmpty());
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    planTests();
    animationTests();
    guardTests();
    styleTests();
    brandTests();
    smartTests();
    sceneTests();
    textPlanTests();
    generationTests();
    anthropicTests();
    openAiTests();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "all ai tests passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
