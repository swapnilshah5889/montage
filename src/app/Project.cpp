#include "Project.h"

#include "engine/MediaProbe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QSize>
#include <QStandardPaths>
#include <algorithm>
#include <limits>

using namespace mtg;

namespace {

constexpr qint64 kFar = std::numeric_limits<qint64>::max() / 4;

struct AspectPreset {
    const char *name;
    int w, h;
};
constexpr AspectPreset kAspects[] = {
    {"16:9", 1920, 1080}, {"9:16", 1080, 1920}, {"1:1", 1080, 1080},
    {"4:3", 1440, 1080},  {"3:4", 1080, 1440},  {"21:9", 2560, 1080},
};

void insertOnMain(Draft &d, Segment seg, qint64 at)
{
    QList<Segment> &segs = d.mainTrack().segments;
    int index = 0;
    while (index < segs.size() && segs[index].start + segs[index].duration / 2 < at) ++index;
    segs.insert(index, seg);
}

QVariantMap segmentMap(const Draft &d, const Segment &s)
{
    const Material *m = d.material(s.materialId);
    const bool isText = m && m->kind == MaterialKind::Text;
    QVariantMap map{
        {"id", s.id},
        {"materialId", s.materialId},
        {"name", isText ? s.text.text.section(QLatin1Char('\n'), 0, 0) : m ? m->name : QString()},
        {"kind", m ? kindName(m->kind) : QString()},
        {"thumbnail", m && !m->thumbnail.isEmpty() ? QUrl::fromLocalFile(m->thumbnail).toString() : QString()},
        {"hasAudio", m && m->hasAudio},
        {"materialDuration", m ? double(m->durationUs) : 0.0},
        {"width", m ? m->width : 0},
        {"height", m ? m->height : 0},
        {"start", double(s.start)},
        {"duration", double(s.duration)},
        {"sourceStart", double(s.sourceStart)},
        {"volume", s.volume},
        {"opacity", s.opacity},
        {"x", s.transform.x},
        {"y", s.transform.y},
        {"scale", s.transform.scale},
    };
    if (isText) {
        map.insert("text", s.text.text);
        map.insert("font", s.text.font);
        map.insert("textSize", s.text.size);
        map.insert("color", s.text.color);
        map.insert("bold", s.text.bold);
        map.insert("outline", s.text.outline);
        map.insert("outlineColor", s.text.outlineColor);
        map.insert("background", s.text.background);
        map.insert("align", s.text.align);
    }
    return map;
}


}  // namespace

Project::Project(QObject *parent)
    : QObject(parent)
{
    s_instance = this;
    m_thumbDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/thumbs");
}

