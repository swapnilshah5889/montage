// Editing-model tests: drives Project the same way the QML UI does.
// Usage: project_test <media dir containing clipA.mp4 clipB.mov music.mp3 photo.png>

#include "app/Project.h"
#include "engine/Beats.h"
#include "engine/MediaProbe.h"
#include "engine/Speech.h"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>
#include <cstdio>

using namespace mtg;

static int failures = 0;
#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

static QString materialId(const Project &p, const QString &file)
{
    for (const Material &m : p.draft().materials)
        if (m.path.endsWith(file)) return m.id;
    return {};
}

static QList<qint64> starts(const Track &t)
{
    QList<qint64> out;
    for (const Segment &s : t.segments) out << s.start;
    return out;
}

// Beat sync, ducking and captions on real audio (beat.mp3 is a 120 BPM loop,
// voiceover.wav says "Fresh bread every morning. Order yours today.").
static void smartTests(const QDir &media)
{
    Material beat;
    QString err;
    if (!probeMedia(media.filePath("beat.mp3"), QDir::tempPath(), &beat, &err)) {
        std::fprintf(stderr, "skipping smart tests: %s (run tests/make_media.sh)\n", qPrintable(err));
        return;
    }
    const BeatGrid grid = beatGrid(beat);
    CHECK(std::abs(grid.bpm - 120) < 1.5);
    bool onHalfSeconds = grid.beats.size() > 40;
    for (double b : grid.beats.mid(1)) onHalfSeconds &= std::abs(b * 2 - std::round(b * 2)) < 0.04;
    CHECK(onHalfSeconds);

    // Cuts at 2.8 s and 5.9 s land on the beats at 3 s and 6 s; a text on a cut follows it.
    Draft d;
    beat.id = "beat";
    d.materials.append(beat);
    Material clip;
    probeMedia(media.filePath("beach.mp4"), QDir::tempPath(), &clip, &err);
    clip.id = "clip";
    d.materials.append(clip);
    for (qint64 len : {2'800'000LL, 3'100'000LL, 4'100'000LL}) {
        Segment s;
        s.id = Draft::newId();
        s.materialId = "clip";
        s.duration = len;
        d.mainTrack().segments.append(s);
    }
    d.ensureTextMaterial();
    Track texts{Draft::newId(), TrackRole::Overlay, {}};
    Segment cta;
    cta.id = "cta";
    cta.materialId = kTextMaterialId;
    cta.start = 5'900'000;
    cta.duration = 4'100'000;
    texts.segments.append(cta);
    d.tracks.append(texts);
    Track music{Draft::newId(), TrackRole::Audio, {}};
    Segment song;
    song.id = "song";
    song.materialId = "beat";
    song.duration = 10'000'000;
    music.segments.append(song);
    d.tracks.append(music);
    d.normalize();
    CHECK(musicSegment(d) == "song");
    CHECK(snapCutsToBeats(d, "song", "beats") == 2);
    const QList<Segment> &main = d.mainTrack().segments;
    CHECK(std::abs(main[1].start - 3'000'000) < 60'000 && std::abs(main[2].start - 6'000'000) < 60'000);
    CHECK(main.last().end() == 10'000'000);
    CHECK(std::abs(d.segment("cta")->start - 6'000'000) < 60'000);

    // Ducking: the voice is talking at about 1-3.5 s on the timeline.
    Material voice;
    if (!probeMedia(media.filePath("voiceover.wav"), QDir::tempPath(), &voice, &err)) return;
    voice.id = "voice";
    d.materials.append(voice);
    Track vo{Draft::newId(), TrackRole::Audio, {}};
    Segment talk;
    talk.id = "talk";
    talk.materialId = "voice";
    talk.start = 1'000'000;
    talk.duration = voice.durationUs;
    talk.speech = true;
    vo.segments.append(talk);
    d.tracks.append(vo);
    const auto spans = speechSpans(d);
    CHECK(!spans.isEmpty() && spans.first().first < 1'400'000 && spans.last().second > 3'000'000);
    CHECK(duckAmount(spans, 2'000'000) > 0.99 && duckAmount(spans, 8'000'000) == 0);
    CHECK(musicSegment(d) == "song");  // the voice isn't music

    // Captions (needs whisper.cpp and a model; skipped without them).
    if (!whisperProblem().isEmpty()) {
        std::fprintf(stderr, "skipping caption test: %s\n", qPrintable(whisperProblem()));
        return;
    }
    QString captionError;
    const int n = addCaptions(d, "kinetic-caption", &captionError);
    CHECK(n >= 2);
    QString said;
    qint64 first = -1;
    for (const Track &t : d.tracks)
        for (const Segment &s : t.segments)
            if (s.origin == "captions") {
                said += s.text.text + ' ';
                if (first < 0) first = s.start;
            }
    CHECK(said.contains("bread", Qt::CaseInsensitive) && said.contains("today", Qt::CaseInsensitive));
    CHECK(first >= 1'000'000 && first < 1'600'000);
    CHECK(addCaptions(d, "caption") == n);  // again: replaced, not added
    CHECK(removeCaptions(d) == n);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const QDir media(argc > 1 ? argv[1] : ".");
    Project p(&app);

    const int n = p.importFiles({QUrl::fromLocalFile(media.filePath("clipA.mp4")),
                                 QUrl::fromLocalFile(media.filePath("clipB.mov")),
                                 QUrl::fromLocalFile(media.filePath("music.mp3")),
                                 QUrl::fromLocalFile(media.filePath("photo.png"))});
    CHECK(n == 4);
    CHECK(p.importFiles({QUrl::fromLocalFile(media.filePath("clipA.mp4"))}) == 0);  // no duplicates

    const QString a = materialId(p, "clipA.mp4"), b = materialId(p, "clipB.mov");
    const QString music = materialId(p, "music.mp3"), photo = materialId(p, "photo.png");

    // Main track is magnetic.
    p.addToTimeline(a);
    const QString segB = p.addToTimeline(b);
    CHECK(p.draft().tracks[0].segments.size() == 2);
    CHECK(p.duration() == 7'000'000);
    CHECK((starts(p.draft().tracks[0]) == QList<qint64>{0, 4'000'000}));
    CHECK(p.canvasWidth() == 1920 && p.canvasHeight() == 1080);

    // Split with nothing selected cuts the main-track clip under the playhead.
    p.setSelectedId({});
    CHECK(p.splitAt(1'500'000));
    const Track &main = p.draft().tracks[0];
    CHECK(main.segments.size() == 3);
    CHECK(main.segments[1].start == 1'500'000 && main.segments[1].sourceStart == 1'500'000);
    CHECK(main.segments[1].duration == 2'500'000);
    CHECK(p.duration() == 7'000'000);

    // Undo / redo
    p.undo();
    CHECK(p.draft().tracks[0].segments.size() == 2);
    p.redo();
    CHECK(p.draft().tracks[0].segments.size() == 3);

    // Moving clipB to the front reorders and repacks the main track.
    p.moveSegment(segB, p.draft().tracks[0].id, 0);
    CHECK(p.draft().tracks[0].segments.first().id == segB);
    CHECK((starts(p.draft().tracks[0]) == QList<qint64>{0, 3'000'000, 4'500'000}));

    // Right-edge trim can't run past the end of the source.
    p.trimSegment(segB, 0, 10'000'000);
    CHECK(p.draft().segment(segB)->duration == 3'000'000);
    p.trimSegment(segB, 0, 2'000'000);
    CHECK(p.draft().segment(segB)->duration == 2'000'000);
    CHECK(p.duration() == 6'000'000);  // later clips ripple left

    // Left-edge trim moves the source in-point.
    const QString right = p.draft().tracks[0].segments[2].id;  // second half of clipA
    const Segment before = *p.draft().segment(right);
    p.trimSegment(right, before.start + 500'000, before.duration - 500'000);
    CHECK(p.draft().segment(right)->sourceStart == before.sourceStart + 500'000);
    CHECK(p.draft().segment(right)->duration == before.duration - 500'000);

    // Overlays: a clash on the same track spills onto a new track.
    const QString ov1 = p.addToTimeline(photo, "new-overlay", 1'000'000);
    const QString ov2 = p.addToTimeline(photo, "new-overlay", 8'000'000);
    int overlays = 0;
    for (const Track &t : p.draft().tracks) overlays += t.role == TrackRole::Overlay;
    CHECK(overlays == 2);
    const Track *ov1Track = nullptr;
    p.draft().segment(ov1, &ov1Track);
    p.moveSegment(ov2, ov1Track->id, 2'000'000);  // overlaps ov1 (1s..6s)
    overlays = 0;
    for (const Track &t : p.draft().tracks) overlays += t.role == TrackRole::Overlay;
    CHECK(overlays == 2);
    const Track *ov2Track = nullptr;
    p.draft().segment(ov2, &ov2Track);
    CHECK(ov2Track && ov2Track->id != ov1Track->id);
    p.moveSegment(ov2, ov1Track->id, 6'000'000);  // fits right after ov1
    p.draft().segment(ov2, &ov2Track);
    p.draft().segment(ov1, &ov1Track);
    CHECK(ov2Track && ov1Track && ov2Track->id == ov1Track->id);
    overlays = 0;
    for (const Track &t : p.draft().tracks) overlays += t.role == TrackRole::Overlay;
    CHECK(overlays == 1);  // the emptied track was removed

    // Audio can't be dragged onto the main track; it stays on an audio track.
    const QString mus = p.addToTimeline(music, {}, 0);
    p.moveSegment(mus, p.draft().tracks[0].id, 500'000);
    const Track *musTrack = nullptr;
    const Segment *musSeg = p.draft().segment(mus, &musTrack);
    CHECK(musTrack && musTrack->role == TrackRole::Audio && musSeg->start == 500'000);

    // A slider drag is many live updates but one undo step.
    p.setSegmentProperty(ov1, "opacity", 0.8, false);
    p.setSegmentProperty(ov1, "opacity", 0.5, false);
    p.setSegmentProperty(ov1, "opacity", 0.3, true);
    CHECK(qFuzzyCompare(p.draft().segment(ov1)->opacity, 0.3));
    p.undo();
    CHECK(qFuzzyCompare(p.draft().segment(ov1)->opacity, 1.0));
    p.redo();

    // "Original" follows the first video, not a photo/logo at the start.
    {
        Project q(&app);
        q.importFiles({QUrl::fromLocalFile(media.filePath("photo.png")), QUrl::fromLocalFile(media.filePath("clipB.mov"))});
        q.addToTimeline(materialId(q, "photo.png"));
        CHECK(q.canvasWidth() >= 1080 && q.canvasHeight() >= 1080);  // small photo alone: at least 1080 on the short side
        q.addToTimeline(materialId(q, "clipB.mov"));
        CHECK(q.canvasWidth() == 1920 && q.canvasHeight() == 1080);  // 720p video, scaled to 1080 on the short side
    }

    // Speed: faster is shorter and plays the same footage; freeze frames go in at the playhead.
    {
        Project q(&app);
        q.importFiles({QUrl::fromLocalFile(media.filePath("clipA.mp4"))});
        const QString seg = q.addToTimeline(materialId(q, "clipA.mp4"));
        q.setSpeed(seg, 2.0);
        CHECK(q.draft().segment(seg)->duration == 2'000'000 && q.draft().segment(seg)->sourceSpan() == 4'000'000);
        q.setSpeed(seg, 0.5, "hero");
        CHECK(q.draft().segment(seg)->duration == 8'000'000 && q.draft().segment(seg)->curve == "hero");
        q.setSpeed(seg, 1.0);
        CHECK(q.freezeFrame(1'000'000, 1.5));
        const QList<Segment> &mainSegs = q.draft().tracks[0].segments;
        CHECK(mainSegs.size() == 3 && mainSegs[1].speed == 0 && mainSegs[1].duration == 1'500'000);
        CHECK(mainSegs[1].sourceStart == 1'000'000 && mainSegs[2].sourceStart == 1'000'000 && q.duration() == 5'500'000);

        // Splitting a keyframed clip keeps each half's animation.
        Project k(&app);
        k.importFiles({QUrl::fromLocalFile(media.filePath("clipA.mp4"))});
        const QString ks = k.addToTimeline(materialId(k, "clipA.mp4"));
        k.addKeyframe(ks, 0);
        k.setSegmentProperty(ks, "scale", 2.0, true, 4'000'000);  // keyframed scale 1 -> 2
        k.setSelectedId(ks);
        CHECK(k.splitAt(2'000'000));
        const Segment &l = k.draft().tracks[0].segments[0], &r = k.draft().tracks[0].segments[1];
        CHECK(std::abs(l.keys.value("scale").last().v - 1.5) < 0.01 && std::abs(r.keys.value("scale").first().v - 1.5) < 0.01);
        CHECK(r.keys.value("scale").last().t == 2'000'000 && std::abs(r.keys.value("scale").last().v - 2.0) < 1e-9);
    }

    // Fixed aspect ratio
    p.setAspect("9:16");
    CHECK(p.canvasWidth() == 1080 && p.canvasHeight() == 1920 && p.aspect() == "9:16");

    // Save / load round trip
    QTemporaryDir dir;
    const QUrl file = QUrl::fromLocalFile(dir.filePath("roundtrip.montage"));
    CHECK(p.save(file));
    const QJsonObject saved = p.draft().toJson();
    Project q(&app);
    CHECK(q.load(file));
    CHECK(q.draft().toJson() == saved);
    CHECK(!q.dirty() && !q.canUndo());

    // Text clips: overlay placement, presets, split keeps the style, JSON round trip.
    const QString title = p.addText("Hello\nworld", 500'000, 0, "caption");
    const Track *titleTrack = nullptr;
    const Segment *titleSeg = p.draft().segment(title, &titleTrack);
    CHECK(titleSeg && titleTrack->role == TrackRole::Overlay);
    CHECK(titleSeg->duration == kTextDefaultUs && !titleSeg->text.background.isEmpty());
    CHECK(p.materials().size() == 4);  // the built-in text material stays out of the media bin
    p.setTextProperty(title, "color", "#FFD60A");
    p.applyTextPreset(title, "title");
    CHECK(p.draft().segment(title)->text.text == "Hello\nworld");
    CHECK(p.draft().segment(title)->text.background.isEmpty());
    CHECK(p.draft().segment(title)->text.color == "#ffffff");
    p.setSelectedId(title);
    CHECK(p.splitAt(2'000'000));
    CHECK(p.draft().segment(p.selectedId())->text.text == "Hello\nworld");
    p.moveSegment(title, p.draft().tracks[0].id, 0);  // text can't go on the main track
    p.draft().segment(title, &titleTrack);
    CHECK(titleTrack->role == TrackRole::Overlay);
    CHECK(Draft::fromJson(p.draft().toJson()).toJson() == p.draft().toJson());

    // An undo group turns many edits into one step.
    const QJsonObject beforeGroup = p.draft().toJson();
    p.beginUndoGroup();
    p.addText("one", 0);
    p.addText("two", 4'000'000);
    p.setAspect("1:1");
    p.endUndoGroup();
    p.undo();
    CHECK(p.draft().toJson() == beforeGroup);

    // Delete
    p.setSelectedId(segB);
    p.deleteSelected();
    CHECK(!p.draft().segment(segB));
    CHECK(p.selectedId().isEmpty());

    smartTests(media);

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "all project tests passed", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
