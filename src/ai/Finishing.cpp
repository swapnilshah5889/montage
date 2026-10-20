#include "Finishing.h"

#include "engine/Beats.h"
#include "engine/Speech.h"

namespace mtg::ai {

bool needsFinishing(const Draft &d)
{
    return !d.beatSync.isEmpty() || !d.captions.isEmpty();
}

QStringList finishDraft(Draft &d, QStringList *problems)
{
    QStringList notes;
    if (!d.beatSync.isEmpty()) {
        const QString music = musicSegment(d);
        if (music.isEmpty()) {
            // Nothing to cut to yet; the setting stays for when music arrives.
        } else if (const int moved = snapCutsToBeats(d, music, d.beatSync); moved > 0) {
            notes << QStringLiteral("cut to the %1 of the music (%2 cut%3 moved)")
                         .arg(d.beatSync == QLatin1String("bars") ? QStringLiteral("bars") : QStringLiteral("beat"))
                         .arg(moved)
                         .arg(moved == 1 ? "" : "s");
        }
    }
    if (!d.captions.isEmpty()) {
        QString error;
        const int n = addCaptions(d, d.captions, &error);
        if (n > 0) notes << QStringLiteral("captioned what's said (%1 caption%2)").arg(n).arg(n == 1 ? "" : "s");
        if (!error.isEmpty() && problems) *problems << error;
    } else {
        removeCaptions(d);
    }
    return notes;
}

}  // namespace mtg::ai
