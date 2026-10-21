#include "Scenes.h"

#include "VideoDecoder.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <vector>

#import <CoreGraphics/CoreGraphics.h>
#import <Vision/Vision.h>

namespace mtg {

namespace {

constexpr int kVersion = 1;
constexpr qint64 kSample = 200'000;  // 5 samples a second
constexpr int kWidth = 192;          // analysis width
constexpr qint64 kMinShot = 600'000;

QMutex g_mutex;
QHash<QString, SceneIndex> g_indexes;

QString fileKey(const Material &m)
{
    const QFileInfo fi(m.path);
    return m.path + QLatin1Char('|') + QString::number(fi.lastModified().toMSecsSinceEpoch()) + QLatin1Char('|') + QString::number(fi.size());
}

QString cachePath(const QString &key)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/scenes");
    QDir().mkpath(dir);
    return dir + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex()) + QStringLiteral(".json");
}

QJsonObject toJson(const SceneIndex &x)
{
    QJsonArray shots;
    for (const Shot &s : x.shots) {
        QJsonArray q;
        for (float v : s.quality) q.append(std::round(v * 100) / 100);
        shots.append(QJsonObject{{"start", double(s.start)}, {"end", double(s.end)}, {"sharpness", s.sharpness},
                                 {"exposure", s.exposure}, {"stability", s.stability}, {"motion", s.motion},
                                 {"faces", s.faces}, {"tags", QJsonArray::fromStringList(s.tags)},
                                 {"description", s.description}, {"framing", s.framing},
                                 {"subjects", QJsonArray::fromStringList(s.subjects)}, {"appeal", s.appeal},
                                 {"quality", q}, {"sample", double(s.sampleUs)}});
    }
    return {{"version", kVersion}, {"path", x.path}, {"described", x.described}, {"shots", shots}};
}

SceneIndex fromJson(const QJsonObject &o)
{
    SceneIndex x;
    if (o.value("version").toInt() != kVersion) return x;
    x.path = o.value("path").toString();
    x.described = o.value("described").toBool();
    for (const QJsonValue &v : o.value("shots").toArray()) {
        const QJsonObject j = v.toObject();
        Shot s;
        s.start = qint64(j.value("start").toDouble());
        s.end = qint64(j.value("end").toDouble());
        s.sharpness = j.value("sharpness").toDouble();
        s.exposure = j.value("exposure").toDouble();
        s.stability = j.value("stability").toDouble();
        s.motion = j.value("motion").toDouble();
        s.faces = j.value("faces").toInt();
        for (const QJsonValue &t : j.value("tags").toArray()) s.tags << t.toString();
        s.description = j.value("description").toString();
        s.framing = j.value("framing").toString();
        for (const QJsonValue &t : j.value("subjects").toArray()) s.subjects << t.toString();
        s.appeal = j.value("appeal").toInt();
        for (const QJsonValue &q : j.value("quality").toArray()) s.quality << float(q.toDouble());
        s.sampleUs = qint64(j.value("sample").toDouble(kSample));
        x.shots << s;
    }
    return x;
}

void store(const Material &m, const SceneIndex &x)
{
    const QString key = fileKey(m);
    if (QFile f(cachePath(key)); f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(toJson(x)).toJson(QJsonDocument::Compact));
    QMutexLocker lock(&g_mutex);
    g_indexes.insert(key, x);
}

// ---- per-frame measurements

struct Sample {
    std::vector<float> hist;  // 3 x 16 bins, normalised
    std::vector<uint8_t> gray;
    int w = 0, h = 0;
    double sharp = 0;      // variance of the Laplacian
    double exposure = 0;   // 0..1
    double dx = 0, dy = 0; // camera shift from the previous sample (pixels)
    double residual = 0;   // what still differs after the shift (content motion)
};

