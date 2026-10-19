#include "ExportJob.h"

#include "FFmpeg.h"
#include "Renderer.h"

#include <QFile>
#include <vector>

namespace mtg {

namespace {

struct Output {
    AVFormatContext *fmt = nullptr;
    AVCodecContext *venc = nullptr;
    AVCodecContext *aenc = nullptr;
    AVStream *vst = nullptr;
    AVStream *ast = nullptr;
    AVFrame *vframe = nullptr;
    AVFrame *aframe = nullptr;
    AVPacket *pkt = nullptr;
    SwsContext *sws = nullptr;

    ~Output()
    {
        sws_freeContext(sws);
        av_packet_free(&pkt);
        av_frame_free(&aframe);
        av_frame_free(&vframe);
        avcodec_free_context(&aenc);
        avcodec_free_context(&venc);
        if (fmt) {
            if (fmt->pb) avio_closep(&fmt->pb);
            avformat_free_context(fmt);
        }
    }
};

// Hardware encoder first, then software fallbacks.
AVCodecContext *openVideoEncoder(const Canvas &c, AVRational fps, bool globalHeader, QString *name)
{
    for (const char *codecName : {"h264_videotoolbox", "libx264", "mpeg4"}) {
        const AVCodec *codec = avcodec_find_encoder_by_name(codecName);
        if (!codec) continue;
        AVCodecContext *enc = avcodec_alloc_context3(codec);
        enc->width = c.width;
        enc->height = c.height;
        enc->time_base = av_inv_q(fps);
        enc->framerate = fps;
        enc->pix_fmt = AV_PIX_FMT_YUV420P;
        enc->gop_size = std::max(1, int(av_q2d(fps) * 2));
        enc->color_range = AVCOL_RANGE_MPEG;
        enc->colorspace = AVCOL_SPC_BT709;
        enc->color_primaries = AVCOL_PRI_BT709;
        enc->color_trc = AVCOL_TRC_BT709;
        if (globalHeader) enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVDictionary *opts = nullptr;
        if (qstrcmp(codecName, "libx264") == 0) {
            av_dict_set(&opts, "preset", "veryfast", 0);
            av_dict_set(&opts, "crf", "20", 0);
        } else {
            // ~0.15 bits per pixel: about 9 Mbit/s for 1080p30.
            enc->bit_rate = int64_t(double(c.width) * c.height * av_q2d(fps) * 0.15);
        }
        const int r = avcodec_open2(enc, codec, &opts);
        av_dict_free(&opts);
        if (r >= 0) {
            *name = QString::fromLatin1(codecName);
            return enc;
        }
        avcodec_free_context(&enc);
    }
    return nullptr;
}

bool drain(Output &o, AVCodecContext *enc, AVStream *st, QString *err)
{
    for (;;) {
        const int r = avcodec_receive_packet(enc, o.pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
        if (r < 0) {
            *err = avErr(r);
            return false;
        }
        av_packet_rescale_ts(o.pkt, enc->time_base, st->time_base);
        o.pkt->stream_index = st->index;
        const int w = av_interleaved_write_frame(o.fmt, o.pkt);
        if (w < 0) {
            *err = avErr(w);
            return false;
        }
    }
}

bool encode(Output &o, AVCodecContext *enc, AVStream *st, AVFrame *frame, QString *err)
{
    const int r = avcodec_send_frame(enc, frame);
    if (r < 0 && r != AVERROR_EOF) {
        *err = avErr(r);
        return false;
    }
    return drain(o, enc, st, err);
}

}  // namespace

ExportResult exportDraft(const Draft &draft, const QString &path,
                         const std::function<bool(double)> &progress)
{
    initFFmpeg();
    ExportResult res;
    const qint64 duration = draft.duration();
    if (duration <= 0) {
        res.message = QStringLiteral("The timeline is empty.");
        return res;
    }

    Canvas canvas = draft.canvas;
    canvas.width &= ~1;
    canvas.height &= ~1;
    const AVRational fps = av_d2q(canvas.fps > 0 ? canvas.fps : 30.0, 1001000);

    Output o;
    QString err;
    const QByteArray file = path.toUtf8();
    int r = avformat_alloc_output_context2(&o.fmt, nullptr, "mp4", file.constData());
    if (r < 0) {
        res.message = avErr(r);
        return res;
    }
    const bool globalHeader = o.fmt->oformat->flags & AVFMT_GLOBALHEADER;

    o.venc = openVideoEncoder(canvas, fps, globalHeader, &res.videoEncoder);
    if (!o.venc) {
        res.message = QStringLiteral("No usable H.264 encoder.");
        return res;
    }
    o.vst = avformat_new_stream(o.fmt, nullptr);
    avcodec_parameters_from_context(o.vst->codecpar, o.venc);
    o.vst->time_base = o.venc->time_base;
    o.vst->avg_frame_rate = fps;

    const AVCodec *aac = avcodec_find_encoder(AV_CODEC_ID_AAC);
    o.aenc = avcodec_alloc_context3(aac);
    o.aenc->sample_rate = kSampleRate;
    o.aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
    o.aenc->bit_rate = 192000;
    o.aenc->time_base = AVRational{1, kSampleRate};
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&o.aenc->ch_layout, &stereo);
    if (globalHeader) o.aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if ((r = avcodec_open2(o.aenc, aac, nullptr)) < 0) {
        res.message = QStringLiteral("AAC encoder: ") + avErr(r);
        return res;
    }
    o.ast = avformat_new_stream(o.fmt, nullptr);
    avcodec_parameters_from_context(o.ast->codecpar, o.aenc);
    o.ast->time_base = o.aenc->time_base;

