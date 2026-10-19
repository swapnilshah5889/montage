#include "VideoDecoder.h"

#include "FFmpeg.h"

#include <QTransform>
#include <mutex>
#include <utility>

namespace mtg {

void initFFmpeg()
{
    static std::once_flag once;
    std::call_once(once, [] { av_log_set_level(AV_LOG_ERROR); });
}

VideoDecoder::~VideoDecoder()
{
    close();
}

void VideoDecoder::close()
{
    sws_freeContext(m_sws);
    m_sws = nullptr;
    av_frame_free(&m_cur);
    av_frame_free(&m_next);
    av_packet_free(&m_pkt);
    avcodec_free_context(&m_codec);
    avformat_close_input(&m_fmt);
    m_stream = -1;
    m_hasCur = m_hasNext = m_eof = false;
    m_cache = {};
    m_cacheUs = -1;
}

bool VideoDecoder::open(const QString &path, QString *error)
{
    close();
    initFFmpeg();
    auto fail = [&](const QString &msg) {
        if (error) *error = msg;
        close();
        return false;
    };

    int r = avformat_open_input(&m_fmt, path.toUtf8().constData(), nullptr, nullptr);
    if (r < 0) return fail(avErr(r));
    if ((r = avformat_find_stream_info(m_fmt, nullptr)) < 0) return fail(avErr(r));

    const AVCodec *dec = nullptr;
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (m_stream < 0 || !dec) return fail(QStringLiteral("No video stream"));
    AVStream *st = m_fmt->streams[m_stream];

    m_codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(m_codec, st->codecpar);
    m_codec->thread_count = 0;
    m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if ((r = avcodec_open2(m_codec, dec, nullptr)) < 0) return fail(avErr(r));

    m_pkt = av_packet_alloc();
    m_cur = av_frame_alloc();
    m_next = av_frame_alloc();
    m_rotation = streamRotation(st);

    const AVRational fr = av_guess_frame_rate(m_fmt, st, nullptr);
    m_fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 30.0;
    if (st->duration != AV_NOPTS_VALUE)
        m_durationUs = av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q);
    else if (m_fmt->duration != AV_NOPTS_VALUE)
        m_durationUs = m_fmt->duration;
    return true;
}

QSize VideoDecoder::displaySize() const
{
    if (!m_codec) return {};
    int w = m_codec->width;
    const int h = m_codec->height;
    const AVRational sar = m_codec->sample_aspect_ratio;
    if (sar.num > 0 && sar.den > 0 && sar.num != sar.den) w = int(std::lround(w * av_q2d(sar)));
    return (m_rotation % 180) ? QSize(h, w) : QSize(w, h);
}

qint64 VideoDecoder::frameUs(const AVFrame *f) const
{
    int64_t ts = f->best_effort_timestamp;
    if (ts == AV_NOPTS_VALUE) ts = f->pts;
    if (ts == AV_NOPTS_VALUE) return m_hasCur ? m_curUs + qint64(1e6 / m_fps) : 0;
    return streamToUs(m_fmt->streams[m_stream], ts);
}

bool VideoDecoder::seekTo(qint64 us)
{
    const int64_t ts = usToStream(m_fmt->streams[m_stream], std::max<qint64>(0, us));
    int r = av_seek_frame(m_fmt, m_stream, ts, AVSEEK_FLAG_BACKWARD);
    if (r < 0) r = av_seek_frame(m_fmt, m_stream, ts, AVSEEK_FLAG_BACKWARD | AVSEEK_FLAG_ANY);
    avcodec_flush_buffers(m_codec);
    m_hasCur = m_hasNext = m_eof = false;
    m_cacheUs = -1;
    return r >= 0;
}

bool VideoDecoder::decodeInto(AVFrame *dst)
{
    for (;;) {
        int r = avcodec_receive_frame(m_codec, dst);
        if (r == 0) return true;
        if (r == AVERROR_EOF) {
            m_eof = true;
            return false;
        }
        if (r != AVERROR(EAGAIN)) return false;

        r = av_read_frame(m_fmt, m_pkt);
        if (r < 0) {
            avcodec_send_packet(m_codec, nullptr);  // drain
            continue;
        }
        if (m_pkt->stream_index == m_stream) avcodec_send_packet(m_codec, m_pkt);
        av_packet_unref(m_pkt);
    }
}

QImage VideoDecoder::frameAt(qint64 us, QSize size)
{
    if (!isOpen()) return {};
    if (size.isEmpty()) size = displaySize();

    constexpr qint64 kForwardWindowUs = 2'000'000;
    // m_floorUs is the earliest time the current frame is valid for (the
    // seek target when the first frame after a seek starts later).
    const bool canRollForward = m_hasCur && us >= m_floorUs && us - m_curUs <= kForwardWindowUs;
    if (!canRollForward) seekTo(us);

    for (;;) {
        if (!m_hasNext) {
            if (m_eof || !decodeInto(m_next)) break;
            m_nextUs = frameUs(m_next);
            m_hasNext = true;
        }
        if (!m_hasCur || m_nextUs <= us) {
            const bool first = !m_hasCur;
            std::swap(m_cur, m_next);
            m_curUs = m_nextUs;
            m_floorUs = first ? std::min(us, m_curUs) : m_curUs;
            m_hasCur = true;
            m_hasNext = false;
            m_cacheUs = -1;
            continue;
        }
        break;
    }

    if (!m_hasCur) return {};
    if (m_cacheUs == m_curUs && m_cache.size() == size) return m_cache;
    m_cache = convert(m_cur, size);
    m_cacheUs = m_curUs;
    return m_cache;
}

QImage VideoDecoder::convert(const AVFrame *f, QSize size)
{
    const QSize coded = (m_rotation % 180) ? size.transposed() : size;
    SwsContext *prev = m_sws;
    m_sws = sws_getCachedContext(m_sws, f->width, f->height, AVPixelFormat(f->format),
                                 coded.width(), coded.height(), AV_PIX_FMT_BGRA, SWS_BILINEAR,
                                 nullptr, nullptr, nullptr);
    if (!m_sws) return {};
    if (m_sws != prev) {
        int cs = SWS_CS_DEFAULT;
        if (f->colorspace == AVCOL_SPC_BT709 || (f->colorspace == AVCOL_SPC_UNSPECIFIED && f->height >= 720))
            cs = SWS_CS_ITU709;
        else if (f->colorspace == AVCOL_SPC_BT2020_NCL || f->colorspace == AVCOL_SPC_BT2020_CL)
            cs = SWS_CS_BT2020;
        const int fullRange = f->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
        sws_setColorspaceDetails(m_sws, sws_getCoefficients(cs), fullRange,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16, 1 << 16);
    }

    // BGRA bytes are ARGB32 on little-endian; video is opaque, so it is
    // already premultiplied.
    QImage img(coded, QImage::Format_ARGB32_Premultiplied);
    uint8_t *dst[4] = {img.bits(), nullptr, nullptr, nullptr};
    int dstStride[4] = {int(img.bytesPerLine()), 0, 0, 0};
    sws_scale(m_sws, f->data, f->linesize, 0, f->height, dst, dstStride);
    if (m_rotation) img = img.transformed(QTransform().rotate(m_rotation));
    return img;
}

}  // namespace mtg