Sample measure(const QImage &frame)
{
    Sample s;
    const QImage img = frame.convertToFormat(QImage::Format_RGB32);
    s.w = img.width();
    s.h = img.height();
    s.hist.assign(48, 0);
    s.gray.resize(size_t(s.w) * size_t(s.h));
    double sum = 0;
    int clipped = 0;
    for (int y = 0; y < s.h; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < s.w; ++x) {
            const int r = qRed(row[x]), g = qGreen(row[x]), b = qBlue(row[x]);
            s.hist[size_t(r >> 4)] += 1;
            s.hist[size_t(16 + (g >> 4))] += 1;
            s.hist[size_t(32 + (b >> 4))] += 1;
            const int l = (r * 77 + g * 150 + b * 29) >> 8;
            s.gray[size_t(y) * size_t(s.w) + size_t(x)] = uint8_t(l);
            sum += l;
            clipped += l < 8 || l > 247;
        }
    }
    const double n = double(s.w) * s.h;
    for (float &v : s.hist) v = float(v / n);
    // Sharpness: variance of the 4-neighbour Laplacian.
    double ls = 0, ls2 = 0;
    int ln = 0;
    for (int y = 1; y + 1 < s.h; ++y)
        for (int x = 1; x + 1 < s.w; ++x) {
            const auto at = [&](int xx, int yy) { return double(s.gray[size_t(yy) * size_t(s.w) + size_t(xx)]); };
            const double lap = 4 * at(x, y) - at(x - 1, y) - at(x + 1, y) - at(x, y - 1) - at(x, y + 1);
            ls += lap;
            ls2 += lap * lap;
            ++ln;
        }
    s.sharp = ln ? ls2 / ln - (ls / ln) * (ls / ln) : 0;
    const double mean = sum / n, clipFrac = clipped / n;
    s.exposure = std::clamp(1 - std::max(0.0, (55 - mean) / 55) - std::max(0.0, (mean - 205) / 50) - std::max(0.0, clipFrac - 0.15) * 1.5, 0.0, 1.0);
    return s;
}

// Camera shift between two samples by block matching on the centre.
void shiftBetween(const Sample &a, Sample &b)
{
    if (a.w != b.w || a.h != b.h || a.w < 32) return;
    // Flat frames (sky, gradients, blur) have nothing to match: no measurable shake.
    if (a.sharp < 40 || b.sharp < 40) return;
    constexpr int R = 6;
    double best = 1e18;
    int bx = 0, by = 0;
    const int x0 = a.w / 6, x1 = a.w - a.w / 6, y0 = a.h / 6, y1 = a.h - a.h / 6;
    for (int dy = -R; dy <= R; ++dy)
        for (int dx = -R; dx <= R; ++dx) {
            double sad = 0;
            for (int y = y0; y < y1; y += 2)
                for (int x = x0; x < x1; x += 2)
                    sad += std::abs(int(a.gray[size_t(y) * size_t(a.w) + size_t(x)]) - int(b.gray[size_t(y + dy) * size_t(b.w) + size_t(x + dx)]));
            if (sad < best) best = sad, bx = dx, by = dy;
        }
    const double count = double((x1 - x0) / 2) * ((y1 - y0) / 2);
    b.dx = bx;
    b.dy = by;
    b.residual = count > 0 ? best / count : 0;
}

double histDistance(const Sample &a, const Sample &b)
{
    double d = 0;
    for (size_t i = 0; i < a.hist.size(); ++i) d += std::abs(a.hist[i] - b.hist[i]);
    return d / 3;  // 0..2
}

double sharpScore(double lapVar)
{
    return std::clamp((std::log10(1 + lapVar) - 1.3) / 1.7, 0.0, 1.0);  // ~20 blurry .. ~1000 crisp
}

// ---- Vision: faces and labels on a frame

