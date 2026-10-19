#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QString>
#include <cmath>

namespace mtg {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 2;

inline QString avErr(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// Stream timestamp <-> microseconds from the start of the stream.
inline qint64 streamToUs(const AVStream *st, int64_t ts)
{
    const int64_t origin = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    return av_rescale_q(ts - origin, st->time_base, AV_TIME_BASE_Q);
}

inline int64_t usToStream(const AVStream *st, qint64 us)
{
    const int64_t origin = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    return av_rescale_q(us, AV_TIME_BASE_Q, st->time_base) + origin;
}

// Clockwise rotation (0, 90, 180, 270) that should be applied for display.
inline int streamRotation(const AVStream *st)
{
    const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (!sd || sd->size < 9 * 4) return 0;
    const double ccw = av_display_rotation_get(reinterpret_cast<const int32_t *>(sd->data));
    int cw = int(std::lround(-ccw)) % 360;
    if (cw < 0) cw += 360;
    return (cw + 45) / 90 * 90 % 360;
}

void initFFmpeg();

}  // namespace mtg
