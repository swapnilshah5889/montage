#include "Reframe.h"

#include "VideoDecoder.h"

#include <QHash>
#include <QMutex>
#include <algorithm>
#include <cmath>

#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Vision/Vision.h>

namespace mtg {

namespace {

constexpr int kAnalysisWidth = 384;

struct SubjectTrack {
    QList<qint64> times;  // source time of each sample
    QList<QPointF> centers;
};

QMutex g_mutex;
QHash<QString, SubjectTrack> g_tracks;

QPointF weightedCenter(NSArray *boxes, bool faces)
{
    double sx = 0, sy = 0, sw = 0, faceH = 0;
    for (VNDetectedObjectObservation *o in boxes) {
        const CGRect r = o.boundingBox;  // normalised, origin bottom-left
        const double w = r.size.width * r.size.height * std::max<double>(0.05, o.confidence);
        sx += (r.origin.x + r.size.width / 2) * w;
        sy += (1 - (r.origin.y + r.size.height / 2)) * w;
        faceH += r.size.height * w;
        sw += w;
    }
    if (sw <= 0) return {-1, -1};
    QPointF c(sx / sw, sy / sw);
    // Frame faces the way a camera operator would: eyes in the upper part
    // of the picture, so aim a little below the face.
    if (faces) c.setY(c.y() + 0.6 * faceH / sw);
    return c;
}

// Centroid of the hottest part of a saliency heat map (one float per cell,
// top row first). The boxes Vision derives from it are wide and pulled to the
// middle; the map itself pins the subject.
QPointF heatCenter(CVPixelBufferRef map)
{
    if (!map || CVPixelBufferGetPixelFormatType(map) != kCVPixelFormatType_OneComponent32Float) return {-1, -1};
    CVPixelBufferLockBaseAddress(map, kCVPixelBufferLock_ReadOnly);
    const size_t w = CVPixelBufferGetWidth(map), h = CVPixelBufferGetHeight(map), stride = CVPixelBufferGetBytesPerRow(map);
    const auto *base = static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(map));
    auto at = [&](size_t x, size_t y) { return reinterpret_cast<const float *>(base + y * stride)[x]; };
    float peak = 0;
    for (size_t y = 0; y < h; ++y)
        for (size_t x = 0; x < w; ++x) peak = std::max(peak, at(x, y));
    double sx = 0, sy = 0, sw = 0;
    const float floor = 0.6f * peak;
    for (size_t y = 0; y < h && peak > 0; ++y)
        for (size_t x = 0; x < w; ++x) {
            const double v = at(x, y) - floor;
            if (v <= 0) continue;
            sx += (x + 0.5) * v * v;
            sy += (y + 0.5) * v * v;
            sw += v * v;
        }
    CVPixelBufferUnlockBaseAddress(map, kCVPixelBufferLock_ReadOnly);
    if (sw <= 0) return {-1, -1};
    return {sx / sw / double(w), sy / sw / double(h)};
}

SubjectTrack analyse(const Material &mat, qint64 sourceStart, qint64 sourceSpan)
{
    SubjectTrack track;
    if (mat.kind == MaterialKind::Image) {
        QImage img(mat.path);
        if (!img.isNull()) {
            track.times << 0;
            track.centers << subjectCenter(img.scaledToWidth(kAnalysisWidth, Qt::SmoothTransformation));
        }
        return track;
    }
    VideoDecoder dec;
    if (!dec.open(mat.path)) return track;
    const QSize display = dec.displaySize();
    if (display.isEmpty()) return track;
    const QSize size(kAnalysisWidth, std::max(2, int(std::lround(kAnalysisWidth * double(display.height()) / display.width()))));
    const qint64 span = std::max<qint64>(1, sourceSpan);
    const int samples = std::clamp(int(span / 1'500'000) + 1, 2, 7);
    const qint64 last = std::max<qint64>(0, dec.durationUs() - 50'000);
    for (int k = 0; k < samples; ++k) {
        const qint64 t = std::clamp<qint64>(sourceStart + span * k / (samples - 1), 0, last);
        const QImage frame = dec.frameAt(t, size);
        if (frame.isNull()) continue;
        track.times << t;
        track.centers << subjectCenter(frame);
    }
    // Smooth so the crop glides, and hold it a little towards the middle.
    if (track.centers.size() >= 3) {
        QList<QPointF> smooth = track.centers;
        for (qsizetype i = 0; i < smooth.size(); ++i) {
            const QPointF a = track.centers[std::max<qsizetype>(0, i - 1)];
            const QPointF b = track.centers[i];
            const QPointF c = track.centers[std::min<qsizetype>(track.centers.size() - 1, i + 1)];
            smooth[i] = (a + 2 * b + c) / 4;
        }
        track.centers = smooth;
    }
    return track;
}

}  // namespace

QPointF subjectCenter(const QImage &frame)
{
    if (frame.isNull()) return {0.5, 0.5};
    const QImage rgba = frame.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QPointF out(0.5, 0.5);
    @autoreleasepool {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, rgba.constBits(), size_t(rgba.sizeInBytes()), nullptr);
        CGImageRef cg = CGImageCreate(size_t(rgba.width()), size_t(rgba.height()), 8, 32, size_t(rgba.bytesPerLine()), cs,
                                      CGBitmapInfo(kCGImageAlphaPremultipliedLast), provider, nullptr, false,
                                      kCGRenderingIntentDefault);
        CGDataProviderRelease(provider);
        CGColorSpaceRelease(cs);
        if (cg) {
            VNImageRequestHandler *handler = [[VNImageRequestHandler alloc] initWithCGImage:cg options:@{}];
            VNDetectFaceRectanglesRequest *faces = [[VNDetectFaceRectanglesRequest alloc] init];
            VNGenerateAttentionBasedSaliencyImageRequest *saliency = [[VNGenerateAttentionBasedSaliencyImageRequest alloc] init];
            NSError *error = nil;
            const BOOL ok = [handler performRequests:@[faces, saliency] error:&error];
            if (ok) {
                QPointF c = weightedCenter(faces.results, true);
                if (c.x() < 0) {
                    VNSaliencyImageObservation *obs = saliency.results.firstObject;
                    if (obs) c = heatCenter(obs.pixelBuffer);
                }
                if (c.x() >= 0) out = QPointF(std::clamp(c.x(), 0.0, 1.0), std::clamp(c.y(), 0.0, 1.0));
                if (qEnvironmentVariableIsSet("MONTAGE_DEBUG")) qInfo("reframe: faces=%lu -> %.2f,%.2f", (unsigned long)faces.results.count, out.x(), out.y());
            }
            CGImageRelease(cg);
        }
    }
    return out;
}

