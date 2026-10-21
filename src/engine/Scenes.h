#pragma once

// Scene index: what's in each clip, so the assistant can pick moments the
// way an editor would. Each video is split into shots (cuts found from
// colour-histogram jumps), and every shot gets measurements - sharpness,
// exposure, camera shake, faces - plus Apple's on-device image labels.
// A vision-language model can add a description later (setShotDescriptions).
// Photos are one shot. Results are cached on disk per file.

#include "model/Draft.h"

#include <QImage>
#include <QList>
#include <QString>
#include <QStringList>

namespace mtg {

struct Shot {
    qint64 start = 0, end = 0;  // microseconds in the file
    double sharpness = 0;       // 0..1, relative to the sharpest frames seen
    double exposure = 0;        // 0..1, 1 = well exposed (not dark, not blown out)
    double stability = 0;       // 0..1, 1 = steady camera
    double motion = 0;          // 0..1, how much the picture moves
    int faces = 0;              // most faces in one frame
    QStringList tags;           // on-device labels ("beach", "dog", "food")
    // From a vision-language model; empty until described.
    QString description;
    QString framing;            // wide, medium, close-up, detail
    QStringList subjects;
    int appeal = 0;             // 1-5, 0 = unknown

    // Per-sample quality (0..1) every `sampleUs` from `start`: for picking
    // the best stretch of a given length.
    QList<float> quality;
    qint64 sampleUs = 200'000;

    qint64 length() const { return end - start; }
    // Overall worth for an ad, 0..1: sharp, steady, well exposed, with people
    // or an appealing subject.
    double score() const;
    // The best [from, to) of `length` inside the shot (clamped to the shot).
    QPair<qint64, qint64> bestWindow(qint64 length) const;
};

struct SceneIndex {
    QString path;
    QList<Shot> shots;
    bool described = false;  // every shot has a description
    bool isEmpty() const { return shots.isEmpty(); }
};

// Analyses a video or photo (blocking: about a second per minute of video).
// Cached per file in memory and on disk; thread-safe.
SceneIndex sceneIndex(const Material &m);

// The cached index, without analysing; empty if there is none yet.
SceneIndex cachedSceneIndex(const Material &m);

// Replaces a file's index in the in-memory cache (tests, imports from elsewhere).
void putSceneIndex(const Material &m, const SceneIndex &index);

// Stores descriptions for shots (in order) and caches them.
void setShotDescriptions(const Material &m, const QList<Shot> &described);

// A representative frame for a shot, about `width` pixels wide.
QImage shotFrame(const Material &m, const Shot &s, int width = 448);

// Shots matching words like "the dog running" or "close-up of the product",
// best first, with how well each matches (0..1). `minScore` drops weak ones.
struct ShotMatch {
    int shot = -1;
    double match = 0;
};
QList<ShotMatch> findShots(const SceneIndex &index, const QString &query, double minScore = 0.34);

}  // namespace mtg