Project *Project::create(QQmlEngine *, QJSEngine *)
{
    Q_ASSERT(s_instance);
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

template <typename F>
bool Project::edit(F &&fn)
{
    Draft next = m_draft;
    if (!fn(next)) return false;
    next.normalize();
    applyCanvasFromFirstClip(next);

    const Draft &base = m_liveEditBase ? *m_liveEditBase : m_draft;
    if (next.toJson() == base.toJson()) {
        m_draft = next;
        m_liveEditBase.reset();
        notify();
        return false;
    }
    if (m_groupDepth == 0 || !m_groupPushed) m_undo.append(base);
    if (m_groupDepth > 0) m_groupPushed = true;
    if (m_undo.size() > 200) m_undo.removeFirst();
    m_redo.clear();
    m_liveEditBase.reset();
    m_draft = next;
    m_dirty = true;
    notify();
    return true;
}

void Project::notify()
{
    if (!m_selected.isEmpty() && !m_draft.segment(m_selected)) m_selected.clear();
    emit changed();
    emit selectionChanged();
}

void Project::applyCanvasFromFirstClip(Draft &d) const
{
    if (d.canvasLocked || d.mainTrack().segments.isEmpty()) return;
    // Follow the first video; a logo or photo at the start shouldn't set the
    // shape (or a tiny resolution) for the whole film.
    const Material *m = nullptr;
    for (const Segment &s : d.mainTrack().segments) {
        const Material *candidate = d.material(s.materialId);
        if (candidate && candidate->width > 0 && candidate->height > 0) {
            if (!m) m = candidate;
            if (candidate->kind == MaterialKind::Video) {
                m = candidate;
                break;
            }
        }
    }
    if (!m) return;
    // Keep the clip's shape: at least 1080 on the short side, at most 4K on the long side.
    QSize use(m->width, m->height);
    if (std::min(use.width(), use.height()) < 1080) use = use * (1080.0 / std::min(use.width(), use.height()));
    if (std::max(use.width(), use.height()) > 3840) use = use.scaled(3840, 3840, Qt::KeepAspectRatio);
    d.canvas.width = std::max(2, use.width() & ~1);
    d.canvas.height = std::max(2, use.height() & ~1);
    if (m->fps >= 10 && m->fps <= 120) d.canvas.fps = std::round(m->fps * 1000) / 1000;
}

void Project::setName(const QString &name)
{
    if (name == m_draft.name || name.trimmed().isEmpty()) return;
    m_draft.name = name.trimmed();
    m_dirty = true;
    emit changed();
}

QString Project::aspect() const
{
    if (!m_draft.canvasLocked) return QStringLiteral("Original");
    for (const AspectPreset &a : kAspects)
        if (a.w == m_draft.canvas.width && a.h == m_draft.canvas.height) return QString::fromLatin1(a.name);
    return QStringLiteral("Custom");
}

QVariantList Project::materials() const
{
    QSet<QString> used;
    for (const Track &t : m_draft.tracks)
        for (const Segment &s : t.segments) used.insert(s.materialId);

    QVariantList out;
    for (const Material &m : m_draft.materials) {
        if (m.kind == MaterialKind::Text) continue;
        out.append(QVariantMap{
            {"id", m.id},
            {"kind", kindName(m.kind)},
            {"name", m.name},
            {"path", m.path},
            {"duration", double(m.durationUs)},
            {"width", m.width},
            {"height", m.height},
            {"hasAudio", m.hasAudio},
            {"thumbnail", m.thumbnail.isEmpty() ? QString() : QUrl::fromLocalFile(m.thumbnail).toString()},
            {"used", used.contains(m.id)},
        });
    }
    return out;
}

QVariantList Project::tracks() const
{
    // Display order, top to bottom: overlays (newest on top), main, audio.
    QList<const Track *> order;
    for (auto it = m_draft.tracks.crbegin(); it != m_draft.tracks.crend(); ++it)
        if (it->role == TrackRole::Overlay) order.append(&*it);
    order.append(&m_draft.tracks[0]);
    for (const Track &t : m_draft.tracks)
        if (t.role == TrackRole::Audio) order.append(&t);

    QVariantList out;
    for (const Track *t : order) {
        QVariantList segs;
        for (const Segment &s : t->segments) segs.append(segmentMap(m_draft, s));
        out.append(QVariantMap{{"id", t->id}, {"role", roleName(t->role)}, {"segments", segs}});
    }
    return out;
}

void Project::setSelectedId(const QString &id)
{
    if (id == m_selected) return;
    m_selected = m_draft.segment(id) ? id : QString();
    emit selectionChanged();
}

QVariantMap Project::selection() const
{
    const Track *owner = nullptr;
    const Segment *s = m_draft.segment(m_selected, &owner);
    if (!s) return {};
    QVariantMap map = segmentMap(m_draft, *s);
    map.insert("role", roleName(owner->role));
    return map;
}

int Project::importFiles(const QList<QUrl> &urls)
{
    QList<Material> added;
    QStringList failed;
    for (const QUrl &url : urls) {
        const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
        if (QFileInfo(path).isDir()) continue;
        const bool known = std::any_of(m_draft.materials.cbegin(), m_draft.materials.cend(),
                                       [&](const Material &m) { return m.path == path; });
        if (known) continue;
        Material m;
        QString error;
        if (!probeMedia(path, m_thumbDir, &m, &error)) {
            failed << QFileInfo(path).fileName() + QStringLiteral(" (") + error + QLatin1Char(')');
            continue;
        }
        m.id = Draft::newId();
        added.append(m);
    }
    if (!failed.isEmpty()) emit message(QStringLiteral("Could not import: ") + failed.join(QStringLiteral(", ")));
    if (added.isEmpty()) return 0;
    edit([&](Draft &d) {
        d.materials.append(added);
        return true;
    });
    return int(added.size());
}

void Project::removeMaterial(const QString &materialId)
{
    edit([&](Draft &d) {
        const auto before = d.materials.size();
        d.materials.removeIf([&](const Material &m) { return m.id == materialId; });
        for (Track &t : d.tracks) t.segments.removeIf([&](const Segment &s) { return s.materialId == materialId; });
        return d.materials.size() != before;
    });
}

void Project::place(Draft &d, Segment seg, const QString &trackId, TrackRole role) const
{
    if (role == TrackRole::Main) {
        insertOnMain(d, seg, seg.start);
        return;
    }
    seg.start = std::max<qint64>(0, seg.start);
    Track *preferred = d.track(trackId);
    if (preferred && preferred->role == role) {
        if (preferred->fits(seg.start, seg.end(), seg.id)) {
            preferred->segments.append(seg);
            return;
        }
    } else if (trackId.isEmpty()) {
        for (Track &t : d.tracks) {
            if (t.role == role && t.fits(seg.start, seg.end(), seg.id)) {
                t.segments.append(seg);
                return;
            }
        }
    }
    Track fresh;
    fresh.id = Draft::newId();
    fresh.role = role;
    fresh.segments.append(seg);
    d.tracks.append(fresh);
}

QString Project::addToTimeline(const QString &materialId, const QString &trackId, qint64 at)
{
    const Material *m = m_draft.material(materialId);
    if (!m) return {};

    Segment seg;
    seg.id = Draft::newId();
    seg.materialId = materialId;
    seg.duration = m->isTimed() ? m->durationUs : kImageDefaultUs;

    const Track *target = nullptr;
    for (const Track &t : m_draft.tracks)
        if (t.id == trackId) target = &t;

    TrackRole role = TrackRole::Main;
    if (m->kind == MaterialKind::Audio)
        role = TrackRole::Audio;
    else if (trackId == QLatin1String("new-overlay") || (target && target->role == TrackRole::Overlay))
        role = TrackRole::Overlay;

    if (role == TrackRole::Main) seg.start = at < 0 ? kFar : at;
    else seg.start = std::max<qint64>(0, at);

    const bool ok = edit([&](Draft &d) {
        place(d, seg, (target && target->role == role) || trackId.startsWith(QLatin1String("new-")) ? trackId : QString(), role);
        return true;
    });
    if (!ok) return {};
    setSelectedId(seg.id);
    return seg.id;
}

void Project::moveSegment(const QString &segmentId, const QString &trackId, qint64 start)
{
    edit([&](Draft &d) {
        Track *owner = nullptr;
        Segment *found = d.segment(segmentId, &owner);
        if (!found) return false;
        const Material *m = d.material(found->materialId);
        if (!m) return false;

        const TrackRole fromRole = owner->role;
        Track *target = d.track(trackId);
        TrackRole role = fromRole;
        if (m->kind == MaterialKind::Audio) {
            role = TrackRole::Audio;
            if (target && target->role != TrackRole::Audio) target = nullptr;
        } else if (target) {
            if (target->role == TrackRole::Audio) return false;  // pictures don't go on audio tracks
            if (m->kind == MaterialKind::Text && target->role == TrackRole::Main) return false;
            role = target->role;
        } else if (trackId == QLatin1String("new-overlay")) {
            role = TrackRole::Overlay;
        }

        Segment seg = *found;
        seg.start = std::max<qint64>(0, start);
        owner->segments.removeIf([&](const Segment &s) { return s.id == segmentId; });

        QString destination = target ? target->id : trackId;
        if (!target && !trackId.startsWith(QLatin1String("new-"))) destination = owner->id;
        place(d, seg, destination, role);
        return true;
    });
}

void Project::trimSegment(const QString &segmentId, qint64 newStart, qint64 newDuration)
{
    edit([&](Draft &d) {
        Track *owner = nullptr;
        Segment *s = d.segment(segmentId, &owner);
        if (!s) return false;
        const Material *m = d.material(s->materialId);
        if (!m) return false;
        const bool main = owner->role == TrackRole::Main;
        const qint64 oldStart = s->start;
        const qint64 oldEnd = s->end();

        if (newStart != oldStart) {
            // Left edge: the end stays put, the source in-point moves.
            qint64 minStart = m->isTimed() ? oldStart - s->sourceStart : -kFar;
            if (!main) {
                minStart = std::max<qint64>(minStart, 0);
                for (const Segment &o : owner->segments)
                    if (o.id != s->id && o.end() <= oldStart) minStart = std::max(minStart, o.end());
            }
            const qint64 start = std::clamp(newStart, minStart, oldEnd - kMinSegmentUs);
            s->sourceStart = m->isTimed() ? s->sourceStart + (start - oldStart) : 0;
            s->start = start;
            s->duration = oldEnd - start;
        } else {
            qint64 maxEnd = m->isTimed() ? oldStart + (m->durationUs - s->sourceStart) : kFar;
            if (!main)
                for (const Segment &o : owner->segments)
                    if (o.id != s->id && o.start >= oldEnd) maxEnd = std::min(maxEnd, o.start);
            const qint64 end = std::clamp(oldStart + newDuration, oldStart + kMinSegmentUs, std::max(maxEnd, oldStart + kMinSegmentUs));
            s->duration = end - oldStart;
        }
        return true;
    });
}

bool Project::splitAt(qint64 t)
{
    auto splittable = [t](const Segment &s) { return t > s.start + kMinSegmentUs / 2 && t < s.end() - kMinSegmentUs / 2; };

    QString targetId;
    if (const Segment *sel = m_draft.segment(m_selected); sel && splittable(*sel)) {
        targetId = sel->id;
    } else {
        for (const Segment &s : m_draft.tracks[0].segments)
            if (splittable(s)) targetId = s.id;
    }
    if (targetId.isEmpty()) return false;

    QString rightId = Draft::newId();
    const bool ok = edit([&](Draft &d) {
        Track *owner = nullptr;
        Segment *left = d.segment(targetId, &owner);
        if (!left) return false;
        const Material *m = d.material(left->materialId);
        Segment right = *left;
        right.id = rightId;
        right.start = t;
        right.duration = left->end() - t;
        right.sourceStart = (m && m->isTimed()) ? left->sourceStart + (t - left->start) : 0;
        left->duration = t - left->start;
        for (qsizetype i = 0; i < owner->segments.size(); ++i) {
            if (owner->segments[i].id == targetId) {
                owner->segments.insert(i + 1, right);
                break;
            }
        }
        return true;
    });
    if (ok) setSelectedId(rightId);
    return ok;
}

void Project::deleteSelected()
{
    const QString id = m_selected;
    if (id.isEmpty()) return;
    edit([&](Draft &d) {
        for (Track &t : d.tracks) t.segments.removeIf([&](const Segment &s) { return s.id == id; });
        return true;
    });
}

static void applyProperty(Segment &s, const QString &key, double v)
{
    if (key == QLatin1String("volume")) s.volume = std::clamp(v, 0.0, 2.0);
    else if (key == QLatin1String("opacity")) s.opacity = std::clamp(v, 0.0, 1.0);
    else if (key == QLatin1String("x")) s.transform.x = std::clamp(v, -2.0, 2.0);
    else if (key == QLatin1String("y")) s.transform.y = std::clamp(v, -2.0, 2.0);
    else if (key == QLatin1String("scale")) s.transform.scale = std::clamp(v, 0.05, 8.0);
}

template <typename F>
void Project::liveOrCommit(const QString &segmentId, bool commit, F &&apply)
{
    if (!commit) {
        if (!std::as_const(m_draft).segment(segmentId)) return;
        // Snapshot before taking a mutable pointer: the copy shares storage,
        // and the pointer must come from m_draft after it detaches.
        if (!m_liveEditBase) m_liveEditBase = m_draft;
        apply(*m_draft.segment(segmentId));
        m_dirty = true;
        emit selectionChanged();
        emit liveChanged();
        return;
    }
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        if (!s) return false;
        apply(*s);
        return true;
    });
}

