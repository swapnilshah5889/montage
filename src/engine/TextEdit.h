#pragma once

// Text-based editing: cutting video by what's said. Words come from the
// transcript (Speech.h); cut points are snapped into the nearest silence so
// edits don't clip syllables. A clip with words taken out becomes jump-cut
// pieces that share a group, so they stay one unit for transitions, beat sync
// and the assistant's plans.

#include "model/Draft.h"
#include "Speech.h"

#include <QList>
#include <QPair>

namespace mtg {

using Range = QPair<qint64, qint64>;  // [first, second) in microseconds

struct SpeechCutOptions {
    bool fillers = true;              // um, uh, erm...
    bool pauses = true;               // silences longer than maxPause
    qint64 maxPause = 700'000;
    qint64 keepPause = 300'000;       // what a long pause is shortened to
};

// Source ranges of [from, to) in a file to cut for fillers and long pauses.
// `fillers`/`pauses` (optional) count what was found.
QList<Range> speechCuts(const Material &m, const Transcript &t, qint64 from, qint64 to, const SpeechCutOptions &o,
                        int *fillers = nullptr, int *pauses = nullptr);

// Cuts source ranges out of a segment. On the main track the rest of the
// timeline moves up (texts, overlays and effects after a cut move with it);
// on other tracks the pieces close up in place. Returns the timeline time
// removed.
qint64 cutSegment(Draft &d, const QString &segmentId, QList<Range> sourceRanges);

// A segment with source ranges `skips` taken out, as back-to-back jump-cut
// pieces from its start (no ripple). For building timelines from plans.
QList<Segment> splitBySkips(const Segment &s, QList<Range> skips);

// Removes [t0, t1) of the main track's timeline. Returns the time removed.
qint64 cutTimeline(Draft &d, qint64 t0, qint64 t1);

// Cuts fillers and long pauses from every clip with a transcript (main track
// and voiceovers). Returns the time removed; counts what was cut.
qint64 cleanSpeech(Draft &d, const SpeechCutOptions &o, int *fillers = nullptr, int *pauses = nullptr);

// Words on the timeline: each kept word of the main track's clips with its
// timeline span. For the transcript panel and the assistant.
struct TimelineWord {
    QString text;
    qint64 start = 0, end = 0;  // timeline
    QString segmentId;
    bool filler = false;
    qint64 pauseAfter = 0;      // silence before the next word, timeline
};
QList<TimelineWord> timelineWords(const Draft &d);

}  // namespace mtg
