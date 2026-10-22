#include "TextEdit.h"

#include <algorithm>
#include <cmath>

namespace mtg {

namespace {

constexpr qint64 kFrame = 10'000;  // loudness resolution

struct Quiet {
    QList<float> env;
    float threshold = 0.004f;

    explicit Quiet(const Material &m)
        : env(loudness(m))
    {
        if (env.isEmpty()) return;
        QList<float> sorted = env;
        std::sort(sorted.begin(), sorted.end());
        const float loud = sorted[qsizetype(double(sorted.size() - 1) * 0.9)];
        threshold = std::max(0.004f, loud * 0.08f);
    }
    bool quiet(qint64 t) const
    {
        const qsizetype i = qsizetype(t / kFrame);
        return env.isEmpty() || i < 0 || i >= env.size() || env[i] < threshold;
    }
    // The nearest quiet moment at or before `t`, looking back at most `window`.
    qint64 quietBefore(qint64 t, qint64 window) const
    {
        for (qint64 x = t; x >= t - window && x >= 0; x -= kFrame)
            if (quiet(x)) return x;
        return t;
    }
    // Where the sound before `t` ends: scan back from `t` while it's quiet.
    qint64 soundEndsBefore(qint64 t, qint64 limit) const
    {
        qint64 x = t;
        while (x - kFrame > limit && quiet(x - kFrame)) x -= kFrame;
        return x;
    }
};

QList<Range> merged(QList<Range> r)
{
    std::sort(r.begin(), r.end());
    QList<Range> out;
    for (const Range &x : r) {
        if (x.second - x.first < 40'000) continue;
        if (!out.isEmpty() && x.first <= out.last().second) out.last().second = std::max(out.last().second, x.second);
        else out << x;
    }
    return out;
}

}  // namespace

QList<Range> speechCuts(const Material &m, const Transcript &t, qint64 from, qint64 to, const SpeechCutOptions &o, int *fillers, int *pauses)
{
    QList<Word> words;
    for (const Word &w : t.words)
        if (w.start >= from - 20'000 && w.start < to) words << w;
    if (words.isEmpty()) return {};
    const Quiet q(m);
    QList<Range> cuts;

    if (o.fillers) {
        for (qsizetype i = 0; i < words.size(); ++i) {
            if (!isFiller(words[i].text)) continue;
            const qint64 next = i + 1 < words.size() ? words[i + 1].start : std::min(to, words[i].end + 250'000);
            const qint64 a = std::max(from, q.quietBefore(words[i].start, 120'000));
            const qint64 b = std::min(to, q.quietBefore(next, 120'000) - 30'000);
            if (b - a < 60'000) continue;
            cuts << Range{a, b};
            if (fillers) ++*fillers;
        }
    }
    if (o.pauses) {
        // Silence between words (and before the first / after the last), longer than allowed.
        auto pause = [&](qint64 silenceStart, qint64 silenceEnd) {
            if (silenceEnd - silenceStart <= o.maxPause) return;
            cuts << Range{silenceStart + o.keepPause / 2, silenceEnd - o.keepPause / 2};
            if (pauses) ++*pauses;
        };
        pause(from, std::max(from, q.quietBefore(words.first().start, 120'000)));
        for (qsizetype i = 0; i + 1 < words.size(); ++i) {
            if (o.fillers && isFiller(words[i + 1].text)) continue;  // the filler's cut covers it
            const qint64 next = words[i + 1].start;
            const qint64 silenceStart = q.soundEndsBefore(next - 20'000, words[i].start + 80'000);
            pause(silenceStart, next - 30'000);
        }
        const qint64 lastEnd = q.soundEndsBefore(to, words.last().start + 80'000);
        pause(lastEnd, to);
    }
    QList<Range> out;
    for (const Range &r : merged(cuts)) out << Range{std::max(r.first, from), std::min(r.second, to)};
    return merged(out);
}

qint64 cutSegment(Draft &d, const QString &segmentId, QList<Range> sourceRanges)
{
    Track *owner = nullptr;
    Segment *found = d.segment(segmentId, &owner);
    if (!found || found->speed <= 0 || !found->curve.isEmpty()) return 0;
    const Segment seg = *found;
    const qint64 src0 = seg.sourceStart, src1 = seg.sourceStart + seg.sourceSpan();
    for (Range &r : sourceRanges) r = Range{std::max(r.first, src0), std::min(r.second, src1)};
    sourceRanges = merged(sourceRanges);
    if (sourceRanges.isEmpty()) return 0;

    // What's kept, as pieces of the clip.
    QList<Range> keep;
    qint64 cursor = src0;
    for (const Range &r : sourceRanges) {
        if (r.first - cursor >= 40'000) keep << Range{cursor, r.first};
        cursor = r.second;
    }
    if (src1 - cursor >= 40'000) keep << Range{cursor, src1};

    const QString group = seg.group.isEmpty() ? Draft::newId() : seg.group;
    QList<Segment> pieces;
    for (qsizetype k = 0; k < keep.size(); ++k) {
        Segment p = seg;
        if (k > 0) p.id = Draft::newId();
        p.group = group;
        p.sourceStart = keep[k].first;
        p.duration = std::max<qint64>(1, qint64(double(keep[k].second - keep[k].first) / seg.speed));
        if (k > 0) {
            p.transition = Transition{};
            p.motion.in.clear();
            p.keys.clear();
        }
        if (k + 1 < keep.size()) p.motion.out.clear();
        pieces << p;
    }

    // Where each cut sits on the original timeline, and how long it was.
    QList<Range> removed;  // (timeline position, duration)
    qint64 total = 0;
    for (const Range &r : sourceRanges) {
        const qint64 pos = seg.start + qint64(double(r.first - src0) / seg.speed);
        const qint64 len = qint64(double(r.second - r.first) / seg.speed);
        removed << Range{pos, len};
        total += len;
    }

    QList<Segment> &list = owner->segments;
    qsizetype at = 0;
    while (at < list.size() && list[at].id != segmentId) ++at;
    list.removeAt(at);
    qint64 start = seg.start;
    for (qsizetype k = 0; k < pieces.size(); ++k) {
        pieces[k].start = start;
        start += pieces[k].duration;
        list.insert(at + k, pieces[k]);
    }

    // Ripple: things after each cut move up by its length.
    auto shift = [&](Segment &s) {
        qint64 delta = 0;
        for (const Range &r : removed) {
            if (s.start >= r.first + r.second) delta += r.second;
            else if (s.start > r.first) delta += s.start - r.first;  // started inside the cut: to its start
        }
        s.start -= delta;
    };
    if (owner->role == TrackRole::Main) {
        for (Track &t : d.tracks) {
            if (&t == owner || t.role == TrackRole::Audio) continue;
            for (Segment &s : t.segments) shift(s);
        }
    } else {
        for (Segment &s : list)
            if (s.group != group && s.start >= seg.end()) shift(s);
    }
    d.normalize();
    return total;
}

QList<Segment> splitBySkips(const Segment &seg, QList<Range> skips)
{
    if (seg.speed <= 0 || !seg.curve.isEmpty()) return {seg};
    const qint64 src0 = seg.sourceStart, src1 = seg.sourceStart + seg.sourceSpan();
    for (Range &r : skips) r = Range{std::max(r.first, src0), std::min(r.second, src1)};
    skips = merged(skips);
    if (skips.isEmpty()) return {seg};
    QList<Range> keep;
    qint64 cursor = src0;
    for (const Range &r : skips) {
        if (r.first - cursor >= 40'000) keep << Range{cursor, r.first};
        cursor = r.second;
    }
    if (src1 - cursor >= 40'000) keep << Range{cursor, src1};
    const QString group = seg.group.isEmpty() ? Draft::newId() : seg.group;
    QList<Segment> out;
    qint64 start = seg.start;
    for (qsizetype k = 0; k < keep.size(); ++k) {
        Segment p = seg;
        if (k > 0) {
            p.id = Draft::newId();
            p.transition = Transition{};
            p.motion.in.clear();
            p.keys.clear();
        }
        if (k + 1 < keep.size()) p.motion.out.clear();
        p.group = group;
        p.sourceStart = keep[k].first;
        p.duration = std::max<qint64>(1, qint64(double(keep[k].second - keep[k].first) / seg.speed));
        p.start = start;
        start += p.duration;
        out << p;
    }
    return out;
}

qint64 cutTimeline(Draft &d, qint64 t0, qint64 t1)
{
    if (t1 <= t0) return 0;
    qint64 total = 0;
    // Last clip first, so earlier positions stay valid.
    const QList<Segment> main = d.mainTrack().segments;
    for (auto it = main.crbegin(); it != main.crend(); ++it) {
        const Segment &s = *it;
        const qint64 a = std::max(t0, s.start), b = std::min(t1, s.end());
        if (b <= a) continue;
        if (a <= s.start && b >= s.end()) {  // all of it
            const qint64 len = s.duration;
            QList<Segment> &list = d.mainTrack().segments;
            list.removeIf([&](const Segment &x) { return x.id == s.id; });
            for (Track &t : d.tracks) {
                if (t.role != TrackRole::Overlay) continue;
                for (Segment &o : t.segments) {
                    if (o.start >= s.end()) o.start -= len;
                    else if (o.start > s.start) o.start = s.start;
                }
            }
            d.normalize();
            total += len;
            continue;
        }
        total += cutSegment(d, s.id, {Range{s.sourceAt(a - s.start), s.sourceAt(b - s.start)}});
    }
    return total;
}

qint64 cleanSpeech(Draft &d, const SpeechCutOptions &o, int *fillers, int *pauses)
{
    qint64 total = 0;
    QList<QString> ids;
    for (const Track &t : d.tracks)
        for (const Segment &s : t.segments)
            if (t.role == TrackRole::Main || (t.role == TrackRole::Audio && s.speech)) ids << s.id;
    // Last first: cuts move what comes after.
    for (auto it = ids.crbegin(); it != ids.crend(); ++it) {
        const Segment *s = std::as_const(d).segment(*it);
        const Material *m = s ? d.material(s->materialId) : nullptr;
        if (!m || !m->hasAudio || s->speed != 1.0) continue;
        const Transcript t = cachedFileTranscript(*m);
        if (!t.done || t.words.isEmpty()) continue;
        const QList<Range> cuts = speechCuts(*m, t, s->sourceStart, s->sourceStart + s->sourceSpan(), o, fillers, pauses);
        if (!cuts.isEmpty()) total += cutSegment(d, *it, cuts);
    }
    return total;
}

QList<TimelineWord> timelineWords(const Draft &d)
{
    QList<TimelineWord> out;
    for (const Segment &s : d.tracks.isEmpty() ? QList<Segment>{} : d.tracks.first().segments) {
        const Material *m = d.material(s.materialId);
        if (!m || !m->hasAudio || s.speed <= 0) continue;
        const Transcript t = cachedFileTranscript(*m);
        if (!t.done) continue;
        const qint64 a = s.sourceStart, b = s.sourceStart + s.sourceSpan();
        for (const Word &w : t.words) {
            if (w.start < a - 20'000 || w.start >= b) continue;
            TimelineWord tw;
            tw.text = w.text;
            tw.start = s.start + qint64(double(std::max(w.start, a) - a) / s.speed);
            tw.end = s.start + qint64(double(std::min(std::max(w.end, w.start + 80'000), b) - a) / s.speed);
            tw.segmentId = s.id;
            tw.filler = isFiller(w.text);
            out << tw;
        }
    }
    for (qsizetype i = 0; i + 1 < out.size(); ++i) out[i].pauseAfter = std::max<qint64>(0, out[i + 1].start - out[i].end);
    return out;
}

}  // namespace mtg