void Project::setSegmentProperty(const QString &segmentId, const QString &key, double value, bool commit)
{
    liveOrCommit(segmentId, commit, [&](Segment &s) { applyProperty(s, key, value); });
}

static void applyTextProperty(Segment &s, const QString &key, const QVariant &v)
{
    TextStyle &t = s.text;
    if (key == QLatin1String("text")) t.text = v.toString();
    else if (key == QLatin1String("font")) t.font = v.toString();
    else if (key == QLatin1String("size")) t.size = std::clamp(v.toDouble(), 0.01, 0.5);
    else if (key == QLatin1String("color")) t.color = v.toString();
    else if (key == QLatin1String("bold")) t.bold = v.toBool();
    else if (key == QLatin1String("outline")) t.outline = std::clamp(v.toDouble(), 0.0, 0.3);
    else if (key == QLatin1String("outlineColor")) t.outlineColor = v.toString();
    else if (key == QLatin1String("background")) t.background = v.toString();
    else if (key == QLatin1String("align")) t.align = v.toString();
}

void Project::setTextProperty(const QString &segmentId, const QString &key, const QVariant &value, bool commit)
{
    if (const Segment *s = std::as_const(m_draft).segment(segmentId); !s || s->materialId != kTextMaterialId) return;
    liveOrCommit(segmentId, commit, [&](Segment &s) { applyTextProperty(s, key, value); });
}

