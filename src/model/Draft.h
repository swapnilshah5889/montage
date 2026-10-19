#pragma once

// The project document. Same idea as CapCut's draft: media lives in a
// material table and the timeline only holds segments that point at a
// material by id, with a timeline range and a source offset. All times are
// microseconds.

#include <QJsonObject>
#include <QList>
#include <QString>

namespace mtg {

enum class MaterialKind { Video, Audio, Image };
enum class TrackRole { Main, Overlay, Audio };

QString kindName(MaterialKind k);
QString roleName(TrackRole r);

constexpr qint64 kImageDefaultUs = 5'000'000;
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
    bool isTimed() const { return kind != MaterialKind::Image; }
};

struct Transform {
    double x = 0;      // offset as a fraction of half the canvas width
    double y = 0;      // offset as a fraction of half the canvas height
    double scale = 1;  // 1 = fit inside the canvas
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
};

struct Draft {
    QString name = QStringLiteral("Untitled");
    Canvas canvas;
    bool canvasLocked = false;  // false: follow the first clip's shape
    QList<Material> materials;
    QList<Track> tracks;        // tracks[0] is always the main track

    Draft();

    qint64 duration() const;
    const Material *material(const QString &id) const;
    Track *track(const QString &id);
    Segment *segment(const QString &id, Track **owner = nullptr);
    const Segment *segment(const QString &id, const Track **owner = nullptr) const;
    Track &mainTrack() { return tracks[0]; }

    // Re-establish invariants after an edit: the main track is magnetic
    // (no gaps), other tracks are sorted, and empty extra tracks go away.
    void normalize();

    QJsonObject toJson() const;
    static Draft fromJson(const QJsonObject &obj);

    static QString newId();
};

}  // namespace mtg
