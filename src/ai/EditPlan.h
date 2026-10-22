#pragma once

// The edit plan: a declarative JSON description of the whole timeline, in
// seconds, with named positions instead of raw coordinates. Models read the
// current project as a plan and answer with a revised plan; the app
// validates it and rebuilds the timeline from it in one undo step.
//
// {
//   "reply":  "short message for the user",
//   "aspect": "keep" | "original" | "16:9" | "9:16" | "1:1" | "4:5" | "4:3" | "3:4" | "21:9",
//   "main":     [{"media": "m1", "from": 0, "to": 4.5, "volume": 1}],
//   "overlays": [{"media": "m2", "at": 1, "from": 0, "to": 3, "position": "top-right", "scale": 0.4}],
//   "audio":    [{"media": "m3", "at": 0, "from": 0, "to": 12, "volume": 0.3}],
//   "texts":    [{"text": "Hello", "at": 0, "duration": 3, "style": "title", "position": "center"}],
//   "generate": [{"kind": "image", "prompt": "candles on a table", "as": "g1"}],  (made first; see Generators.h)
//   "export": false
// }

#include "model/Draft.h"

#include <QJsonObject>
#include <QStringList>

namespace mtg::ai {

// Short media ids for prompts ("m1", "m2", ...), in material order.
class MediaIndex
{
public:
    explicit MediaIndex(const Draft &d);
    QString alias(const QString &materialId) const;
    QString materialId(const QString &alias) const;
    const QStringList &materialIds() const { return m_ids; }

private:
    QStringList m_ids;
};

struct PlanReport {
    QStringList errors;    // plan rejected; tell the model to fix these
    QStringList warnings;  // plan applied with adjustments
    bool ok() const { return errors.isEmpty(); }
    QString text() const;
};

// The scene index of a file as one indented line ("    shots: 1) 0-3s waves
// on a beach (wide, best); 2) ..."), or empty when it hasn't been built.
QString describeShots(const Material &m);

// What's said in a file as numbered lines ("    says: 1) 0-2s \"Hi, I'm
// Sam.\"; ..."), or empty when it hasn't been transcribed or nobody speaks.
QString describeSpeech(const Material &m);

// One line per media file: id, kind, name, length, size, sound, plus its shots and lines.
QString describeMedia(const Draft &d, const MediaIndex &index);

// Compact human-readable timeline summary (what the model gets back after an edit).
QString summarize(const Draft &d, const MediaIndex &index);

// The current timeline as a plan. Only non-default values are written, so
// small models see a short document.
QJsonObject planFromDraft(const Draft &d, const MediaIndex &index);

// Builds the timeline a plan describes on top of `current` (media and other
// settings are kept). Lenient about number formats; strict about media ids
// and ranges.
Draft draftFromPlan(const Draft &current, const QJsonObject &plan, const MediaIndex &index, PlanReport *report);

// The same edit in another shape ("9:16", "1:1", "4:5", "16:9"...): texts and
// overlays re-anchor to their named positions, clips reframe (auto framing).
Draft resizedDraft(const Draft &d, const QString &aspect);

// Shapes a video can be exported in at once.
QStringList exportFormatNames();  // 9:16, 4:5, 1:1, 16:9

// JSON Schema for plans; portable across Anthropic, OpenAI and Ollama.
QJsonObject planSchema();

// How long a "main" item plays on the timeline, in seconds: its range over
// its speed, a freeze frame's "duration", less any "skips". NaN if the
// range is broken.
double itemSeconds(const QJsonObject &item);

// Parses "12", "12.5", "1:30" or a JSON number into seconds; NaN if invalid.
double seconds(const QJsonValue &v);

}  // namespace mtg::ai
