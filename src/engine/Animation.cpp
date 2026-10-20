#include "Animation.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace mtg {

namespace {

constexpr double kPi = std::numbers::pi;

double lerp(double a, double b, double u)
{
    return a + (b - a) * u;
}

double clamp01(double u)
{
    return std::clamp(u, 0.0, 1.0);
}

double outCubic(double u)
{
    return 1 - std::pow(1 - u, 3);
}

double inCubic(double u)
{
    return u * u * u;
}

double outBack(double u)
{
    constexpr double c1 = 1.70158, c3 = c1 + 1;
    return 1 + c3 * std::pow(u - 1, 3) + c1 * std::pow(u - 1, 2);
}

double outBounce(double u)
{
    constexpr double n = 7.5625, d = 2.75;
    if (u < 1 / d) return n * u * u;
    if (u < 2 / d) { u -= 1.5 / d; return n * u * u + 0.75; }
    if (u < 2.5 / d) { u -= 2.25 / d; return n * u * u + 0.9375; }
    u -= 2.625 / d;
    return n * u * u + 0.984375;
}

double outElastic(double u)
{
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    return std::pow(2, -10 * u) * std::sin((u * 10 - 0.75) * (2 * kPi / 3)) + 1;
}

double keyValue(const QList<KeyPoint> &pts, qint64 local)
{
    if (local <= pts.first().t) return pts.first().v;
    if (local >= pts.last().t) return pts.last().v;
    for (qsizetype i = 0; i + 1 < pts.size(); ++i) {
        const KeyPoint &a = pts[i], &b = pts[i + 1];
        if (local >= a.t && local < b.t) {
            const double u = b.t > a.t ? double(local - a.t) / double(b.t - a.t) : 1;
            return lerp(a.v, b.v, ease(b.ease, u));
        }
    }
    return pts.last().v;
}

// Entrance: u goes 0 -> 1 over the "in" duration.
void enter(const QString &name, double u, FrameState &s)
{
    const double e = outCubic(u);
    if (name == QLatin1String("fade")) {
        s.opacity *= e;
    } else if (name == QLatin1String("pop")) {
        s.scale *= lerp(0.4, 1, outBack(u));
        s.opacity *= clamp01(u * 4);
    } else if (name == QLatin1String("slide-up")) {
        s.y += (1 - e) * 0.35;
        s.opacity *= clamp01(u * 2);
    } else if (name == QLatin1String("slide-down")) {
        s.y -= (1 - e) * 0.35;
        s.opacity *= clamp01(u * 2);
    } else if (name == QLatin1String("slide-left")) {
        s.x += (1 - e) * 0.6;
        s.opacity *= clamp01(u * 2);
    } else if (name == QLatin1String("slide-right")) {
        s.x -= (1 - e) * 0.6;
        s.opacity *= clamp01(u * 2);
    } else if (name == QLatin1String("zoom-in")) {
        s.scale *= lerp(0.6, 1, e);
        s.opacity *= e;
    } else if (name == QLatin1String("zoom-out")) {
        s.scale *= lerp(1.4, 1, e);
        s.opacity *= e;
    } else if (name == QLatin1String("spin")) {
        s.rotation += (1 - e) * -180;
        s.scale *= lerp(0.3, 1, e);
        s.opacity *= e;
    } else if (name == QLatin1String("drop")) {
        s.y -= (1 - outBounce(u)) * 0.6;
        s.opacity *= clamp01(u * 4);
    } else if (name == QLatin1String("blur-in")) {
        s.blur = std::max(s.blur, (1 - e) * 24);
        s.opacity *= e;
    } else if (name == QLatin1String("typewriter") || name == QLatin1String("kinetic")) {
        s.textMode = name;
        s.textProgress = u;
    }
}

// Exit: w goes 1 -> 0 over the "out" duration (1 = not leaving yet).
void leave(const QString &name, double w, FrameState &s)
{
    const double g = 1 - inCubic(1 - w);  // accelerates as it leaves
    if (name == QLatin1String("fade")) {
        s.opacity *= w;
    } else if (name == QLatin1String("pop")) {
        s.scale *= lerp(0.4, 1, g);
        s.opacity *= clamp01(w * 4);
    } else if (name == QLatin1String("slide-up")) {
        s.y -= (1 - g) * 0.35;
        s.opacity *= w;
    } else if (name == QLatin1String("slide-down")) {
        s.y += (1 - g) * 0.35;
        s.opacity *= w;
    } else if (name == QLatin1String("slide-left")) {
        s.x -= (1 - g) * 0.6;
        s.opacity *= w;
    } else if (name == QLatin1String("slide-right")) {
        s.x += (1 - g) * 0.6;
        s.opacity *= w;
    } else if (name == QLatin1String("zoom-in")) {
        s.scale *= lerp(1.4, 1, g);
        s.opacity *= w;
    } else if (name == QLatin1String("zoom-out")) {
        s.scale *= lerp(0.6, 1, g);
        s.opacity *= w;
    } else if (name == QLatin1String("spin")) {
        s.rotation += (1 - g) * 180;
        s.scale *= lerp(0.3, 1, g);
        s.opacity *= w;
    } else if (name == QLatin1String("blur-out")) {
        s.blur = std::max(s.blur, (1 - g) * 24);
        s.opacity *= w;
    }
}

void loop(const QString &name, double secs, double speed, double fraction, FrameState &s)
{
    const double p = secs * speed;
    if (name == QLatin1String("pulse")) {
        s.scale *= 1 + 0.05 * std::sin(2 * kPi * p);
    } else if (name == QLatin1String("float")) {
        s.y += 0.025 * std::sin(2 * kPi * p * 0.5);
    } else if (name == QLatin1String("shake")) {
        s.x += 0.012 * std::sin(2 * kPi * p * 13.1);
        s.y += 0.012 * std::cos(2 * kPi * p * 17.3);
    } else if (name == QLatin1String("wiggle")) {
        s.rotation += 5 * std::sin(2 * kPi * p * 1.2);
    } else if (name == QLatin1String("breathe")) {
        s.opacity *= 0.8 + 0.2 * (0.5 + 0.5 * std::sin(2 * kPi * p * 0.5));
    } else if (name == QLatin1String("spin")) {
        s.rotation += 90 * p;
    } else if (name == QLatin1String("ken-burns")) {
        s.scale *= 1 + 0.12 * fraction;
        s.x += 0.04 * fraction;
        s.y -= 0.02 * fraction;
    }
}

}  // namespace

