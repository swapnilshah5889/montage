#pragma once

#include "model/Draft.h"

#include <QImage>
#include <QStringList>

namespace mtg {

// GPU image effects (Core Image on Metal). One processor per render thread.
class EffectProcessor
{
public:
    EffectProcessor();
    ~EffectProcessor();
    EffectProcessor(const EffectProcessor &) = delete;
    EffectProcessor &operator=(const EffectProcessor &) = delete;

    // Clip effects: applied to one clip's frame. `local` is seconds since the
    // clip started; sizes are tuned for 1080p and scaled to the image.
    QImage apply(const QImage &frame, const QList<Effect> &effects, double local, bool transparentLayer = false);

    // Effect moments (adjustment layers): applied to everything beneath, with
    // `progress` 0..1 through the moment.
    QImage applyMoment(const QImage &canvas, const QList<Effect> &effects, double local, double progress);

private:
    struct Impl;
    Impl *d;
};

QStringList effectNames();  // clip effects and looks
QStringList momentNames();  // effects for adjustment layers (also accept effectNames())

}  // namespace mtg
