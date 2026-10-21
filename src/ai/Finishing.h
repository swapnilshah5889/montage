#pragma once

// Finishing: steps that need the media itself and run after a plan is
// built - moving cuts onto the music's beats and writing auto captions from
// what's said. Slow the first time (audio analysis, transcription; both are
// cached), so the app runs them off the UI thread.

#include "model/Draft.h"

#include <QStringList>

namespace mtg::ai {

// Whether `d` asks for any finishing step.
bool needsFinishing(const Draft &d);

// Runs the steps `d` asks for. Returns notes for the chat; problems (no
// whisper installed, nothing to caption) go to `problems`.
QStringList finishDraft(Draft &d, QStringList *problems);

}  // namespace mtg::ai
