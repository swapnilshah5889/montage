#pragma once

#include <QString>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

namespace mtg {

// Decodes one file's audio to 48 kHz interleaved stereo float and serves
// arbitrary sample ranges, seeking when the request jumps.
class AudioDecoder
{
public:
    AudioDecoder() = default;
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder &) = delete;
    AudioDecoder &operator=(const AudioDecoder &) = delete;

    bool open(const QString &path, QString *error = nullptr);
    void close();
    bool isOpen() const { return m_codec != nullptr; }

    // Adds `frames` stereo frames starting at `startSample` (48 kHz, from the
    // start of the file) into `out`, multiplied by `gain`.
    void mixInto(qint64 startSample, int frames, float *out, float gain);

private:
    void seekTo(qint64 sample);
    bool decodeMore();

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    AVPacket *m_pkt = nullptr;
    AVFrame *m_frame = nullptr;
    SwrContext *m_swr = nullptr;
    int m_stream = -1;
    bool m_eof = false;

    std::vector<float> m_buf;  // interleaved stereo
    qint64 m_bufStart = -1;    // sample index of m_buf[0], -1 until known
};

}  // namespace mtg
