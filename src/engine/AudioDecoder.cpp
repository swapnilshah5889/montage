#include "AudioDecoder.h"

#include "FFmpeg.h"

#include <algorithm>

namespace mtg {

AudioDecoder::~AudioDecoder()
{
    close();
}

void AudioDecoder::close()
{
    swr_free(&m_swr);
    av_frame_free(&m_frame);
    av_packet_free(&m_pkt);
    avcodec_free_context(&m_codec);
    avformat_close_input(&m_fmt);
    m_stream = -1;
    m_eof = false;
    m_buf.clear();
    m_bufStart = -1;
}

bool AudioDecoder::open(const QString &path, QString *error)
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
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (m_stream < 0 || !dec) return fail(QStringLiteral("No audio stream"));

    m_codec = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(m_codec, m_fmt->streams[m_stream]->codecpar);
    if ((r = avcodec_open2(m_codec, dec, nullptr)) < 0) return fail(avErr(r));
    if (m_codec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&m_codec->ch_layout, m_codec->ch_layout.nb_channels);

    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    r = swr_alloc_set_opts2(&m_swr, &stereo, AV_SAMPLE_FMT_FLT, kSampleRate, &m_codec->ch_layout,
                            m_codec->sample_fmt, m_codec->sample_rate, 0, nullptr);
    if (r < 0 || (r = swr_init(m_swr)) < 0) return fail(avErr(r));

    m_pkt = av_packet_alloc();
    m_frame = av_frame_alloc();
    return true;
}

void AudioDecoder::seekTo(qint64 sample)
{
    AVStream *st = m_fmt->streams[m_stream];
    const qint64 us = av_rescale(sample, AV_TIME_BASE, kSampleRate);
    av_seek_frame(m_fmt, m_stream, usToStream(st, std::max<qint64>(0, us)), AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(m_codec);
    swr_init(m_swr);  // drop resampler history
    m_buf.clear();
    m_bufStart = -1;
    m_eof = false;
}

bool AudioDecoder::decodeMore()
{
    for (;;) {
        int r = avcodec_receive_frame(m_codec, m_frame);
        if (r == 0) break;
        if (r == AVERROR_EOF) {
            m_eof = true;
            return false;
        }
        if (r != AVERROR(EAGAIN)) return false;

        r = av_read_frame(m_fmt, m_pkt);
        if (r < 0) {
            avcodec_send_packet(m_codec, nullptr);
            continue;
        }
        if (m_pkt->stream_index == m_stream) avcodec_send_packet(m_codec, m_pkt);
        av_packet_unref(m_pkt);
    }

    if (m_bufStart < 0) {
        int64_t ts = m_frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = m_frame->pts;
        const qint64 us = ts == AV_NOPTS_VALUE ? 0 : streamToUs(m_fmt->streams[m_stream], ts);
        m_bufStart = av_rescale(us, kSampleRate, AV_TIME_BASE);
    }

    const int maxOut = swr_get_out_samples(m_swr, m_frame->nb_samples);
    const size_t old = m_buf.size();
    m_buf.resize(old + size_t(maxOut) * kChannels);
    uint8_t *out = reinterpret_cast<uint8_t *>(m_buf.data() + old);
    const int got = swr_convert(m_swr, &out, maxOut, const_cast<const uint8_t **>(m_frame->extended_data),
                                m_frame->nb_samples);
    m_buf.resize(old + size_t(std::max(got, 0)) * kChannels);
    av_frame_unref(m_frame);
    return true;
}

void AudioDecoder::mixInto(qint64 startSample, int frames, float *out, float gain)
{
    if (!isOpen() || frames <= 0) return;

    const qint64 bufFrames = qint64(m_buf.size() / kChannels);
    constexpr qint64 kSlack = kSampleRate / 4;
    if (m_bufStart < 0 || startSample < m_bufStart || startSample > m_bufStart + bufFrames + kSlack)
        seekTo(startSample);

    const qint64 endSample = startSample + frames;
    while (!m_eof && (m_bufStart < 0 || m_bufStart + qint64(m_buf.size() / kChannels) < endSample)) {
        if (!decodeMore()) break;
    }
    if (m_bufStart < 0) return;

    const qint64 have = qint64(m_buf.size() / kChannels);
    const qint64 from = std::max(startSample, m_bufStart);
    const qint64 to = std::min(endSample, m_bufStart + have);
    for (qint64 s = from; s < to; ++s) {
        const float *src = &m_buf[size_t(s - m_bufStart) * kChannels];
        float *dst = out + (s - startSample) * kChannels;
        dst[0] += src[0] * gain;
        dst[1] += src[1] * gain;
    }

    // Keep a little history for small backward steps, drop the rest.
    const qint64 keepFrom = std::max(m_bufStart, startSample - kSampleRate / 10);
    if (keepFrom > m_bufStart) {
        const qint64 drop = std::min(keepFrom - m_bufStart, have);
        m_buf.erase(m_buf.begin(), m_buf.begin() + drop * kChannels);
        m_bufStart += drop;
    }
}

}  // namespace mtg
