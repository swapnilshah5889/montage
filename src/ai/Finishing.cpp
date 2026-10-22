#include "Finishing.h"

#include "engine/Beats.h"
#include "engine/Speech.h"
#include "engine/TextEdit.h"

namespace mtg::ai {

bool needsFinishing(const Draft &d)
{
    return !d.beatSync.isEmpty() || !d.captions.isEmpty() || d.cleanSpeech;
}

QStringList finishDraft(Draft &d, QStringList *problems)
{
    QStringList notes;
    if (d.cleanSpeech) {  // first: cutting changes where everything else lands
        for (const Segment &s : d.mainTrack().segments)
            if (const Material *m = d.material(s.materialId); m && m->hasAudio) fileTranscript(*m);
        int fillers = 0, pauses = 0;
        const qint64 removed = cleanSpeech(d, {}, &fillers, &pauses);
        if (removed > 0) {
            QStringList what;
            if (fillers) what << QStringLiteral("%1 filler%2").arg(fillers).arg(fillers == 1 ? "" : "s");
            if (pauses) what << QStringLiteral("%1 long pause%2").arg(pauses).arg(pauses == 1 ? "" : "s");
            notes << QStringLiteral("cut %1 (%2s shorter)").arg(what.join(QStringLiteral(" and "))).arg(removed / 1e6, 0, 'f', 1);
        }
    }
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
