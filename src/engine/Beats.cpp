#include "Beats.h"

#include "AudioDecoder.h"
#include "FFmpeg.h"

#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

namespace mtg {

namespace {

constexpr int kRate = 12000;     // analysis sample rate (48 kHz / 4)
constexpr int kFft = 1024;       // ~85 ms window
constexpr int kHop = 128;        // ~10.7 ms per onset frame
constexpr double kFps = double(kRate) / kHop;
constexpr double kMaxSeconds = 600;

QMutex g_mutex;
QHash<QString, BeatGrid> g_grids;

void fft(std::vector<std::complex<float>> &a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * std::numbers::pi / double(len);
        const std::complex<float> wl(float(std::cos(ang)), float(std::sin(ang)));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Mono samples at kRate.
std::vector<float> decodeMono(const Material &m)
{
    std::vector<float> out;
    AudioDecoder dec;
    if (!dec.open(m.path)) return out;
    const double secs = std::min(kMaxSeconds, m.durationUs > 0 ? m.durationUs / 1e6 : kMaxSeconds);
    const qint64 total = qint64(secs * kSampleRate);
    constexpr int kChunk = kSampleRate * 4;
    std::vector<float> buf(size_t(kChunk) * 2);
    out.reserve(size_t(total / 4) + 1);
    for (qint64 start = 0; start < total; start += kChunk) {
        const int frames = int(std::min<qint64>(kChunk, total - start));
        std::fill(buf.begin(), buf.end(), 0.0f);
        dec.mixInto(start, frames, buf.data(), 1.0f);
        for (int i = 0; i + 3 < frames; i += 4) {  // average 4 stereo frames -> 1 mono sample
            float s = 0;
            for (int k = 0; k < 4; ++k) s += buf[size_t(i + k) * 2] + buf[size_t(i + k) * 2 + 1];
            out.push_back(s / 8);
        }
    }
    return out;
}

// Normalises an envelope: remove the local mean (about a second), keep the
// rises, scale to unit standard deviation.
std::vector<double> normalised(const std::vector<double> &env)
{
    const size_t n = env.size();
    std::vector<double> out(n, 0);
    const int half = int(kFps / 2);
    double sum = 0;
    int count = 0, lo = 0, hi = -1;
    for (size_t i = 0; i < n; ++i) {
        while (hi < int(std::min(n - 1, i + half))) sum += env[size_t(++hi)], ++count;
        while (lo < int(i) - half) sum -= env[size_t(lo++)], --count;
        out[i] = std::max(0.0, env[i] - sum / std::max(1, count));
    }
    double mean = 0, var = 0;
    for (double v : out) mean += v;
    mean /= std::max<size_t>(1, n);
    for (double v : out) var += (v - mean) * (v - mean);
    const double sd = std::sqrt(var / std::max<size_t>(1, n));
    if (sd > 1e-9)
        for (double &v : out) v /= sd;
    return out;
}

BeatGrid analyse(const Material &m)
{
    BeatGrid grid;
    const std::vector<float> x = decodeMono(m);
    if (x.size() < size_t(kRate) * 3) return grid;  // under three seconds: nothing to track

    // Spectral flux on log magnitudes; a second, low-band flux finds kicks.
    std::vector<float> window(kFft);
    for (int i = 0; i < kFft; ++i) window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / (kFft - 1)));
    const size_t frames = (x.size() - kFft) / kHop;
    const int bins = kFft / 2, lowBins = int(200.0 * kFft / kRate) + 1;
    std::vector<double> flux(frames, 0), low(frames, 0);
    std::vector<float> prev(size_t(bins), 0.0f), cur(size_t(bins), 0.0f);
    std::vector<std::complex<float>> buf(kFft);
    for (size_t f = 0; f < frames; ++f) {
        for (int i = 0; i < kFft; ++i) buf[size_t(i)] = x[f * kHop + size_t(i)] * window[size_t(i)];
        fft(buf);
        double s = 0, sl = 0;
        for (int k = 1; k < bins; ++k) {
            cur[size_t(k)] = std::log1p(100.0f * std::abs(buf[size_t(k)]));
            const double d = std::max(0.0f, cur[size_t(k)] - prev[size_t(k)]);
            s += d;
            if (k < lowBins) sl += d;
        }
        flux[f] = f ? s : 0;
        low[f] = f ? sl : 0;
        std::swap(prev, cur);
    }
    // Beats live on the kick and snare; broadband flux alone locks onto
    // off-beat hi-hats, so the low band gets extra weight.
    const std::vector<double> kick = normalised(low);
    std::vector<double> onset = normalised(flux);
    for (size_t i = 0; i < onset.size(); ++i) onset[i] = 0.5 * onset[i] + kick[i];