QString Project::addText(const QString &text, qint64 at, qint64 duration, const QString &preset)
{
    Segment seg;
    seg.id = Draft::newId();
    seg.materialId = kTextMaterialId;
    seg.start = std::max<qint64>(0, at);
    seg.duration = duration > 0 ? duration : kTextDefaultUs;
    seg.text.text = text;
    if (!mtg::applyTextPreset(preset, &seg.text, &seg.transform))
        mtg::applyTextPreset(QStringLiteral("title"), &seg.text, &seg.transform);
    const bool ok = edit([&](Draft &d) {
        d.ensureTextMaterial();
        place(d, seg, {}, TrackRole::Overlay);
        return true;
    });
    if (!ok) return {};
    setSelectedId(seg.id);
    return seg.id;
}

void Project::applyTextPreset(const QString &segmentId, const QString &preset)
{
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        if (!s || s->materialId != kTextMaterialId) return false;
        return mtg::applyTextPreset(preset, &s->text, &s->transform);
    });
}

QStringList Project::textPresets() const
{
    return {QStringLiteral("title"), QStringLiteral("subtitle"), QStringLiteral("caption"),
            QStringLiteral("lower-third"), QStringLiteral("end-card")};
}

bool Project::applyDraft(const Draft &draft)
{
    return edit([&](Draft &d) {
        d = draft;
        return true;
    });
}