void looksAt(const QImage &frame, int *faces, QStringList *tags)
{
    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, rgba.constBits(), size_t(rgba.sizeInBytes()), nullptr);
        CGImageRef cg = CGImageCreate(size_t(rgba.width()), size_t(rgba.height()), 8, 32, size_t(rgba.bytesPerLine()), cs,
                                      CGBitmapInfo(kCGImageAlphaPremultipliedLast), provider, nullptr, false, kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(cs);
        if (!cg) return;
        VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:cg options:@{}];
        VNDetectFaceRectanglesRequest *faceReq = [[VNDetectFaceRectanglesRequest alloc] init];
        VNClassifyImageRequest *labelReq = tags ? [[VNClassifyImageRequest alloc] init] : nil;
        NSMutableArray *reqs = [NSMutableArray arrayWithObject:faceReq];
        if (labelReq) [reqs addObject:labelReq];
        NSError *error = nil;
        if ([handler performRequests:reqs error:&error]) {
            if (faces) *faces = std::max(*faces, int(faceReq.results.count));
            if (labelReq) {
                for (VNClassificationObservation *o in labelReq.results) {
                    if (o.confidence < 0.3f || tags->size() >= 6) continue;
                    const QString label = QString::fromNSString(o.identifier).replace(QLatin1Char('_'), QLatin1Char(' '));
                    if (!tags->contains(label)) tags->append(label);
                }
            }
        }
        CGImageRelease(cg);
    }
}

QSize analysisSize(QSize display, int width)
{
    if (display.isEmpty()) return {};
    return QSize(width, std::max(2, int(std::lround(width * double(display.height()) / display.width())) & ~1));
}

SceneIndex analyseImage(const Material &m)
{
    SceneIndex x;
    x.path = m.path;
    QImageReader reader(m.path);
    reader.setAutoTransform(true);
    const QImage img = reader.read();
    if (img.isNull()) return x;
    const Sample s = measure(img.scaledToWidth(kWidth, Qt::SmoothTransformation));
    Shot shot;
    shot.sharpness = sharpScore(s.sharp);
    shot.exposure = s.exposure;
    shot.stability = 1;
    looksAt(img.scaledToWidth(384, Qt::SmoothTransformation), &shot.faces, &shot.tags);
    shot.quality = {float(std::pow(shot.sharpness, 0.7) * shot.exposure)};
    x.shots << shot;
    return x;
}