    if ((r = avio_open(&o.fmt->pb, file.constData(), AVIO_FLAG_WRITE)) < 0) {
        res.message = avErr(r);
        return res;
    }
    AVDictionary *muxOpts = nullptr;
    av_dict_set(&muxOpts, "movflags", "+faststart", 0);
    r = avformat_write_header(o.fmt, &muxOpts);
    av_dict_free(&muxOpts);
    if (r < 0) {
        res.message = avErr(r);
        return res;
    }

    o.pkt = av_packet_alloc();
    o.vframe = av_frame_alloc();
    o.vframe->format = o.venc->pix_fmt;
    o.vframe->width = canvas.width;
    o.vframe->height = canvas.height;
    av_frame_get_buffer(o.vframe, 0);

    const int aFrameSize = o.aenc->frame_size > 0 ? o.aenc->frame_size : 1024;
    o.aframe = av_frame_alloc();
    o.aframe->format = AV_SAMPLE_FMT_FLTP;
    o.aframe->nb_samples = aFrameSize;
    o.aframe->sample_rate = kSampleRate;
    av_channel_layout_copy(&o.aframe->ch_layout, &stereo);
    av_frame_get_buffer(o.aframe, 0);

    o.sws = sws_getContext(canvas.width, canvas.height, AV_PIX_FMT_BGRA, canvas.width, canvas.height,
                           AV_PIX_FMT_YUV420P, SWS_BICUBIC, nullptr, nullptr, nullptr);
    sws_setColorspaceDetails(o.sws, sws_getCoefficients(SWS_CS_DEFAULT), 1,
                             sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16);

    Renderer renderer;
    renderer.setDraft(draft);
    const QSize size(canvas.width, canvas.height);
    const int64_t totalFrames = av_rescale_q_rnd(duration, AV_TIME_BASE_Q, o.venc->time_base, AV_ROUND_UP);
    const int64_t totalSamples = av_rescale(duration, kSampleRate, AV_TIME_BASE);
    int64_t samplesDone = 0;
    std::vector<float> mix(size_t(aFrameSize) * kChannels);

    auto encodeAudioUntil = [&](int64_t untilSample) {
        while (samplesDone < untilSample) {
            renderer.renderAudio(samplesDone, aFrameSize, mix.data());
            av_frame_make_writable(o.aframe);
            auto *left = reinterpret_cast<float *>(o.aframe->data[0]);
            auto *right = reinterpret_cast<float *>(o.aframe->data[1]);
            for (int i = 0; i < aFrameSize; ++i) {
                left[i] = mix[size_t(i) * 2];
                right[i] = mix[size_t(i) * 2 + 1];
            }
            o.aframe->pts = samplesDone;
            if (!encode(o, o.aenc, o.ast, o.aframe, &err)) return false;
            samplesDone += aFrameSize;
        }
        return true;
    };

    for (int64_t i = 0; i < totalFrames; ++i) {
        const qint64 t = av_rescale_q(i, o.venc->time_base, AV_TIME_BASE_Q);
        const QImage img = renderer.renderVideo(t, size);

        av_frame_make_writable(o.vframe);
        const uint8_t *src[4] = {img.constBits(), nullptr, nullptr, nullptr};
        const int srcStride[4] = {int(img.bytesPerLine()), 0, 0, 0};
        sws_scale(o.sws, src, srcStride, 0, canvas.height, o.vframe->data, o.vframe->linesize);
        o.vframe->pts = i;
        if (!encode(o, o.venc, o.vst, o.vframe, &err)) break;

        const qint64 nextT = av_rescale_q(i + 1, o.venc->time_base, AV_TIME_BASE_Q);
        if (!encodeAudioUntil(std::min(totalSamples, av_rescale(nextT, kSampleRate, AV_TIME_BASE)))) break;

        if (progress && !progress(double(i + 1) / double(totalFrames))) {
            res.cancelled = true;
            break;
        }
    }

    if (err.isEmpty() && !res.cancelled) {
        encode(o, o.venc, o.vst, nullptr, &err);
        if (err.isEmpty()) encode(o, o.aenc, o.ast, nullptr, &err);
        av_write_trailer(o.fmt);
    }

    if (res.cancelled || !err.isEmpty()) {
        avio_closep(&o.fmt->pb);
        QFile::remove(path);
        res.message = res.cancelled ? QStringLiteral("Export cancelled.") : err;
        return res;
    }

    res.ok = true;
    res.message = QStringLiteral("Exported %1 frames with %2.").arg(totalFrames).arg(res.videoEncoder);
    return res;
}

}  // namespace mtg
