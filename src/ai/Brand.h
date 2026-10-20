#pragma once

// Brand kit: colours, font, logo and a house style. When it is on (or the
// request mentions the brand), the app applies it to what the assistant
// makes, the same way every time: brand colours on buttons, badges and
// lower thirds, the brand font on new text, the logo as a watermark, and the
// house ad style. Models never have to remember any of it.

#include "model/Draft.h"

#include <QJsonObject>
#include <QStringList>
#include <QVariantMap>

namespace mtg::ai {

class MediaIndex;

struct BrandKit {
    QString name;
    QString primary;                           // buttons, lower thirds, text-only backgrounds
    QString accent;                            // badges and prices; falls back to primary
    QString text = QStringLiteral("#FFFFFF");  // headline and title colour
    QString font;
    QString logo;                              // image file
    QString logoPosition = QStringLiteral("top-right");
    QString style;                             // ad style, e.g. "bold"
    bool enabled = false;                      // apply to every edit, not only when asked

    bool isEmpty() const { return primary.isEmpty() && font.isEmpty() && logo.isEmpty() && style.isEmpty(); }
    QVariantMap toMap() const;
    static BrandKit fromMap(const QVariantMap &m);
};

// The kit saved in the app settings.
BrandKit loadBrandKit();
void saveBrandKit(const BrandKit &kit);

// Whether the kit applies to this request: it's on, or the request asks for it.
bool brandApplies(const QString &request, const BrandKit &kit);

// Applies the kit to `plan` (see the top of this file). `before` is the current
// timeline as a plan; only new texts are recoloured unless the request names
// the brand. The logo must already be imported (its material path equals
// kit.logo). Returns notes for the chat.
QStringList applyBrand(const QString &request, const BrandKit &kit, const QJsonObject &before, const Draft &d,
                       const MediaIndex &index, QJsonObject *plan);

}  // namespace mtg::ai
