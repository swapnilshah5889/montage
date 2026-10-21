#include "Speech.h"

#include "AudioDecoder.h"
#include "FFmpeg.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <vector>

namespace mtg {

namespace {

QMutex g_mutex;
QHash<QString, QList<QPair<qint64, qint64>>> g_spans;
QHash<QString, Transcript> g_transcripts;

QString fileKey(const Material &m)
{
    return m.path + QLatin1Char('|') + QString::number(QFileInfo(m.path).lastModified().toMSecsSinceEpoch());
}

// Mono samples of [from, to) at 48 kHz / `decimate`.
std::vector<float> decodeMono(const Material &m, int decimate, qint64 from, qint64 to)
{
    std::vector<float> out;
    AudioDecoder dec;
    if (!dec.open(m.path)) return out;
    const qint64 first = from * kSampleRate / 1'000'000, last = to * kSampleRate / 1'000'000;
    constexpr int kChunk = kSampleRate * 4;
    std::vector<float> buf(size_t(kChunk) * 2);
    out.reserve(size_t((last - first) / decimate) + 1);
    for (qint64 start = first; start < last; start += kChunk) {
        const int frames = int(std::min<qint64>(kChunk, last - start));
        std::fill(buf.begin(), buf.end(), 0.0f);
        dec.mixInto(start, frames, buf.data(), 1.0f);
        for (int i = 0; i + decimate - 1 < frames; i += decimate) {
            float s = 0;
            for (int k = 0; k < decimate; ++k) s += buf[size_t(i + k) * 2] + buf[size_t(i + k) * 2 + 1];
            out.push_back(s / float(2 * decimate));
        }
    }
    return out;
}

QList<QPair<qint64, qint64>> merged(QList<QPair<qint64, qint64>> spans, qint64 gap)
{
    std::sort(spans.begin(), spans.end());
    QList<QPair<qint64, qint64>> out;
    for (const auto &s : spans) {
        if (!out.isEmpty() && s.first <= out.last().second + gap) out.last().second = std::max(out.last().second, s.second);
        else out << s;
    }
    return out;
}

QString whisperBinary()
{
    if (const QString env = qEnvironmentVariable("MONTAGE_WHISPER"); !env.isEmpty()) return env;
    QString bin = QStandardPaths::findExecutable(QStringLiteral("whisper-cli"));
    if (bin.isEmpty())  // apps started from the Dock don't get Homebrew's PATH
        bin = QStandardPaths::findExecutable(QStringLiteral("whisper-cli"), {QStringLiteral("/opt/homebrew/bin"), QStringLiteral("/usr/local/bin")});
    return bin;
}

QString whisperModel()
{
    if (const QString env = qEnvironmentVariable("MONTAGE_WHISPER_MODEL"); !env.isEmpty()) return env;
    const QDir dir(QDir::homePath() + QStringLiteral("/ai-studio/models/whisper"));
    for (const char *name : {"ggml-small.bin", "ggml-base.bin", "ggml-small.en.bin", "ggml-base.en.bin"})
        if (dir.exists(QLatin1String(name))) return dir.filePath(QLatin1String(name));
    const QStringList any = dir.entryList({QStringLiteral("ggml-*.bin")}, QDir::Files, QDir::Size);
    return any.isEmpty() ? QString() : dir.filePath(any.first());
}

bool writeWav16k(const std::vector<float> &x, const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const quint32 dataBytes = quint32(x.size() * 2);
    auto u32 = [&](quint32 v) { f.write(reinterpret_cast<const char *>(&v), 4); };
    auto u16 = [&](quint16 v) { f.write(reinterpret_cast<const char *>(&v), 2); };
    f.write("RIFF");
    u32(36 + dataBytes);
    f.write("WAVEfmt ");
    u32(16);
    u16(1);  // PCM
    u16(1);  // mono
    u32(16000);
    u32(32000);
    u16(2);
    u16(16);
    f.write("data");
    u32(dataBytes);
    std::vector<qint16> pcm(x.size());
    for (size_t i = 0; i < x.size(); ++i) pcm[i] = qint16(std::clamp(x[i], -1.0f, 1.0f) * 32767);
    f.write(reinterpret_cast<const char *>(pcm.data()), qint64(pcm.size() * 2));
    return true;
}

QString cachePath(const QString &key)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/transcripts");
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex()) + QStringLiteral(".json");
}

QJsonObject toJson(const Transcript &t)
{
    QJsonArray words;
    for (const Word &w : t.words) words.append(QJsonArray{double(w.start), double(w.end), w.text});
    return {{"language", t.language}, {"words", words}};
}

