#include "Draft.h"

#include <QJsonArray>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace mtg {

QString kindName(MaterialKind k)
{
    switch (k) {
    case MaterialKind::Video: return QStringLiteral("video");
    case MaterialKind::Audio: return QStringLiteral("audio");
    case MaterialKind::Image: return QStringLiteral("image");
    case MaterialKind::Text: return QStringLiteral("text");
    case MaterialKind::Effect: return QStringLiteral("effect");
    }
    return {};
}

QString roleName(TrackRole r)
{
    switch (r) {
    case TrackRole::Main: return QStringLiteral("main");
    case TrackRole::Overlay: return QStringLiteral("overlay");
    case TrackRole::Audio: return QStringLiteral("audio");
    }
    return {};
}

static MaterialKind kindFromName(const QString &s)
{
    if (s == QLatin1String("audio")) return MaterialKind::Audio;
    if (s == QLatin1String("image")) return MaterialKind::Image;
    if (s == QLatin1String("text")) return MaterialKind::Text;
    if (s == QLatin1String("effect")) return MaterialKind::Effect;
    return MaterialKind::Video;
}

static TrackRole roleFromName(const QString &s)
{
    if (s == QLatin1String("overlay")) return TrackRole::Overlay;
    if (s == QLatin1String("audio")) return TrackRole::Audio;
    return TrackRole::Main;
}

bool Track::fits(qint64 s, qint64 e, const QString &ignoreId) const
{
    for (const Segment &seg : segments) {
        if (seg.id == ignoreId) continue;
        if (s < seg.end() && seg.start < e) return false;
    }
    return true;
}

Draft::Draft()
{
    Track main;
    main.id = newId();
    main.role = TrackRole::Main;
    tracks.append(main);
}

QString Draft::newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(13);
}

void Draft::ensureTextMaterial()
{
    if (material(kTextMaterialId)) return;
    Material m;
    m.id = kTextMaterialId;
    m.kind = MaterialKind::Text;
    m.name = QStringLiteral("Text");
    m.hasVideo = true;
    materials.append(m);
}

void Draft::ensureEffectMaterial()
{
    if (material(kEffectMaterialId)) return;
    Material m;
    m.id = kEffectMaterialId;
    m.kind = MaterialKind::Effect;
    m.name = QStringLiteral("Effect");
    m.hasVideo = true;
    materials.append(m);
}

static QJsonArray effectsToJson(const QList<Effect> &fx)
{
    QJsonArray a;
    for (const Effect &e : fx) {
        QJsonObject o{{"name", e.name}, {"amount", e.amount}};
        if (!e.path.isEmpty()) o.insert("path", e.path);
        a.append(o);
    }
    return a;
}

static QList<Effect> effectsFromJson(const QJsonArray &a)
{
    QList<Effect> fx;
    for (const QJsonValue &v : a) {
        const QJsonObject o = v.toObject();
        if (o.value("name").toString().isEmpty()) continue;
        fx.append(Effect{o.value("name").toString(), o.value("amount").toDouble(1.0), o.value("path").toString()});
    }
    return fx;
}

static QJsonObject motionToJson(const Motion &m)
{
    QJsonObject o;
    if (!m.in.isEmpty()) o.insert("in", QJsonObject{{"name", m.in}, {"duration", double(m.inUs)}});
    if (!m.out.isEmpty()) o.insert("out", QJsonObject{{"name", m.out}, {"duration", double(m.outUs)}});
    if (!m.loop.isEmpty()) o.insert("loop", QJsonObject{{"name", m.loop}, {"speed", m.loopSpeed}});
    return o;
}

