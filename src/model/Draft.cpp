#include "Draft.h"

#include <QJsonArray>
#include <QUuid>
#include <algorithm>

namespace mtg {

QString kindName(MaterialKind k)
{
    switch (k) {
    case MaterialKind::Video: return QStringLiteral("video");
    case MaterialKind::Audio: return QStringLiteral("audio");
    case MaterialKind::Image: return QStringLiteral("image");
    case MaterialKind::Text: return QStringLiteral("text");
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

static QJsonObject textToJson(const TextStyle &t)
{
    return QJsonObject{
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
    return t;
}

// Starting looks for text clips: style plus where it sits on the canvas.
bool applyTextPreset(const QString &name, TextStyle *st, Transform *tr)
{
    const QString text = st->text;
    TextStyle base;
    Transform pos;
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
    } else {
        return false;
    }
    base.text = text;
    *st = base;
    *tr = pos;
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
                                          {"scale", s.transform.scale}}},
            };
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
            if (so.contains("text")) s.text = textFromJson(so.value("text").toObject());
            if (d.material(s.materialId) && s.duration > 0) t.segments.append(s);
        }
        d.tracks.append(t);
    }

    d.normalize();
    return d;
}

}  // namespace mtg
