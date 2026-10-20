#pragma once

// Auto Reframe: keeps the subject in frame when a clip fills a canvas of a
// different shape (landscape footage in a vertical ad). Faces lead when there
// are any, otherwise Vision's attention saliency - where a viewer looks.
// The subject is tracked across the clip and smoothed so the crop glides
// rather than jitters.

#include "model/Draft.h"

#include <QImage>
#include <QPointF>
#include <QStringList>

namespace mtg {

// Centre of attention in `frame`, 0..1 from the top-left. (0.5, 0.5) if
// nothing stands out.
QPointF subjectCenter(const QImage &frame);

// Subject position for a clip at source time `sourceUs`. Analyses the clip's
// range of the file on first use (a few frames) and caches the track for the
// whole process, so preview and export share it. Thread-safe.
QPointF subjectAt(const Material &mat, qint64 sourceStart, qint64 sourceSpan, qint64 sourceUs);

}  // namespace mtg