static Motion motionFromJson(const QJsonObject &o)
{
    Motion m;
    m.in = o.value("in").toObject().value("name").toString();
    m.inUs = qint64(o.value("in").toObject().value("duration").toDouble(500'000));
    m.out = o.value("out").toObject().value("name").toString();
    m.outUs = qint64(o.value("out").toObject().value("duration").toDouble(400'000));
    m.loop = o.value("loop").toObject().value("name").toString();
    m.loopSpeed = o.value("loop").toObject().value("speed").toDouble(1.0);
    return m;
}

static QJsonObject keysToJson(const KeyTracks &k)
{
    QJsonObject o;
    for (auto it = k.cbegin(); it != k.cend(); ++it) {
        QJsonArray pts;
        for (const KeyPoint &p : it.value())
            pts.append(QJsonObject{{"t", double(p.t)}, {"v", p.v}, {"ease", p.ease}});
        o.insert(it.key(), pts);
    }
    return o;
}

static KeyTracks keysFromJson(const QJsonObject &o)
{
    KeyTracks k;
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        QList<KeyPoint> pts;
        for (const QJsonValue &v : it.value().toArray()) {
            const QJsonObject p = v.toObject();
            pts.append(KeyPoint{qint64(p.value("t").toDouble()), p.value("v").toDouble(),
                                p.value("ease").toString(QStringLiteral("ease-in-out"))});
        }
        std::sort(pts.begin(), pts.end(), [](const KeyPoint &a, const KeyPoint &b) { return a.t < b.t; });
        if (!pts.isEmpty()) k.insert(it.key(), pts);
    }
    return k;
}

static QJsonObject textToJson(const TextStyle &t)
{
    return QJsonObject{
        {"shadow", t.shadow},
        {"shape", t.shape},
        {"gradient", t.gradient},
        {"text", t.text},
        {"font", t.font},
        {"size", t.size},
        {"color", t.color},
        {"bold", t.bold},
        {"outline", t.outline},
        {"outline_color", t.outlineColor},
        {"background", t.background},
        {"align", t.align},
    };
}

static TextStyle textFromJson(const QJsonObject &o)
{
    TextStyle t;
    t.text = o.value("text").toString();
    t.font = o.value("font").toString(t.font);
    t.size = o.value("size").toDouble(t.size);
    t.color = o.value("color").toString(t.color);
    t.bold = o.value("bold").toBool(t.bold);
    t.outline = o.value("outline").toDouble(t.outline);
    t.outlineColor = o.value("outline_color").toString(t.outlineColor);
    t.background = o.value("background").toString();
    t.align = o.value("align").toString(t.align);
    t.shadow = o.value("shadow").toString();
    t.shape = o.value("shape").toString();
    t.gradient = o.value("gradient").toString();
    return t;
}

// Starting looks for text clips: style plus where it sits on the canvas.
QStringList framingNames()
{
    return {"auto", "fill", "fit", "fit-blur"};
}

QStringList speedCurveNames()
{
    return {"ramp-up", "ramp-down", "hero", "bullet"};
}

namespace {

// Relative speed over the clip (u in 0..1) for each ramp; integrated and
// normalised so the clip still plays exactly its source span.
double rampSpeed(const QString &curve, double u)
{
    if (curve == QLatin1String("ramp-up")) return 0.3 + 1.7 * u;
    if (curve == QLatin1String("ramp-down")) return 2.0 - 1.7 * u;
    if (curve == QLatin1String("hero")) return 0.2 + 2.2 * std::pow(2 * u - 1, 2);
    if (curve == QLatin1String("bullet")) return 1.0 - 0.9 * std::exp(-std::pow((u - 0.5) / 0.1, 2));
    return 1.0;
}

// Fraction of the source span played by u (0..1).
double rampProgress(const QString &curve, double u)
{
    constexpr int kSteps = 128;
    double done = 0, total = 0;
    for (int i = 0; i < kSteps; ++i) {
        const double a = double(i) / kSteps, b = double(i + 1) / kSteps;
        const double area = (rampSpeed(curve, a) + rampSpeed(curve, b)) / 2 / kSteps;
        total += area;
        if (b <= u) done += area;
        else if (a < u) done += area * (u - a) * kSteps;
    }
    return total > 0 ? done / total : u;
}

}  // namespace

qint64 Segment::sourceAt(qint64 local) const
{
    if (speed <= 0) return sourceStart;  // freeze frame
    if (curve.isEmpty() || duration <= 0 || local < 0 || local > duration)
        return sourceStart + qint64(double(local) * speed);
    return sourceStart + qint64(double(sourceSpan()) * rampProgress(curve, double(local) / double(duration)));
}

QStringList transitionNames()
{
    return {"crossfade", "fade-black", "fade-white", "push-left", "push-right", "push-up", "push-down", "slide-left",
            "slide-right", "slide-up", "slide-down", "zoom", "whip", "spin", "glitch", "flash", "wipe", "circle", "blur"};
}

QStringList textPresetNames()
{
    return {"title", "subtitle", "caption", "lower-third", "end-card", "headline", "cta", "price", "badge", "kinetic-caption"};
}

QStringList textShapeNames()
{
    return {"rounded", "pill", "circle", "burst", "tag", "ribbon", "underline"};
}

bool applyTextPreset(const QString &name, TextStyle *st, Transform *tr, Motion *motion)
{
    const QString text = st->text;
    TextStyle base;
    Transform pos;
    Motion move;
    if (name == QLatin1String("title")) {
        base.size = 0.11;
        base.outline = 0.05;
    } else if (name == QLatin1String("subtitle")) {
        base.size = 0.06;
        base.bold = false;
        base.outline = 0.04;
        pos.y = 0.3;
    } else if (name == QLatin1String("caption")) {
        base.size = 0.055;
        base.background = QStringLiteral("#B3000000");
        pos.y = 0.78;
    } else if (name == QLatin1String("lower-third")) {
        base.size = 0.05;
        base.align = QStringLiteral("left");
        base.background = QStringLiteral("#E61FD1C1");
        base.color = QStringLiteral("#06201D");
        pos.x = -0.55;
        pos.y = 0.52;
    } else if (name == QLatin1String("end-card")) {
        base.size = 0.09;
        base.color = QStringLiteral("#1FD1C1");
    } else if (name == QLatin1String("headline")) {  // big ad headline
        base.size = 0.14;
        base.gradient = QStringLiteral("#FFD60A");
        base.shadow = QStringLiteral("#000000");
        pos.y = -0.12;
        move.in = QStringLiteral("kinetic");
        move.inUs = 900'000;
    } else if (name == QLatin1String("cta")) {  // call-to-action button
        base.size = 0.065;
        base.background = QStringLiteral("#FF3B30");
        base.shape = QStringLiteral("pill");
        pos.y = 0.62;
        move.in = QStringLiteral("pop");
        move.loop = QStringLiteral("pulse");
    } else if (name == QLatin1String("price")) {  // tilted price callout
        base.size = 0.1;
        base.color = QStringLiteral("#FFD60A");
        base.outline = 0.06;
        base.shadow = QStringLiteral("#000000");
        pos.x = 0.5;
        pos.y = -0.5;
        pos.rotation = -8;
        move.in = QStringLiteral("pop");
        move.loop = QStringLiteral("wiggle");
    } else if (name == QLatin1String("badge")) {  // "NEW" / "50% OFF" starburst
        base.size = 0.055;
        base.background = QStringLiteral("#FF2D55");
        base.shape = QStringLiteral("burst");
        pos.x = -0.62;
        pos.y = -0.6;
        pos.rotation = -12;
        move.in = QStringLiteral("spin");
        move.loop = QStringLiteral("pulse");
    } else if (name == QLatin1String("kinetic-caption")) {  // words pop in one by one
        base.size = 0.07;
        base.outline = 0.07;
        pos.y = 0.35;
        move.in = QStringLiteral("kinetic");
        move.inUs = 1'200'000;
    } else {
        return false;
    }
    base.text = text;
    *st = base;
    *tr = pos;
    if (motion) *motion = move;
    return true;
}

qint64 Draft::duration() const
{
    qint64 d = 0;
    for (const Track &t : tracks)
        for (const Segment &s : t.segments) d = std::max(d, s.end());
    return d;
}

const Material *Draft::material(const QString &id) const
{
    for (const Material &m : materials)
        if (m.id == id) return &m;
    return nullptr;
}

Track *Draft::track(const QString &id)
{
    for (Track &t : tracks)
        if (t.id == id) return &t;
    return nullptr;
}

Segment *Draft::segment(const QString &id, Track **owner)
{
    for (Track &t : tracks)
        for (Segment &s : t.segments)
            if (s.id == id) {
                if (owner) *owner = &t;
                return &s;
            }
    return nullptr;
}

const Segment *Draft::segment(const QString &id, const Track **owner) const
{
    for (const Track &t : tracks)
        for (const Segment &s : t.segments)
            if (s.id == id) {
                if (owner) *owner = &t;
                return &s;
            }
    return nullptr;
}

void Draft::normalize()
{
    if (tracks.isEmpty() || tracks[0].role != TrackRole::Main) {
        auto it = std::find_if(tracks.begin(), tracks.end(),
                               [](const Track &t) { return t.role == TrackRole::Main; });
        if (it != tracks.end()) {
            Track main = *it;
            tracks.erase(it);
            tracks.prepend(main);
        } else {
            Track main;
            main.id = newId();
            tracks.prepend(main);
        }
    }

    auto byStart = [](const Segment &a, const Segment &b) { return a.start < b.start; };
    for (Track &t : tracks) std::stable_sort(t.segments.begin(), t.segments.end(), byStart);

    // Main track is magnetic: clips sit back to back from zero.
    qint64 cursor = 0;
    for (Segment &s : tracks[0].segments) {
        s.start = cursor;
        cursor += s.duration;
    }

    tracks.erase(std::remove_if(tracks.begin() + 1, tracks.end(),
                                [](const Track &t) { return t.segments.isEmpty(); }),
                 tracks.end());
}

QJsonObject Draft::toJson() const
{
    QJsonArray mats;
    for (const Material &m : materials) {
        mats.append(QJsonObject{
            {"id", m.id},
            {"kind", kindName(m.kind)},
            {"path", m.path},
            {"name", m.name},
            {"duration", double(m.durationUs)},
            {"width", m.width},
            {"height", m.height},
            {"fps", m.fps},
            {"has_video", m.hasVideo},
            {"has_audio", m.hasAudio},
            {"thumbnail", m.thumbnail},
        });
    }

    QJsonArray trks;
    for (const Track &t : tracks) {
        QJsonArray segs;
        for (const Segment &s : t.segments) {
            QJsonObject so{
                {"id", s.id},
                {"material", s.materialId},
                {"start", double(s.start)},
                {"duration", double(s.duration)},
                {"source_start", double(s.sourceStart)},
                {"volume", s.volume},
                {"opacity", s.opacity},
                {"transform", QJsonObject{{"x", s.transform.x},
                                          {"y", s.transform.y},
                                          {"scale", s.transform.scale},
                                          {"rotation", s.transform.rotation}}},
            };
            if (!s.motion.isEmpty()) so.insert("motion", motionToJson(s.motion));
            if (!s.keys.isEmpty()) so.insert("keys", keysToJson(s.keys));
            if (!s.effects.isEmpty()) so.insert("effects", effectsToJson(s.effects));
            if (s.speed != 1.0) so.insert("speed", s.speed);
            if (!s.curve.isEmpty()) so.insert("curve", s.curve);
            so.insert("framing", s.framing);
            if (s.speech) so.insert("speech", true);
            if (s.duck > 0) so.insert("duck", s.duck);
            if (!s.origin.isEmpty()) so.insert("origin", s.origin);
            if (!s.group.isEmpty()) so.insert("group", s.group);
            if (!s.transition.name.isEmpty())
                so.insert("transition", QJsonObject{{"name", s.transition.name}, {"duration", double(s.transition.us)}});
            if (s.materialId == kTextMaterialId) so.insert("text", textToJson(s.text));
            segs.append(so);
        }
        trks.append(QJsonObject{{"id", t.id}, {"role", roleName(t.role)}, {"segments", segs}});
    }

    return QJsonObject{
        {"format", "montage-draft"},
        {"version", 1},
        {"name", name},
        {"canvas", QJsonObject{{"width", canvas.width},
                               {"height", canvas.height},
                               {"fps", canvas.fps},
                               {"background", canvas.background},
                               {"locked", canvasLocked}}},
        {"style", style},
        {"beat_sync", beatSync},
        {"captions", captions},
        {"clean_speech", cleanSpeech},
        {"materials", mats},
        {"tracks", trks},
    };
}

Draft Draft::fromJson(const QJsonObject &obj)
{
    Draft d;
    d.tracks.clear();
    d.name = obj.value("name").toString(QStringLiteral("Untitled"));

    const QJsonObject c = obj.value("canvas").toObject();
    d.canvas.width = c.value("width").toInt(1920);
    d.canvas.height = c.value("height").toInt(1080);
    d.canvas.fps = c.value("fps").toDouble(30);
    d.canvas.background = c.value("background").toString(QStringLiteral("#000000"));
    d.canvasLocked = c.value("locked").toBool(false);
    d.style = obj.value("style").toString();
    d.beatSync = obj.value("beat_sync").toString();
    d.captions = obj.value("captions").toString();
    d.cleanSpeech = obj.value("clean_speech").toBool();

    for (const QJsonValue &v : obj.value("materials").toArray()) {
        const QJsonObject o = v.toObject();
        Material m;
        m.id = o.value("id").toString();
        m.kind = kindFromName(o.value("kind").toString());
        m.path = o.value("path").toString();
        m.name = o.value("name").toString();
        m.durationUs = qint64(o.value("duration").toDouble());
        m.width = o.value("width").toInt();
        m.height = o.value("height").toInt();
        m.fps = o.value("fps").toDouble();
        m.hasVideo = o.value("has_video").toBool();
        m.hasAudio = o.value("has_audio").toBool();
        m.thumbnail = o.value("thumbnail").toString();
        d.materials.append(m);
    }

    for (const QJsonValue &v : obj.value("tracks").toArray()) {
        const QJsonObject o = v.toObject();
        Track t;
        t.id = o.value("id").toString(newId());
        t.role = roleFromName(o.value("role").toString());
        for (const QJsonValue &sv : o.value("segments").toArray()) {
            const QJsonObject so = sv.toObject();
            Segment s;
            s.id = so.value("id").toString(newId());
            s.materialId = so.value("material").toString();
            s.start = qint64(so.value("start").toDouble());
            s.duration = qint64(so.value("duration").toDouble());
            s.sourceStart = qint64(so.value("source_start").toDouble());
            s.volume = so.value("volume").toDouble(1.0);
            s.opacity = so.value("opacity").toDouble(1.0);
            const QJsonObject tr = so.value("transform").toObject();
            s.transform.x = tr.value("x").toDouble(0);
            s.transform.y = tr.value("y").toDouble(0);
            s.transform.scale = tr.value("scale").toDouble(1);
            s.transform.rotation = tr.value("rotation").toDouble(0);
            s.motion = motionFromJson(so.value("motion").toObject());
            s.keys = keysFromJson(so.value("keys").toObject());
            s.effects = effectsFromJson(so.value("effects").toArray());
            s.speed = std::clamp(so.value("speed").toDouble(1.0), 0.0, 16.0);
            s.curve = so.value("curve").toString();
            s.framing = so.value("framing").toString(QStringLiteral("fit"));  // drafts from before framing letterboxed
            s.speech = so.value("speech").toBool();
            s.duck = std::clamp(so.value("duck").toDouble(0), 0.0, 1.0);
            s.origin = so.value("origin").toString();
            s.group = so.value("group").toString();
            s.transition.name = so.value("transition").toObject().value("name").toString();
            s.transition.us = qint64(so.value("transition").toObject().value("duration").toDouble(500'000));
            if (so.contains("text")) s.text = textFromJson(so.value("text").toObject());
            if (d.material(s.materialId) && s.duration > 0) t.segments.append(s);
        }
        d.tracks.append(t);
    }

    d.normalize();
    return d;
}

}  // namespace mtg