SceneIndex analyseVideo(const Material &m)
{
    SceneIndex x;
    x.path = m.path;
    VideoDecoder dec;
    if (!dec.open(m.path)) return x;
    const QSize size = analysisSize(dec.displaySize(), kWidth);
    const qint64 duration = m.durationUs > 0 ? m.durationUs : dec.durationUs();
    if (size.isEmpty() || duration <= 0) return x;

    std::vector<Sample> samples;
    std::vector<qint64> times;
    for (qint64 t = 0; t < duration; t += kSample) {
        const QImage f = dec.frameAt(t, size);
        if (f.isNull()) break;
        Sample s = measure(f);
        if (!samples.empty()) shiftBetween(samples.back(), s);
        samples.push_back(std::move(s));
        times.push_back(t);
    }
    if (samples.empty()) return x;

    // Cuts: histogram jumps well above the local level.
    const size_t n = samples.size();
    std::vector<double> dist(n, 0);
    for (size_t i = 1; i < n; ++i) dist[i] = histDistance(samples[i - 1], samples[i]);
    std::vector<size_t> starts{0};
    for (size_t i = 1; i < n; ++i) {
        std::vector<double> around;
        for (size_t k = i >= 8 ? i - 8 : 1; k < std::min(n, i + 9); ++k)
            if (k != i) around.push_back(dist[k]);
        std::sort(around.begin(), around.end());
        const double local = around.empty() ? 0 : around[around.size() / 2];
        if (dist[i] > 0.35 && dist[i] > 4 * local + 0.1 && (times[i] - times[starts.back()]) >= kMinShot) starts.push_back(i);
    }

    for (size_t si = 0; si < starts.size(); ++si) {
        const size_t a = starts[si], b = si + 1 < starts.size() ? starts[si + 1] : n;
        Shot shot;
        shot.start = times[a];
        shot.end = si + 1 < starts.size() ? times[b] : duration;
        shot.sampleUs = kSample;
        std::vector<double> sharp, exposure, residual;
        for (size_t i = a; i < b; ++i) {
            sharp.push_back(samples[i].sharp);
            exposure.push_back(samples[i].exposure);
            if (i > a) residual.push_back(samples[i].residual);
        }
        std::vector<double> sorted = sharp;
        std::sort(sorted.begin(), sorted.end());
        shot.sharpness = sharpScore(sorted[sorted.size() / 2]);
        double e = 0;
        for (double v : exposure) e += v;
        shot.exposure = e / double(exposure.size());
        // Shake: jerk of the camera path (second difference of the shifts).
        double jerk = 0;
        int jn = 0;
        for (size_t i = a + 2; i < b; ++i) {
            jerk += std::hypot(samples[i].dx - samples[i - 1].dx, samples[i].dy - samples[i - 1].dy);
            ++jn;
        }
        jerk = jn ? jerk / jn : 0;
        // Up to a pixel of jitter is moving content, not the camera.
        shot.stability = std::clamp(1 - (jerk - 1.0) / 4.0, 0.0, 1.0);
        double r = 0;
        for (double v : residual) r += v;
        shot.motion = residual.empty() ? 0 : std::clamp(r / double(residual.size()) / 25.0, 0.0, 1.0);
        // Per-sample quality for picking the best stretch.
        for (size_t i = a; i < b; ++i) {
            double local = 0;
            int ln = 0;
            for (size_t k = std::max(a + 1, i > 1 ? i - 1 : 1); k <= std::min(b - 1, i + 1) && k > a; ++k) {
                if (k < a + 1 || k >= b) continue;
                local += std::hypot(samples[k].dx - samples[k - 1].dx, samples[k].dy - samples[k - 1].dy);
                ++ln;
            }
            const double steady = std::clamp(1 - ((ln ? local / ln : 0) - 1.0) / 5.0, 0.0, 1.0);
            shot.quality << float(std::pow(sharpScore(samples[i].sharp), 0.7) * samples[i].exposure * (0.4 + 0.6 * steady));
        }
        x.shots << shot;
    }

    // Faces (three frames per shot) and labels (the middle one), at a size Vision likes.
    const QSize look = analysisSize(dec.displaySize(), 384);
    for (Shot &s : x.shots) {
        for (double f : {0.25, 0.5, 0.75}) {
            const QImage frame = dec.frameAt(s.start + qint64(double(s.length()) * f), look);
            if (frame.isNull()) continue;
            looksAt(frame, &s.faces, f == 0.5 ? &s.tags : nullptr);
        }
    }
    return x;
}

QStringList words(const QString &text)
{
    static const QSet<QString> stop{"the", "a", "an", "of", "with", "and", "or", "in", "on", "at", "to", "for", "shot", "clip",
                                    "part", "bit", "where", "when", "that", "this", "scene", "video", "footage", "my", "our",
                                    "use", "show", "one", "is", "are", "from", "some", "it", "its", "just", "only", "moment"};
    QStringList out;
    for (QString w : text.toLower().split(QRegularExpression(QStringLiteral("[^a-z0-9]+")), Qt::SkipEmptyParts)) {
        if (stop.contains(w) || w.size() < 2) continue;
        // Light stemming so "dogs"/"dog", "running"/"run" meet.
        for (const char *suffix : {"ing", "es", "s", "ed"})
            if (w.size() > 4 && w.endsWith(QLatin1String(suffix))) {
                w.chop(int(qstrlen(suffix)));
                break;
            }
        out << w;
    }
    return out;
}

}  // namespace

double Shot::score() const
{
    const double look = appeal > 0 ? (appeal - 1) / 4.0 : 0.5;
    return std::clamp(0.32 * sharpness + 0.24 * stability + 0.16 * exposure + 0.1 * std::min(1, faces) + 0.13 * look
                          + 0.05 * std::min(1.0, motion * 2),
                      0.0, 1.0);
}

