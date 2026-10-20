#pragma once

// The project document. Same idea as CapCut's draft: media lives in a
// material table and the timeline only holds segments that point at a
// material by id, with a timeline range and a source offset. All times are
// microseconds.

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

namespace mtg {

enum class MaterialKind { Video, Audio, Image, Text, Effect };
enum class TrackRole { Main, Overlay, Audio };

QString kindName(MaterialKind k);
QString roleName(TrackRole r);

constexpr qint64 kImageDefaultUs = 5'000'000;
constexpr qint64 kTextDefaultUs = 3'000'000;
inline const QString kTextMaterialId = QStringLiteral("text");
inline const QString kEffectMaterialId = QStringLiteral("fx");  // adjustment layers
constexpr qint64 kMinSegmentUs = 100'000;

struct Material {
    QString id;
    MaterialKind kind = MaterialKind::Video;
    QString path;
    QString name;
    qint64 durationUs = 0;  // 0 for still images
    int width = 0;          // display size, rotation applied
    int height = 0;
    double fps = 0;
    bool hasVideo = false;
    bool hasAudio = false;
    QString thumbnail;      // cached PNG

    bool isVisual() const { return kind != MaterialKind::Audio; }
    bool isTimed() const { return kind == MaterialKind::Video || kind == MaterialKind::Audio; }
};

// Styling for text clips. Sizes are relative to the canvas so text keeps
// its look at any resolution.
struct TextStyle {
    QString text;
    QString font = QStringLiteral("Helvetica Neue");
    double size = 0.08;      // font size as a fraction of the canvas's shorter side
    QString color = QStringLiteral("#ffffff");
    bool bold = true;
    double outline = 0.0;    // outline width as a fraction of the font size
    QString outlineColor = QStringLiteral("#000000");
    QString background;      // box colour behind the text; empty = none
    QString align = QStringLiteral("center");  // left | center | right
    QString shadow;          // drop-shadow colour; empty = none
    QString gradient;        // second fill colour, top to bottom; empty = solid
    QString shape;           // box shape: rounded (default), pill, circle, burst, tag, ribbon, underline

    bool operator==(const TextStyle &) const = default;
};

struct Transform {
    double x = 0;         // offset as a fraction of half the canvas width
    double y = 0;         // offset as a fraction of half the canvas height
    double scale = 1;     // 1 = fit inside the canvas
    double rotation = 0;  // degrees, clockwise

    bool operator==(const Transform &) const = default;
};

// One keyframe of an animated property, in the clip's own time (0 = clip
// start). `ease` shapes the move from the previous key to this one.
struct KeyPoint {
    qint64 t = 0;
    double v = 0;
    QString ease = QStringLiteral("ease-in-out");  // linear | ease-in | ease-out | ease-in-out | back | bounce | elastic

    bool operator==(const KeyPoint &) const = default;
};

// Ready-made movement, the way CapCut and Premiere templates do it: an
// entrance, an exit and a loop. Names are resolved by the animation engine.
struct Motion {
    QString in;                // fade, pop, slide-up/down/left/right, zoom-in, zoom-out, spin, drop, blur-in, typewriter, kinetic
    qint64 inUs = 500'000;
    QString out;               // fade, pop, slide-up/down/left/right, zoom-in, zoom-out, spin, blur-out
    qint64 outUs = 400'000;
    QString loop;              // pulse, float, shake, wiggle, breathe, spin, ken-burns
    double loopSpeed = 1.0;    // cycles per second multiplier

    bool isEmpty() const { return in.isEmpty() && out.isEmpty() && loop.isEmpty(); }
    bool operator==(const Motion &) const = default;
};

// Transition from the previous main-track clip into this one, centred on the cut.
struct Transition {
    QString name;              // crossfade, fade-black, fade-white, push-left/right/up/down, slide-left/right/up/down,
                               // zoom, whip, spin, glitch, flash, wipe, circle, blur
    qint64 us = 500'000;