Transcript fromJson(const QJsonObject &o)
{
    Transcript t;
    t.language = o.value("language").toString();
    for (const QJsonValue &v : o.value("words").toArray()) {
        const QJsonArray a = v.toArray();
        t.words.append({qint64(a.at(0).toDouble()), qint64(a.at(1).toDouble()), a.at(2).toString()});
    }
    return t;
}

}  // namespace

QList<QPair<qint64, qint64>> audibleSpans(const Material &m)
{
    const QString key = fileKey(m);
    {
        QMutexLocker lock(&g_mutex);
        if (auto it = g_spans.constFind(key); it != g_spans.cend()) return *it;
    }
    QList<QPair<qint64, qint64>> spans;
    if (m.hasAudio && m.durationUs > 0) {
        constexpr int kDecimate = 4;  // 12 kHz
        constexpr int kWin = 240;     // 20 ms
        const std::vector<float> x = decodeMono(m, kDecimate, 0, std::min<qint64>(m.durationUs, 1'800'000'000));
        std::vector<float> rms;
        for (size_t i = 0; i + kWin <= x.size(); i += kWin) {
            double s = 0;
            for (int k = 0; k < kWin; ++k) s += double(x[i + size_t(k)]) * x[i + size_t(k)];
            rms.push_back(float(std::sqrt(s / kWin)));
        }
        if (!rms.empty()) {
            std::vector<float> sorted = rms;
            std::sort(sorted.begin(), sorted.end());
            const float loud = sorted[size_t(double(sorted.size() - 1) * 0.95)];
            const float threshold = std::max(0.004f, loud * 0.12f);
            qint64 openAt = -1;
            for (size_t i = 0; i <= rms.size(); ++i) {
                const bool on = i < rms.size() && rms[i] > threshold;
                const qint64 t = qint64(i) * 20'000;
                if (on && openAt < 0) openAt = t;
                if (!on && openAt >= 0) {
                    spans << qMakePair(openAt, t);
                    openAt = -1;
                }
            }
            spans = merged(spans, 350'000);
            spans.removeIf([](const QPair<qint64, qint64> &s) { return s.second - s.first < 150'000; });
        }
    }
    QMutexLocker lock(&g_mutex);
    g_spans.insert(key, spans);
    return spans;
}

QList<QPair<qint64, qint64>> speechSpans(const Draft &d)
{
    QList<QPair<qint64, qint64>> out;
    for (const Track &t : d.tracks)
        for (const Segment &s : t.segments) {
            if (!s.speech || s.volume <= 0 || s.speed <= 0) continue;
            const Material *m = d.material(s.materialId);
            if (!m || !m->hasAudio) continue;
            for (const auto &span : audibleSpans(*m)) {
                const qint64 a = s.start + qint64(double(span.first - s.sourceStart) / s.speed);
                const qint64 b = s.start + qint64(double(span.second - s.sourceStart) / s.speed);
                if (b <= s.start || a >= s.end()) continue;
                out << qMakePair(std::max(a, s.start), std::min(b, s.end()));
            }
        }
    return merged(out, 0);
}

double duckAmount(const QList<QPair<qint64, qint64>> &spans, qint64 t)
{
    constexpr double kAttack = 150'000, kRelease = 500'000;
    double a = 0;
    for (const auto &s : spans) {
        if (s.first - kAttack > t) break;  // sorted: the rest start later
        double v = 0;
        if (t >= s.first && t <= s.second) v = 1;
        else if (t < s.first) v = 1 - double(s.first - t) / kAttack;
        else v = 1 - double(t - s.second) / kRelease;
        a = std::max(a, v);
        if (a >= 1) break;
    }
    a = std::clamp(a, 0.0, 1.0);
    return a * a * (3 - 2 * a);
}

QString whisperProblem()
{
    if (whisperBinary().isEmpty())
        return QStringLiteral("Captions need whisper.cpp. Install it with: brew install whisper-cpp");
    if (whisperModel().isEmpty())
        return QStringLiteral("Captions need a whisper model in ~/ai-studio/models/whisper, e.g. ggml-small.bin from "
                              "https://huggingface.co/ggerganov/whisper.cpp");
    return {};
}

Transcript transcribe(const Material &m, qint64 from, qint64 to)
{
    to = std::min(to, m.durationUs > 0 ? m.durationUs : to);
    const QString key = fileKey(m) + QStringLiteral("|%1|%2|%3").arg(from).arg(to).arg(QFileInfo(whisperModel()).fileName());
    {
        QMutexLocker lock(&g_mutex);
        if (auto it = g_transcripts.constFind(key); it != g_transcripts.cend()) return *it;
    }
    const QString cached = cachePath(key);
    if (QFile f(cached); f.open(QIODevice::ReadOnly)) {
        const Transcript t = fromJson(QJsonDocument::fromJson(f.readAll()).object());
        QMutexLocker lock(&g_mutex);
        g_transcripts.insert(key, t);
        return t;
    }

    Transcript out;
    if (const QString problem = whisperProblem(); !problem.isEmpty()) {
        out.error = problem;
        return out;
    }
    if (!m.hasAudio || to <= from) return out;  // nothing to hear: no words
    QTemporaryDir tmp;
    const QString wav = tmp.filePath(QStringLiteral("speech.wav")), base = tmp.filePath(QStringLiteral("speech"));
    if (!tmp.isValid() || !writeWav16k(decodeMono(m, 3, from, to), wav)) {
        out.error = QStringLiteral("Couldn't read the audio of %1.").arg(m.name);
        return out;
    }
    QProcess p;
    p.start(whisperBinary(), {QStringLiteral("-m"), whisperModel(), QStringLiteral("-f"), wav, QStringLiteral("-l"), QStringLiteral("auto"),
                              QStringLiteral("-oj"), QStringLiteral("-of"), base, QStringLiteral("-ml"), QStringLiteral("1"),
                              QStringLiteral("-sow"), QStringLiteral("-np"), QStringLiteral("-t"), QStringLiteral("8")});
    if (!p.waitForFinished(10 * 60 * 1000) || p.exitCode() != 0) {
        out.error = QStringLiteral("whisper failed on %1: %2").arg(m.name, QString::fromUtf8(p.readAllStandardError()).trimmed().right(300));
        return out;
    }
    QFile json(base + QStringLiteral(".json"));
    if (!json.open(QIODevice::ReadOnly)) {
        out.error = QStringLiteral("whisper wrote no transcript for %1.").arg(m.name);
        return out;
    }
    const QJsonObject doc = QJsonDocument::fromJson(json.readAll()).object();
    out.language = doc.value("result").toObject().value("language").toString();
    static const QRegularExpression noise(QStringLiteral(R"(^[\[\(*♪].*[\]\)*♪]$)"));
    for (const QJsonValue &v : doc.value("transcription").toArray()) {
        const QJsonObject o = v.toObject();
        const QString text = o.value("text").toString().trimmed();
        if (text.isEmpty() || noise.match(text).hasMatch()) continue;
        const QJsonObject off = o.value("offsets").toObject();
        out.words.append({from + qint64(off.value("from").toDouble() * 1000), from + qint64(off.value("to").toDouble() * 1000), text});
    }
    if (QFile f(cached); f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(toJson(out)).toJson(QJsonDocument::Compact));
    QMutexLocker lock(&g_mutex);
    g_transcripts.insert(key, out);
    return out;
}

QList<QList<Word>> captionChunks(const QList<Word> &words, int maxWords, qint64 maxUs)
{
    QList<QList<Word>> out;
    QList<Word> cur;
    for (const Word &w : words) {
        if (!cur.isEmpty()) {
            const QChar lastChar = cur.last().text.isEmpty() ? QChar() : cur.last().text.back();
            const bool sentenceEnd = QStringLiteral(".!?").contains(lastChar);
            const bool pause = w.start - cur.last().end > 600'000;
            const bool full = cur.size() >= maxWords || w.end - cur.first().start > maxUs;
            const bool comma = QStringLiteral(",;:").contains(lastChar) && cur.size() >= 2;
            if (sentenceEnd || pause || full || comma) {
                out << cur;
                cur.clear();
            }
        }
        cur << w;
    }
    if (!cur.isEmpty()) out << cur;
    return out;
}

QList<QString> captionSources(const Draft &d)
{
    QList<QString> speech, voice, video;
    for (const Track &t : d.tracks)
        for (const Segment &s : t.segments) {
            const Material *m = d.material(s.materialId);
            if (!m || !m->hasAudio || s.volume <= 0 || s.origin == QLatin1String("captions")) continue;
            if (s.speech) speech << s.id;
            else if (t.role == TrackRole::Audio && QFileInfo(m->name).completeBaseName().contains(QRegularExpression(
                                                        QStringLiteral("voice|narrat|vo[-_ ]|speech|interview|podcast"), QRegularExpression::CaseInsensitiveOption)))
                voice << s.id;
            else if (t.role == TrackRole::Main && m->kind == MaterialKind::Video) video << s.id;
        }
    return !speech.isEmpty() ? speech : !voice.isEmpty() ? voice : video;
}

int removeCaptions(Draft &d)
{
    int removed = 0;
    for (Track &t : d.tracks)
        removed += int(t.segments.removeIf([](const Segment &s) { return s.origin == QLatin1String("captions"); }));
    if (removed) d.normalize();
    return removed;
}

int addCaptions(Draft &d, const QString &style, QString *error)
{
    if (const QString problem = whisperProblem(); !problem.isEmpty()) {
        if (error) *error = problem;
        return -1;
    }
    const QList<QString> sources = captionSources(d);
    if (sources.isEmpty()) {
        if (error) *error = QStringLiteral("Nothing in the video has sound to caption.");
        return -1;
    }
    QList<Word> words;  // timeline times
    QStringList problems;
    for (const QString &id : sources) {
        const Segment *s = std::as_const(d).segment(id);
        const Material *m = s ? d.material(s->materialId) : nullptr;
        if (!m || s->speed <= 0) continue;
        const Transcript t = transcribe(*m, s->sourceStart, s->sourceStart + s->sourceSpan());
        if (!t.ok()) {
            problems << t.error;
            continue;
        }
        for (const Word &w : t.words) {
            const qint64 a = s->start + qint64(double(w.start - s->sourceStart) / s->speed);
            const qint64 b = s->start + qint64(double(w.end - s->sourceStart) / s->speed);
            if (a < s->start - 50'000 || a >= s->end()) continue;
            words.append({std::max(a, s->start), std::min(std::max(b, a + 120'000), s->end()), w.text});
        }
    }
    if (words.isEmpty()) {
        if (error) *error = problems.isEmpty() ? QStringLiteral("No speech found to caption.") : problems.join('\n');
        return problems.isEmpty() ? 0 : -1;
    }
    std::sort(words.begin(), words.end(), [](const Word &a, const Word &b) { return a.start < b.start; });

    removeCaptions(d);
    d.ensureTextMaterial();
    const qint64 videoEnd = d.mainTrack().segments.isEmpty() ? d.duration() : d.mainTrack().segments.last().end();
    Track track;
    track.id = Draft::newId();
    track.role = TrackRole::Overlay;
    const QList<QList<Word>> chunks = captionChunks(words);
    for (qsizetype i = 0; i < chunks.size(); ++i) {
        const QList<Word> &c = chunks[i];
        QStringList text;
        for (const Word &w : c) text << w.text;
        Segment s;
        s.id = Draft::newId();
        s.materialId = kTextMaterialId;
        s.origin = QStringLiteral("captions");
        if (!applyTextPreset(style, &s.text, &s.transform, &s.motion)) applyTextPreset(QStringLiteral("caption"), &s.text, &s.transform, &s.motion);
        s.text.text = text.join(QLatin1Char(' '));
        s.start = c.first().start;
        qint64 end = c.last().end + 300'000;
        if (i + 1 < chunks.size()) end = std::min(end, chunks[i + 1].first().start);
        if (videoEnd > 0) end = std::min(end, videoEnd);
        if (end - s.start < 300'000) continue;
        s.duration = end - s.start;
        // Stay clear of other text on screen at the same time (titles, buttons):
        // the lower third first (where captions read best in ads), then the preset, then higher up.
        auto clashes = [&](double y) {
            for (const Track &t : d.tracks)
                for (const Segment &o : t.segments)
                    if (o.materialId == kTextMaterialId && o.origin != QLatin1String("captions") && o.start < s.start + s.duration
                        && s.start < o.end() && std::abs(o.transform.y - y) < 0.3)
                        return true;
            return false;
        };
        for (double y : {0.62, s.transform.y, 0.4, -0.7}) {
            if (!clashes(y)) {
                s.transform.y = y;
                break;
            }
        }
        // Word-by-word styles reveal across the time the words are spoken;
        // others keep entrances quick so the words read in time.
        if (s.motion.in == QLatin1String("kinetic") || s.motion.in == QLatin1String("typewriter"))
            s.motion.inUs = std::clamp<qint64>(c.last().start - c.first().start + 200'000, 150'000, s.duration * 9 / 10);
        else
            s.motion.inUs = std::min<qint64>(s.motion.inUs, s.duration / 3);
        s.motion.outUs = std::min<qint64>(s.motion.outUs, 150'000);
        track.segments.append(s);
    }
    const int count = int(track.segments.size());
    if (count) d.tracks.append(track);
    d.normalize();
    if (error) *error = problems.join('\n');
    return count;
}

}  // namespace mtg
