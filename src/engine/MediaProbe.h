#pragma once

#include "model/Draft.h"

namespace mtg {

// Fills a Material from a media file (kind, duration, size, streams) and
// writes a thumbnail into `thumbDir`.
bool probeMedia(const QString &path, const QString &thumbDir, Material *out, QString *error);

}  // namespace mtg