    bool operator==(const Transition &) const = default;
};
QStringList transitionNames();
QStringList speedCurveNames();
QStringList framingNames();  // auto, fill, fit, fit-blur

// Animatable properties: x, y, scale, rotation, opacity.
using KeyTracks = QMap<QString, QList<KeyPoint>>;

// A named GPU effect or look. `amount` is strength (1 = the preset's
// designed look; some adjustments take -1..1). `path` is for LUT files.
struct Effect {
    QString name;
    double amount = 1.0;
    QString path;

    bool operator==(const Effect &) const = default;
};

struct Segment {
    QString id;
    QString materialId;
    qint64 start = 0;        // position on the timeline
    qint64 duration = 0;     // length on the timeline
    qint64 sourceStart = 0;  // offset into the material
    double volume = 1.0;
    double opacity = 1.0;
    Transform transform;
    TextStyle text;          // used when the material is the text material
    Motion motion;
    KeyTracks keys;
    QList<Effect> effects;   // on a clip: applied to it; on an adjustment layer: to everything beneath
    Transition transition;   // main track only: how this clip comes in from the previous one
    double speed = 1.0;      // average playback speed; 0 = freeze frame at sourceStart
    QString curve;           // speed ramp: ramp-up, ramp-down, hero, bullet; empty = constant
    QString framing = QStringLiteral("auto");  // main track: auto (fill, follow the subject), fill, fit, fit-blur
    bool speech = false;     // someone talking (voiceover, interview): music with `duck` dips under it
    double duck = 0;         // music: volume multiplier while someone speaks (0 = no ducking)
    QString origin;          // what made it, when the app did: "captions" for auto captions

    // Source time shown at `local` (time since the clip started). Handles
    // speed, ramps and freezes; extrapolates past the edges for transitions.
    qint64 sourceAt(qint64 local) const;
    // How much of the source the clip plays.
    qint64 sourceSpan() const { return qint64(double(duration) * speed); }

    qint64 end() const { return start + duration; }
    bool contains(qint64 t) const { return t >= start && t < end(); }
};

struct Track {
    QString id;
    TrackRole role = TrackRole::Main;
    QList<Segment> segments;  // sorted by start, never overlapping

    bool isVisual() const { return role != TrackRole::Audio; }
    bool fits(qint64 start, qint64 end, const QString &ignoreId = {}) const;
};

struct Canvas {
    int width = 1920;
    int height = 1080;
    double fps = 30;
    QString background = QStringLiteral("#000000");  // shows wherever no clip covers the frame
};

// Applies a named text preset: style and position, plus its default motion
// when `motion` is given. Keeps the text. False for unknown names.
bool applyTextPreset(const QString &name, TextStyle *style, Transform *transform, Motion *motion = nullptr);
QStringList textPresetNames();
QStringList textShapeNames();

struct Draft {
    QString name = QStringLiteral("Untitled");
    Canvas canvas;
    bool canvasLocked = false;  // false: follow the first clip's shape
    QString style;              // ad style the assistant designs with ("energetic", ...); empty = none
    QString beatSync;           // "beats" or "bars": the assistant cuts to the music; empty = off
    QString captions;           // text preset for auto captions of what's said; empty = none
    QList<Material> materials;
    QList<Track> tracks;        // tracks[0] is always the main track

    Draft();

    qint64 duration() const;
    const Material *material(const QString &id) const;
    Track *track(const QString &id);
    Segment *segment(const QString &id, Track **owner = nullptr);
    const Segment *segment(const QString &id, const Track **owner = nullptr) const;
    Track &mainTrack() { return tracks[0]; }

    // Text clips and adjustment layers share built-in materials; these add them on first use.
    void ensureTextMaterial();
    void ensureEffectMaterial();

    // Re-establish invariants after an edit: the main track is magnetic
    // (no gaps), other tracks are sorted, and empty extra tracks go away.
    void normalize();

    QJsonObject toJson() const;
    static Draft fromJson(const QJsonObject &obj);

    static QString newId();
};

}  // namespace mtg
