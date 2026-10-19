#pragma once

#include "model/Draft.h"

#include <functional>

namespace mtg {

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    QString message;
    QString videoEncoder;
};

// Renders the whole draft to an H.264/AAC MP4 at canvas resolution.
// `progress` receives 0..1 and returns false to cancel.
ExportResult exportDraft(const Draft &draft, const QString &path,
                         const std::function<bool(double)> &progress);

}  // namespace mtg