QPair<qint64, qint64> Shot::bestWindow(qint64 length) const
{
    if (end <= start || length >= end - start || quality.size() < 2) {
        const qint64 len = end > start ? std::min(length, end - start) : length;
        // Skip a short lead-in when there's room (cuts often land mid-motion).
        const qint64 lead = end > start ? std::min<qint64>((end - start - len) / 2, 200'000) : 0;
        return {start + lead, start + lead + len};
    }
    const int win = std::max(1, int((length + sampleUs - 1) / sampleUs));
    double best = -1;
    int arg = 0;
    for (int i = 0; i + win <= int(quality.size()); ++i) {
        double s = 0;
        for (int k = i; k < i + win; ++k) s += quality[k];
        if (s > best + 1e-6) best = s, arg = i;
    }
    const qint64 from = std::clamp<qint64>(start + arg * sampleUs, start, end - length);
    return {from, from + length};
}

SceneIndex cachedSceneIndex(const Material &m)
{
    const QString key = fileKey(m);
    {
        QMutexLocker lock(&g_mutex);
        if (auto it = g_indexes.constFind(key); it != g_indexes.cend()) return *it;
    }
    QFile f(cachePath(key));
    if (!f.open(QIODevice::ReadOnly)) return {};
    SceneIndex x = fromJson(QJsonDocument::fromJson(f.readAll()).object());
    if (x.isEmpty()) return x;
    QMutexLocker lock(&g_mutex);
    g_indexes.insert(key, x);
    return x;
}

SceneIndex sceneIndex(const Material &m)
{
    if (SceneIndex cached = cachedSceneIndex(m); !cached.isEmpty()) return cached;
    if (m.kind != MaterialKind::Video && m.kind != MaterialKind::Image) return {};
    SceneIndex x = m.kind == MaterialKind::Image ? analyseImage(m) : analyseVideo(m);
    if (!x.isEmpty()) store(m, x);
    return x;
}

void putSceneIndex(const Material &m, const SceneIndex &index)
{
    QMutexLocker lock(&g_mutex);
    g_indexes.insert(fileKey(m), index);
}

void setShotDescriptions(const Material &m, const QList<Shot> &described)
{
    SceneIndex x = sceneIndex(m);
    bool all = !x.shots.isEmpty();
    for (qsizetype i = 0; i < x.shots.size(); ++i) {
        if (i < described.size() && !described[i].description.isEmpty()) {
            x.shots[i].description = described[i].description;
            x.shots[i].framing = described[i].framing;
            x.shots[i].subjects = described[i].subjects;
            x.shots[i].appeal = described[i].appeal;
        }
        all &= !x.shots[i].description.isEmpty();
    }
    x.described = all;
    store(m, x);
}

QImage shotFrame(const Material &m, const Shot &s, int width)
{
    if (m.kind == MaterialKind::Image) {
        QImageReader reader(m.path);
        reader.setAutoTransform(true);
        return reader.read().scaledToWidth(width, Qt::SmoothTransformation);
    }
    VideoDecoder dec;
    if (!dec.open(m.path)) return {};
    const auto window = s.bestWindow(std::min<qint64>(2'000'000, s.length()));
    return dec.frameAt((window.first + window.second) / 2, analysisSize(dec.displaySize(), width));
}

QList<ShotMatch> findShots(const SceneIndex &index, const QString &query, double minScore)
{
    const QStringList want = words(query);
    QList<ShotMatch> out;
    if (want.isEmpty()) return out;
    for (qsizetype i = 0; i < index.shots.size(); ++i) {
        const Shot &s = index.shots[i];
        const QStringList have = words(s.description + QLatin1Char(' ') + s.framing + (s.faces > 0 ? QStringLiteral(" person people face") : QString()));
        const QStringList main = words(s.subjects.join(' ') + QLatin1Char(' ') + s.tags.join(' '));
        // A word naming the shot's subject counts for more than one in passing.
        double hit = 0;
        for (const QString &w : want) hit += main.contains(w) ? 1.5 : have.contains(w) ? 1.0 : 0.0;
        const double match = std::min(1.0, hit / want.size());
        if (match >= minScore) out.append({int(i), match});
    }
    std::sort(out.begin(), out.end(), [&](const ShotMatch &a, const ShotMatch &b) {
        if (std::abs(a.match - b.match) > 1e-9) return a.match > b.match;
        return index.shots[a.shot].score() > index.shots[b.shot].score();
    });
    return out;
}

}  // namespace mtg