    // Tempo: autocorrelation of the onset envelope between 60 and 200 BPM,
    // weighted towards 120 BPM (a log-Gaussian, about an octave wide).
    const int minLag = int(kFps * 60 / 200), maxLag = int(kFps * 60 / 60);
    double bestScore = -1;
    int bestLag = 0;
    std::vector<double> ac(size_t(maxLag) + 2, 0);
    for (int lag = minLag; lag <= maxLag + 1 && size_t(lag) < frames; ++lag) {
        double s = 0;
        for (size_t i = size_t(lag); i < frames; ++i) s += onset[i] * onset[i - size_t(lag)];
        ac[size_t(lag)] = s / double(frames - size_t(lag));
    }
    for (int lag = minLag + 1; lag <= maxLag; ++lag) {
        const double bpm = 60 * kFps / lag;
        const double w = std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 0.9, 2));
        const double s = ac[size_t(lag)] * w;
        if (s > bestScore) bestScore = s, bestLag = lag;
    }
    if (bestLag <= 0) return grid;
    // Is there a beat at all? A steady pulse repeats strongly at its period;
    // tones, speech and noise don't (normalised autocorrelation stays low).
    double ac0 = 0;
    for (double v : onset) ac0 += v * v;
    ac0 /= double(frames);
    // ...and are there real hits? Sustained tones and pads have almost no
    // spectral flux (a sine averages about 1; drums and plucked strings 40+).
    double fluxMean = 0;
    for (double v : flux) fluxMean += v;
    fluxMean /= double(frames);
    if (qEnvironmentVariableIsSet("MONTAGE_DEBUG"))
        qInfo("beats: flux mean %.2f, periodicity %.3f", fluxMean, ac0 > 0 ? ac[size_t(bestLag)] / ac0 : 0.0);
    if (fluxMean < 5 || ac0 <= 1e-9 || ac[size_t(bestLag)] / ac0 < 0.4) return grid;
    // Parabolic refinement of the peak.
    double period = bestLag;
    {
        const double a = ac[size_t(bestLag - 1)], b = ac[size_t(bestLag)], c = ac[size_t(bestLag + 1)];
        const double den = a - 2 * b + c;
        if (std::abs(den) > 1e-12) period += std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    }

    // Beats: best path through the onsets with spacing close to the period.
    constexpr double kTightness = 100;
    std::vector<double> score(frames);
    std::vector<int> back(frames, -1);
    for (size_t t = 0; t < frames; ++t) {
        const int lo = int(t) - int(std::round(2 * period)), hi = int(t) - int(std::round(period / 2));
        double best = 0;
        int arg = -1;
        for (int tau = std::max(0, lo); tau <= hi; ++tau) {
            const double r = std::log(double(int(t) - tau) / period);
            const double s = score[size_t(tau)] - kTightness * r * r;
            if (arg < 0 || s > best) best = s, arg = tau;
        }
        score[t] = onset[t] + (arg >= 0 ? best : 0);
        back[t] = arg;
    }
    size_t end = frames - 1;
    for (size_t t = frames - size_t(std::max(1.0, period)); t < frames; ++t)
        if (score[t] > score[end]) end = t;
    QList<int> beatFrames;
    for (int t = int(end); t >= 0; t = back[size_t(t)]) beatFrames.prepend(t);
    // Frame -> seconds: the window centre, plus the flux peak's measured lag
    // behind the attack (calibrated on a click track).
    constexpr double kLatency = 0.03;
    auto toSecs = [](int f) { return std::max(0.0, (double(f) * kHop + kFft / 2.0) / kRate + kLatency); };
    for (int f : beatFrames) grid.beats << toSecs(f);
    if (grid.beats.size() < 2) return grid;
    grid.bpm = 60 * kFps / period;

    // Bars: the phase (of 4) whose beats carry the most kick energy.
    auto around = [&](const std::vector<double> &env, int f) {
        double v = 0;
        for (int k = std::max(0, f - 2); k <= std::min(int(frames) - 1, f + 2); ++k) v = std::max(v, env[size_t(k)]);
        return v;
    };
    double phaseScore[4] = {0, 0, 0, 0};
    for (qsizetype i = 0; i < beatFrames.size(); ++i) phaseScore[i % 4] += around(kick, beatFrames[i]) + 0.25 * around(onset, beatFrames[i]);
    const int phase = int(std::max_element(phaseScore, phaseScore + 4) - phaseScore);
    for (qsizetype i = phase; i < beatFrames.size(); i += 4) grid.downbeats << toSecs(beatFrames[i]);

    // Accents: the strongest onsets, a second apart.
    QList<QPair<double, int>> peaks;
    for (size_t t = 1; t + 1 < frames; ++t)
        if (onset[t] > 3.0 && onset[t] >= onset[t - 1] && onset[t] >= onset[t + 1]) peaks.append({onset[t], int(t)});
    std::sort(peaks.begin(), peaks.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    QList<double> chosen;
    for (const auto &p : peaks) {
        const double s = toSecs(p.second);
        bool clear = true;
        for (double c : chosen) clear &= std::abs(c - s) >= 1.0;
        if (clear) chosen << s;
        if (chosen.size() >= 16) break;
    }
    std::sort(chosen.begin(), chosen.end());
    grid.accents = chosen;
    return grid;
}

}  // namespace

