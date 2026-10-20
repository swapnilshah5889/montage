#pragma once

// Deterministic guards applied to a model's edit plan before it is built.
// Small models are unreliable at exactly these things, and re-asking them
// rarely helps, so the app does them itself:
//  - keep parts of the timeline the request didn't ask to change
//  - hit a requested total length exactly

#include "EditPlan.h"

#include <QJsonObject>
#include <QStringList>

namespace mtg::ai {

// Total video length the request asks for ("a 12s tiktok", "make the video
// 20 seconds"), or 0 if it doesn't name one.
double requestedLength(const QString &request);

// Copies sections of `current` back into `plan` when the request shows no
// intent to change them (e.g. adding a title keeps clips, music and aspect).
// Returns a note per restored section.
QStringList keepUnrequested(const QString &request, const QJsonObject &current, const Draft &d,
                            const MediaIndex &index, QJsonObject *plan);

// Colour named for the background ("on a dark blue background") as #RRGGBB,
// or empty if the request doesn't name one.
QString requestedBackground(const QString &request);

// Parses a colour the way people and small models write it: "#1e3a8a",
// "navy", "dark blue". Empty if it isn't a colour.
QString parseColor(const QString &text);

// Scales main clip lengths so they add up to `seconds`, keeping each clip
// inside its file; music and texts that ended with the video still do.
// Returns false if nothing needed changing.
bool fitToLength(double seconds, const Draft &d, const MediaIndex &index, QJsonObject *plan);

// A new ad (the plan sets a style and there was none) with no music gets the
// first audio file under it, unless the request says otherwise. Returns true
// if music was added.
bool addAdMusic(const QString &request, const QJsonObject &current, const Draft &d, const MediaIndex &index,
                QJsonObject *plan);

// Applies what the request asks for when its target is unambiguous and the
// model left it out: a slow zoom on "the photo(s)", a look on "the whole
// video". Returns a note per change.
QStringList applyObviousFeatures(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan);

// Shapes to export when the request asks for several platforms at once
// ("export it for TikTok and YouTube", "all formats"); empty otherwise.
QStringList requestedFormats(const QString &request);

// New texts meant for the end ("add a Shop now button at the end") that stop
// short of it are moved to end with the video. Returns true if any moved.
bool snapEndTexts(const QString &request, const QJsonObject &current, QJsonObject *plan);

// Effects and speed changes the request names that the plan doesn't contain
// ("add a flash", "slow motion", "black and white"), each as an instruction
// the model can follow, with exact JSON and the cut times.
QStringList missingFeatures(const QString &request, const QJsonObject &plan);

}  // namespace mtg::ai