double ease(const QString &name, double u)
{
    u = clamp01(u);
    if (name == QLatin1String("linear")) return u;
    if (name == QLatin1String("ease-in")) return inCubic(u);
    if (name == QLatin1String("ease-out")) return outCubic(u);
    if (name == QLatin1String("back")) return outBack(u);
    if (name == QLatin1String("bounce")) return outBounce(u);
    if (name == QLatin1String("elastic")) return outElastic(u);
    // ease-in-out (default)
    return u < 0.5 ? 4 * u * u * u : 1 - std::pow(-2 * u + 2, 3) / 2;
}

FrameState animate(const Segment &seg, qint64 local)
{
    FrameState s;
    s.x = seg.transform.x;
    s.y = seg.transform.y;
    s.scale = seg.transform.scale;
    s.rotation = seg.transform.rotation;
    s.opacity = seg.opacity;

    // Keyframes set absolute values.
    for (auto it = seg.keys.cbegin(); it != seg.keys.cend(); ++it) {
        if (it.value().isEmpty()) continue;
        const double v = keyValue(it.value(), local);
        const QString &k = it.key();
        if (k == QLatin1String("x")) s.x = v;
        else if (k == QLatin1String("y")) s.y = v;
        else if (k == QLatin1String("scale")) s.scale = v;
        else if (k == QLatin1String("rotation")) s.rotation = v;
        else if (k == QLatin1String("opacity")) s.opacity = v;
    }

    // Motions modify on top, never longer than half the clip each.
    const Motion &m = seg.motion;
    const qint64 half = std::max<qint64>(1, seg.duration / 2);
    if (!m.in.isEmpty()) {
        const qint64 d = std::clamp<qint64>(m.inUs, 1, half);
        enter(m.in, clamp01(double(local) / double(d)), s);
    }
    if (!m.out.isEmpty()) {
        const qint64 d = std::clamp<qint64>(m.outUs, 1, half);
        leave(m.out, clamp01(double(seg.duration - local) / double(d)), s);
    }
    if (!m.loop.isEmpty())
        loop(m.loop, local / 1e6, m.loopSpeed, seg.duration > 0 ? clamp01(double(local) / double(seg.duration)) : 0, s);

    s.opacity = clamp01(s.opacity);
    s.scale = std::max(0.0, s.scale);
    return s;
}

QStringList motionNames(const QString &slot)
{
    if (slot == QLatin1String("in"))
        return {"fade", "pop", "slide-up", "slide-down", "slide-left", "slide-right", "zoom-in", "zoom-out",
                "spin", "drop", "blur-in", "typewriter", "kinetic"};
    if (slot == QLatin1String("out"))
        return {"fade", "pop", "slide-up", "slide-down", "slide-left", "slide-right", "zoom-in", "zoom-out",
                "spin", "blur-out"};
    return {"pulse", "float", "shake", "wiggle", "breathe", "spin", "ken-burns"};
}

QStringList easeNames()
{
    return {"linear", "ease-in", "ease-out", "ease-in-out", "back", "bounce", "elastic"};
}

}  // namespace mtg
