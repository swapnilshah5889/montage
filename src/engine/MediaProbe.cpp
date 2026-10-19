#include "MediaProbe.h"

#include "FFmpeg.h"
#include "VideoDecoder.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QImageReader>

namespace mtg {

static QString thumbPath(const QString &thumbDir, const QString &path)
{
    const QByteArray key = QCryptographicHash::hash(
        (path + QString::number(QFileInfo(path).lastModified().toMSecsSinceEpoch())).toUtf8(),
        QCryptographicHash::Sha1);
    return QDir(thumbDir).filePath(QString::fromLatin1(key.toHex().left(20)) + QStringLiteral(".jpg"));
}

static bool isStillImage(const QString &path)
{
    static const QStringList exts{"png", "jpg", "jpeg", "webp", "bmp", "gif", "tif", "tiff", "heic", "heif"};
    return exts.contains(QFileInfo(path).suffix().toLower());
}

bool probeMedia(const QString &path, const QString &thumbDir, Material *m, QString *error)
{
    initFFmpeg();
    QDir().mkpath(thumbDir);
    m->path = path;
    m->name = QFileInfo(path).fileName();
    m->thumbnail = thumbPath(thumbDir, path);

    if (isStillImage(path)) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        const QImage img = reader.read();
        if (img.isNull()) {
            if (error) *error = reader.errorString();
            return false;
        }
        m->kind = MaterialKind::Image;
        m->width = img.width();
        m->height = img.height();
        m->hasVideo = true;
        m->durationUs = 0;
        img.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(m->thumbnail, "JPG", 85);
        return true;
    }

    AVFormatContext *fmt = nullptr;
    int r = avformat_open_input(&fmt, path.toUtf8().constData(), nullptr, nullptr);
    if (r < 0) {
        if (error) *error = avErr(r);
        return false;
    }
    avformat_find_stream_info(fmt, nullptr);

    int v = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (v >= 0 && (fmt->streams[v]->disposition & AV_DISPOSITION_ATTACHED_PIC)) v = -1;  // album art
    const int a = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    m->hasVideo = v >= 0;
    m->hasAudio = a >= 0;
    m->durationUs = fmt->duration != AV_NOPTS_VALUE ? fmt->duration : 0;
    avformat_close_input(&fmt);

    if (!m->hasVideo && !m->hasAudio) {
        if (error) *error = QStringLiteral("No audio or video streams");
        return false;
    }
    if (m->durationUs <= 0) {
        if (error) *error = QStringLiteral("Unknown duration");
        return false;
    }

    if (!m->hasVideo) {
        m->kind = MaterialKind::Audio;
        m->thumbnail.clear();
        return true;
    }

    m->kind = MaterialKind::Video;
    VideoDecoder dec;
    if (!dec.open(path, error)) return false;
    const QSize size = dec.displaySize();
    m->width = size.width();
    m->height = size.height();
    m->fps = dec.fps();
    if (dec.durationUs() > 0) m->durationUs = std::min(m->durationUs, dec.durationUs());

    const QSize thumb = size.scaled(320, 180, Qt::KeepAspectRatio);
    const QImage img = dec.frameAt(std::min<qint64>(1'000'000, m->durationUs / 3), thumb);
    if (!img.isNull()) img.save(m->thumbnail, "JPG", 85);
    return true;
}

}  // namespace mtg