QPointF subjectAt(const Material &mat, qint64 sourceStart, qint64 sourceSpan, qint64 sourceUs)
{
    const QString key = QStringLiteral("%1|%2|%3").arg(mat.path).arg(mat.kind == MaterialKind::Image ? 0 : sourceStart)
                            .arg(mat.kind == MaterialKind::Image ? 0 : sourceSpan);
    SubjectTrack track;
    bool have = false;
    {
        QMutexLocker lock(&g_mutex);
        auto it = g_tracks.constFind(key);
        if (it != g_tracks.cend()) {
            track = *it;
            have = true;
        }
    }
    if (!have) {
        track = analyse(mat, sourceStart, sourceSpan);
        QMutexLocker lock(&g_mutex);
        g_tracks.insert(key, track);
    }
    if (track.centers.isEmpty()) return {0.5, 0.5};
    if (track.centers.size() == 1 || sourceUs <= track.times.first()) return track.centers.first();
    if (sourceUs >= track.times.last()) return track.centers.last();
    for (qsizetype i = 0; i + 1 < track.times.size(); ++i) {
        if (sourceUs >= track.times[i] && sourceUs <= track.times[i + 1]) {
            const double u = track.times[i + 1] > track.times[i]
                                 ? double(sourceUs - track.times[i]) / double(track.times[i + 1] - track.times[i]) : 0;
            const double s = u * u * (3 - 2 * u);  // ease between samples
            return track.centers[i] + (track.centers[i + 1] - track.centers[i]) * s;
        }
    }
    return track.centers.last();
}

}  // namespace mtg
