#pragma once

// Speech: when someone is talking (for ducking music under them) and what
// they say (for captions). Talking is found from the voice track's loudness,
// so ducking works without a transcript; words come from whisper.cpp
// running locally (`whisper-cli` + a ggml model).

#include "model/Draft.h"

#include <QList>
#include <QPair>
#include <QString>

namespace mtg {

// Spans of a file (microseconds from its start) where it is clearly
// audible: speech or other foreground sound. Cached per file; thread-safe.
QList<QPair<qint64, qint64>> audibleSpans(const Material &m);

// Timeline spans where a segment marked as speech is talking.
QList<QPair<qint64, qint64>> speechSpans(const Draft &d);

// How much a ducked track should dip at timeline time `t`: 0 = not at all,
// 1 = fully (quick fade down before speech, slower recovery after).
double duckAmount(const QList<QPair<qint64, qint64>> &spans, qint64 t);

struct Word {
    qint64 start = 0, end = 0;  // microseconds from the start of the file
    QString text;
};

struct Transcript {
    QString language;
    QList<Word> words;
    QString error;
    bool done = false;  // transcribed (even if nobody speaks)
    bool ok() const { return error.isEmpty(); }
};

// A sentence (or a stretch between long pauses), fillers left out of `text`.
struct Line {
    qint64 start = 0, end = 0;  // microseconds in the file
    QString text;
    int firstWord = 0, lastWord = 0;
};

// Where Montage keeps whisper models it downloads, and the model in use (empty if none).
QString whisperModelDir();
QString whisperModelPath();

// Why captions can't be made ("install whisper-cpp with Homebrew..."), or
// empty when whisper and a model are available.
QString whisperProblem();

// Everything said in a file, word by word. Blocking (whisper runs about ten
// times faster than real time); cached in memory and on disk per file.
Transcript fileTranscript(const Material &m);
// The cached transcript, without transcribing; `done` is false if there is none yet.
Transcript cachedFileTranscript(const Material &m);
// What's said in [from, to) of a file (from the whole-file transcript).
Transcript transcribe(const Material &m, qint64 from, qint64 to);

// Loudness (RMS) of a file every 10 ms. Cached per file; thread-safe.
QList<float> loudness(const Material &m);

// Replaces a file's transcript in the in-memory cache (tests, imports).
void putTranscript(const Material &m, Transcript t);

// "um", "uh", "erm", "hmm"...
bool isFiller(const QString &word);
// Sentences, split at punctuation and long pauses.
QList<Line> transcriptLines(const Transcript &t);

// Splits words into caption lines: a few words each, breaking at
// punctuation, pauses and `maxUs`.
QList<QList<Word>> captionChunks(const QList<Word> &words, int maxWords = 4, qint64 maxUs = 1'800'000);

// Segments whose words get captioned: those marked as speech; if none, audio
// tracks named like voiceovers; if none, main-track video with sound.
QList<QString> captionSources(const Draft &d);

// Replaces the draft's auto captions (segments with origin "captions") with
// new ones in text preset `style`, transcribing every caption source.
// Returns the number of caption clips, or -1 with `error` set.
int addCaptions(Draft &d, const QString &style, QString *error = nullptr);

// Removes auto captions. Returns how many were removed.
int removeCaptions(Draft &d);

}  // namespace mtg
