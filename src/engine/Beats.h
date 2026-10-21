#pragma once

// Beat tracking for cutting to music: onset strength from spectral flux,
// tempo from its autocorrelation, then beats by dynamic programming (Ellis,
// "Beat Tracking by Dynamic Programming", 2007). Bars assume 4/4 and start on
// the beat with the most low-end (kick drum) energy.

#include "model/Draft.h"

#include <QList>
#include <QString>

namespace mtg {

struct BeatGrid {
    double bpm = 0;
    QList<double> beats;      // seconds from the start of the file
    QList<double> downbeats;  // first beat of each bar
    QList<double> accents;    // the strongest hits (drops, impacts), at least a second apart
    bool isEmpty() const { return beats.size() < 2; }
};

// Analyses a file's audio (the first ten minutes). Cached per file for the
// process; thread-safe.
BeatGrid beatGrid(const Material &m);

QStringList beatSyncNames();  // beats, bars

// Moves the cuts between main-track clips onto the beats ("beats") or bar
// lines ("bars") of audio segment `musicId`, keeping the video's length and
// each clip inside its file. Texts, overlays and effect moments that started
// (or ended) at a cut move with it. Returns how many cuts moved.
int snapCutsToBeats(Draft &d, const QString &musicId, const QString &mode);

// The audio segment to cut to: the longest one on an audio track whose file
// isn't a voiceover (by name), or empty.
QString musicSegment(const Draft &d);

}  // namespace mtg