BeatGrid beatGrid(const Material &m)
{
    const QString key = m.path + QLatin1Char('|') + QString::number(QFileInfo(m.path).lastModified().toMSecsSinceEpoch());
    {
        QMutexLocker lock(&g_mutex);
        auto it = g_grids.constFind(key);
        if (it != g_grids.cend()) return *it;
    }
    const BeatGrid grid = m.kind == MaterialKind::Image ? BeatGrid{} : analyse(m);
    QMutexLocker lock(&g_mutex);
    g_grids.insert(key, grid);
    return grid;
}

QStringList beatSyncNames()
{
    return {"beats", "bars"};
}

QString musicSegment(const Draft &d)
{
    QString best;
    qint64 longest = 0;
    for (const Track &t : d.tracks) {
        if (t.role != TrackRole::Audio) continue;
        for (const Segment &s : t.segments) {
            const Material *m = d.material(s.materialId);
            if (!m || s.speech || QFileInfo(m->name).completeBaseName().startsWith(QLatin1String("voiceover"), Qt::CaseInsensitive))
                continue;
            if (s.duration > longest) longest = s.duration, best = s.id;
        }
    }
    return best;
}

int snapCutsToBeats(Draft &d, const QString &musicId, const QString &mode)
{
    const Track *owner = nullptr;
    const Segment *music = std::as_const(d).segment(musicId, &owner);
    const Material *mm = music ? d.material(music->materialId) : nullptr;
    QList<Segment> &main = d.mainTrack().segments;
    if (!mm || main.size() < 2) return 0;
    const BeatGrid grid = beatGrid(*mm);
    if (grid.isEmpty()) return 0;

    // Beat times on the timeline.
    const QList<double> &marks = mode == QLatin1String("bars") && grid.downbeats.size() >= 2 ? grid.downbeats : grid.beats;
    QList<qint64> beats;
    const double rate = music->speed > 0 ? music->speed : 1.0;
    for (double b : marks) {
        const qint64 t = music->start + qint64((b * 1e6 - double(music->sourceStart)) / rate);
        if (t > music->start && t < music->end()) beats << t;
    }
    if (beats.size() < 2) return 0;
    const qint64 beatLen = (beats.last() - beats.first()) / std::max<qint64>(1, beats.size() - 1);

    // New cut positions: the nearest mark, keeping clips at least half a
    // second (and a third of a beat gap) long and in order.
    const qint64 total = main.last().end();
    const qint64 minLen = std::max<qint64>(500'000, beatLen / 3);
    QList<qint64> oldCuts, newCuts;
    for (qsizetype i = 1; i < main.size(); ++i) oldCuts << main[i].start;
    qint64 prev = 0;
    // Jump cuts inside one clip (text-based edits: pieces sharing a group) stay where they are.
    auto internal = [&](qsizetype k) { return !main[k + 1].group.isEmpty() && main[k + 1].group == main[k].group; };
    for (qsizetype k = 0; k < oldCuts.size(); ++k) {
        if (internal(k)) {
            newCuts << oldCuts[k];
            prev = oldCuts[k];
            continue;
        }
        qint64 nextFixed = total;
        for (qsizetype j = k + 1; j < oldCuts.size(); ++j)
            if (internal(j)) {
                nextFixed = oldCuts[j];
                break;
            }
        const qint64 remaining = (oldCuts.size() - k) * minLen;  // room for the clips after this cut
        qint64 best = -1;
        for (qint64 b : beats) {
            if (b < prev + minLen || b > total - remaining || b > nextFixed - minLen) continue;
            if (best < 0 || std::abs(b - oldCuts[k]) < std::abs(best - oldCuts[k])) best = b;
        }
        // Don't drag a cut further than a bar from where the editor put it.
        if (best < 0 || std::abs(best - oldCuts[k]) > std::max<qint64>(beatLen * 4, 1'000'000)) best = oldCuts[k];
        newCuts << best;
        prev = best;
    }

    int moved = 0;
    QList<qint64> starts{0};
    starts += newCuts;
    for (qsizetype i = 0; i < main.size(); ++i) {
        Segment &s = main[i];
        const qint64 end = i + 1 < main.size() ? newCuts[i] : total;
        const qint64 dur = end - starts[i];
        if (dur == s.duration && starts[i] == s.start) continue;
        if (i + 1 < main.size() && newCuts[i] != oldCuts[i]) ++moved;
        s.start = starts[i];
        s.duration = dur;
        // Keep the source range inside the file.
        if (const Material *m = d.material(s.materialId); m && m->isTimed() && s.speed > 0)
            s.sourceStart = std::clamp<qint64>(s.sourceStart, 0, std::max<qint64>(0, m->durationUs - s.sourceSpan()));
    }
    if (!moved) return 0;

    // Things on other tracks that started or ended on a cut follow it.
    auto shiftFor = [&](qint64 t) -> qint64 {
        for (qsizetype k = 0; k < oldCuts.size(); ++k)
            if (std::abs(t - oldCuts[k]) <= 150'000) return newCuts[k] - oldCuts[k];
        return 0;
    };
    for (qsizetype ti = 1; ti < d.tracks.size(); ++ti) {
        Track &t = d.tracks[ti];
        if (t.role == TrackRole::Audio) continue;
        for (Segment &s : t.segments) {
            const qint64 startShift = shiftFor(s.start), endShift = shiftFor(s.end());
            if (startShift) {
                s.start += startShift;
                s.duration = std::max<qint64>(100'000, s.duration - startShift + endShift);
            } else if (endShift) {
                s.duration = std::max<qint64>(100'000, s.duration + endShift);
            }
        }
        std::sort(t.segments.begin(), t.segments.end(), [](const Segment &a, const Segment &b) { return a.start < b.start; });
        for (qsizetype i = 1; i < t.segments.size(); ++i)  // no overlaps after moving
            if (t.segments[i].start < t.segments[i - 1].end())
                t.segments[i - 1].duration = std::max<qint64>(100'000, t.segments[i].start - t.segments[i - 1].start);
    }
    d.normalize();
    return moved;
}

}  // namespace mtg