void Project::beginUndoGroup()
{
    if (m_groupDepth++ == 0) m_groupPushed = false;
}

void Project::endUndoGroup()
{
    if (m_groupDepth > 0 && --m_groupDepth == 0) m_groupPushed = false;
}

void Project::resetTransform(const QString &segmentId)
{
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        if (!s) return false;
        s->transform = Transform{};
        s->opacity = 1.0;
        return true;
    });
}

void Project::setAspect(const QString &aspect)
{
    edit([&](Draft &d) {
        if (aspect == QLatin1String("Original")) {
            d.canvasLocked = false;
            return true;
        }
        for (const AspectPreset &a : kAspects) {
            if (aspect == QLatin1String(a.name)) {
                d.canvasLocked = true;
                d.canvas.width = a.w;
                d.canvas.height = a.h;
                return true;
            }
        }
        return false;
    });
}

void Project::undo()
{
    if (m_undo.isEmpty()) return;
    m_liveEditBase.reset();
    m_redo.append(m_draft);
    m_draft = m_undo.takeLast();
    m_dirty = true;
    notify();
}

void Project::redo()
{
    if (m_redo.isEmpty()) return;
    m_liveEditBase.reset();
    m_undo.append(m_draft);
    m_draft = m_redo.takeLast();
    m_dirty = true;
    notify();
}

