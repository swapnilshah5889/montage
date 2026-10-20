#pragma once

// Ad styles: one word from the model ("energetic", "luxury", ...) and the app
// designs the rest consistently - transitions on every cut, a colour look,
// cut-synced punch-ins, Ken Burns on photos and text animation - wherever the
// plan left a choice open. Explicit choices in the plan always win.

#include "model/Draft.h"

#include <QJsonObject>
#include <QStringList>

namespace mtg::ai {

class MediaIndex;

QStringList adStyleNames();  // energetic, bold, playful, elegant, luxury, minimal

// Applies `plan["style"]` to the plan. `before` is the current timeline as a
// plan (planFromDraft of `d`). When the style changes, choices that came from
// the old style are removed first and the whole edit is restyled; when it
// stays, only clips and texts that are new get the treatment, so things the
// user removed ("no transitions") stay removed. "none" or [] in the plan is an
// explicit choice and is kept.
// Returns notes for the chat ("applied the energetic style").
QStringList applyAdStyle(const QJsonObject &before, const Draft &d, const MediaIndex &index, QJsonObject *plan);

}  // namespace mtg::ai
