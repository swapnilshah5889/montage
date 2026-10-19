// Editing-model tests: drives Project the same way the QML UI does.
// Usage: project_test <media dir containing clipA.mp4 clipB.mov music.mp3 photo.png>

#include "app/Project.h"

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

    // Delete
    p.setSelectedId(segB);
    p.deleteSelected();
    CHECK(!p.draft().segment(segB));
    CHECK(p.selectedId().isEmpty());

    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "all project tests passed", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