void Project::newProject()
{
    m_draft = Draft();
    m_undo.clear();
    m_redo.clear();
    m_liveEditBase.reset();
    m_selected.clear();
    m_file = QUrl();
    m_dirty = false;
    notify();
}

bool Project::save(const QUrl &url)
{
    const QUrl target = url.isEmpty() ? m_file : url;
    if (target.isEmpty()) return false;
    const QString path = target.toLocalFile();
    if (m_draft.name == QLatin1String("Untitled")) m_draft.name = QFileInfo(path).completeBaseName();

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit message(QStringLiteral("Could not save: ") + f.errorString());
        return false;
    }
    f.write(QJsonDocument(m_draft.toJson()).toJson(QJsonDocument::Indented));
    m_file = target;
    m_dirty = false;
    emit changed();
    return true;
}

bool Project::load(const QUrl &url)
{
    QFile f(url.toLocalFile());
    if (!f.open(QIODevice::ReadOnly)) {
        emit message(QStringLiteral("Could not open: ") + f.errorString());
        return false;
    }
    const QJsonObject obj = QJsonDocument::fromJson(f.readAll()).object();
    if (obj.value("format").toString() != QLatin1String("montage-draft")) {
        emit message(QStringLiteral("Not a Montage project."));
        return false;
    }
    Draft d = Draft::fromJson(obj);
    for (Material &m : d.materials) {
        if (!m.thumbnail.isEmpty() && QFileInfo::exists(m.thumbnail)) continue;
        Material fresh;
        if (QFileInfo::exists(m.path) && probeMedia(m.path, m_thumbDir, &fresh, nullptr)) m.thumbnail = fresh.thumbnail;
    }
    m_draft = d;
    m_undo.clear();
    m_redo.clear();
    m_liveEditBase.reset();
    m_selected.clear();
    m_file = url;
    m_dirty = false;
    notify();
    return true;
}

qint64 Project::snap(qint64 t, qint64 threshold, qint64 playhead, const QString &excludeSegment) const
{
    qint64 best = t;
    qint64 bestDist = threshold + 1;
    auto consider = [&](qint64 c) {
        const qint64 dist = std::abs(c - t);
        if (dist < bestDist) {
            bestDist = dist;
            best = c;
        }
    };
    consider(0);
    consider(playhead);
    for (const Track &tr : m_draft.tracks)
        for (const Segment &s : tr.segments)
            if (s.id != excludeSegment) {
                consider(s.start);
                consider(s.end());
            }
    return bestDist <= threshold ? best : t;
}

QString Project::formatTime(qint64 us, bool frames) const
{
    us = std::max<qint64>(0, us);
    const qint64 totalSec = us / 1'000'000;
    const int h = int(totalSec / 3600), m = int(totalSec / 60 % 60), s = int(totalSec % 60);
    const QLatin1Char zero('0');
    if (!frames) {
        return h > 0 ? QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, zero).arg(s, 2, 10, zero)
                     : QStringLiteral("%1:%2").arg(m, 2, 10, zero).arg(s, 2, 10, zero);
    }
    // HH:MM:SS:FF, like broadcast timecode
    const int f = int((us % 1'000'000) * m_draft.canvas.fps / 1'000'000);
    return QStringLiteral("%1:%2:%3:%4").arg(h, 2, 10, zero).arg(m, 2, 10, zero).arg(s, 2, 10, zero).arg(f, 2, 10, zero);
}
