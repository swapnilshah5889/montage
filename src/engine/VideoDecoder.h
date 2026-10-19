#pragma once

#include <QImage>
#include <QSize>
#include <QString>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace mtg {

// Random-access frame reader for one video file. Sequential reads (playback)
// decode forward; jumps backwards or far ahead seek to the nearest keyframe.
class VideoDecoder
{
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    bool open(const QString &path, QString *error = nullptr);
    void close();
    bool isOpen() const { return m_codec != nullptr; }

    // The frame on screen at `us` (from the start of the file), scaled to
    // `size` in display orientation. Returns a null image past the end.
    QImage frameAt(qint64 us, QSize size);

    QSize displaySize() const;
    qint64 durationUs() const { return m_durationUs; }
    double fps() const { return m_fps; }

private:
    bool seekTo(qint64 us);
    bool decodeInto(AVFrame *dst);
    qint64 frameUs(const AVFrame *f) const;
    QImage convert(const AVFrame *f, QSize size);

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    AVPacket *m_pkt = nullptr;
    AVFrame *m_cur = nullptr;   // frame being shown
    AVFrame *m_next = nullptr;  // decoded look-ahead frame
    SwsContext *m_sws = nullptr;
    int m_stream = -1;
    int m_rotation = 0;
    bool m_hasCur = false;
    bool m_hasNext = false;
    bool m_eof = false;
    qint64 m_curUs = 0;
    qint64 m_floorUs = 0;
    qint64 m_nextUs = 0;
    qint64 m_durationUs = 0;
    double m_fps = 0;

    QImage m_cache;
    qint64 m_cacheUs = -1;
};

}  // namespace mtg
