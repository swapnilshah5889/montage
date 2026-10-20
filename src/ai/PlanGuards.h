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

// Smart-edit requests the app can honour directly: cutting to the beat
// ("beat_sync"), auto captions of what's said ("captions"), lowering music
// while someone talks ("duck"), and removing a clip's background ("cutout"
// effect). Returns a note per change.
QStringList applySmartRequests(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan);

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

// ---- Generated media (see Generators.h)

// Makes the plan's "generate" list match the request: items it didn't ask for
// are dropped (generation is slow, and small models add it freely); a
// requested voiceover, image or music the plan lacks is added - the voiceover
// with the user's quoted words (or the plan's texts), music only when there's
// no audio file to use. Returns a note per change.
QStringList requestedGeneration(const QString &request, const QJsonObject &current, const Draft &d, const MediaIndex &index,
                                QJsonObject *plan);

// The words a requested voiceover should say: the quote next to "voiceover",
// "narration"..., or the request's only quote. Empty if there's none.
QString voiceoverScript(const QString &request);

// Generated media the plan doesn't use yet goes where it obviously belongs:
// an image becomes the picture (or the next clip), a voiceover starts with
// the text it reads (else at 0.5s), music runs under the whole video.
// `generated` are media aliases. Returns a note per item placed.
QStringList placeGenerated(const QStringList &generated, const QString &request, const Draft &d, const MediaIndex &index,
                           QJsonObject *plan);

// Marks voiceovers as speech and makes music dip to about 0.25 while the
// voice talks (keeping its level in between), unless the request is about
// volume. Returns true if anything changed.
bool duckUnderVoiceover(const QString &request, const Draft &d, const MediaIndex &index, QJsonObject *plan);

// For an ad with no music and no audio file to use: a line offering to
// compose a track. Empty otherwise.
QString musicOffer(const QString &request, const QJsonObject &plan, const Draft &d, const MediaIndex &index);

}  // namespace mtg::ai
