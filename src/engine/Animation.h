#pragma once

#include "model/Draft.h"

#include <QStringList>

namespace mtg {

// How a clip looks at one moment, after keyframes, entrance/exit motions
// and loops are applied.
struct FrameState {
    double x = 0;
    double y = 0;
    double scale = 1;
    double rotation = 0;
    double opacity = 1;
    double blur = 0;           // blur radius in pixels at 1080p (blur-in/out)
    QString textMode;          // "", "typewriter" or "kinetic"
    double textProgress = 1;   // 0..1 through the text animation
};

// Easing curve value for progress u in [0, 1].
double ease(const QString &name, double u);

// State of `seg` at `local` microseconds after it starts.
FrameState animate(const Segment &seg, qint64 local);

// Names the engine understands, for UIs, prompts and validation.
QStringList motionNames(const QString &slot);  // "in", "out" or "loop"
QStringList easeNames();

}  // namespace mtg
