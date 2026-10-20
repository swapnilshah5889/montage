#include "Project.h"

#include "engine/Beats.h"
#include "engine/Speech.h"

#include "engine/Animation.h"
#include "engine/Effects.h"
#include "engine/MediaProbe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSet>
#include <QThread>
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
    {"4:3", 1440, 1080},  {"3:4", 1080, 1440},  {"21:9", 2560, 1080}, {"4:5", 1080, 1350},
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
        {"rotation", s.transform.rotation},
        {"motionIn", s.motion.in},
        {"motionInSec", s.motion.inUs / 1e6},
        {"motionOut", s.motion.out},
        {"motionOutSec", s.motion.outUs / 1e6},
        {"motionLoop", s.motion.loop},
        {"speed", s.speed},
        {"curve", s.curve},
        {"framing", s.framing},
        {"speech", s.speech},
        {"duck", s.duck},
        {"origin", s.origin},
        {"beats", [&] {
             // Beat ticks for music clips, in seconds from the clip's start.
             QVariantList out;
             if (!m || m->kind != MaterialKind::Audio || s.speech) return out;
             const BeatGrid g = beatGrid(*m);
             for (double b : g.beats) {
                 const double local = b - s.sourceStart / 1e6;
                 if (local < 0) continue;
                 if (local * 1e6 > s.duration || out.size() > 600) break;
                 out << local;
             }
             return out;
         }()},
        {"transition", s.transition.name},
        {"transitionSec", s.transition.us / 1e6},
        {"effects", [&] {
             QVariantList fx;
             for (const Effect &e : s.effects) fx << QVariantMap{{"name", e.name}, {"amount", e.amount}};
             return fx;
         }()},
        {"keyframes", [&] {
             QVariantList times;  // clip-relative seconds, for timeline markers
             QSet<qint64> seen;
             for (const auto &track : s.keys)
                 for (const KeyPoint &k : track)
                     if (!seen.contains(k.t)) {
                         seen.insert(k.t);
                         times << k.t / 1e6;
                     }
             return times;
         }()},
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
        map.insert("shape", s.text.shape.isEmpty() ? QStringLiteral("rounded") : s.text.shape);
        map.insert("shadow", s.text.shadow);
        map.insert("gradient", s.text.gradient);
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
        if (m.kind == MaterialKind::Text || m.kind == MaterialKind::Effect) continue;
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
    map.insert("canTransition", owner->role == TrackRole::Main && !owner->segments.isEmpty() && owner->segments.first().id != s->id);
    map.insert("onMain", owner->role == TrackRole::Main);
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
            if ((m->kind == MaterialKind::Text || m->kind == MaterialKind::Effect) && target->role == TrackRole::Main) return false;
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
        // Timeline time maps to source time at the clip's speed; freezes don't use up source.
        const bool timed = m->isTimed() && s->speed > 0;
        const double speed = timed ? s->speed : 1.0;

        if (newStart != oldStart) {
            // Left edge: the end stays put, the source in-point moves.
            qint64 minStart = timed ? oldStart - qint64(double(s->sourceStart) / speed) : -kFar;
            if (!main) {
                minStart = std::max<qint64>(minStart, 0);
                for (const Segment &o : owner->segments)
                    if (o.id != s->id && o.end() <= oldStart) minStart = std::max(minStart, o.end());
            }
            const qint64 start = std::clamp(newStart, minStart, oldEnd - kMinSegmentUs);
            if (timed) s->sourceStart = std::max<qint64>(0, s->sourceStart + qint64(double(start - oldStart) * speed));
            else if (!m->isTimed()) s->sourceStart = 0;
            s->start = start;
            s->duration = oldEnd - start;
        } else {
            qint64 maxEnd = timed ? oldStart + qint64(double(m->durationUs - s->sourceStart) / speed) : kFar;
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
        const qint64 local = t - left->start;
        right.id = rightId;
        right.start = t;
        right.duration = left->end() - t;
        right.sourceStart = (m && m->isTimed()) ? left->sourceAt(local) : 0;
        right.transition = Transition{};  // the cut inside a clip starts plain
        // Entrance stays on the first half, exit on the second.
        right.motion.in.clear();
        left->motion.out.clear();
        // Keyframes: each half keeps its side, with the value at the cut pinned.
        if (!left->keys.isEmpty()) {
            Segment still = *left;
            still.motion = Motion{};
            const FrameState at = animate(still, local);
            const QHash<QString, double> cutValue{{"x", at.x}, {"y", at.y}, {"scale", at.scale}, {"rotation", at.rotation}, {"opacity", at.opacity}};
            for (auto it = left->keys.begin(); it != left->keys.end(); ++it) {
                QList<KeyPoint> before, after;
                for (const KeyPoint &k : it.value()) {
                    if (k.t <= local) before << k;
                    if (k.t >= local) after << KeyPoint{k.t - local, k.v, k.ease};
                }
                if (before.isEmpty() || before.last().t != local) before << KeyPoint{local, cutValue.value(it.key()), QStringLiteral("linear")};
                if (after.isEmpty() || after.first().t != 0) after.prepend(KeyPoint{0, cutValue.value(it.key()), QStringLiteral("linear")});
                it.value() = before;
                right.keys[it.key()] = after;
            }
        }
        // A speed ramp can't be cut in two; both halves keep the average speed.
        left->curve.clear();
        right.curve.clear();
        left->duration = local;
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
    else if (key == QLatin1String("rotation")) s.transform.rotation = std::clamp(v, -720.0, 720.0);
    else if (key == QLatin1String("duck")) s.duck = std::clamp(v, 0.0, 1.0);
    else if (key == QLatin1String("speech")) s.speech = v > 0.5;
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

void Project::setSegmentProperty(const QString &segmentId, const QString &key, double value, bool commit, qint64 at)
{
    liveOrCommit(segmentId, commit, [&](Segment &s) {
        auto track = s.keys.find(key);
        if (at < 0 || track == s.keys.end() || track->isEmpty()) {
            applyProperty(s, key, value);
            return;
        }
        Segment probe;  // clamp the value the same way the resting property is clamped
        applyProperty(probe, key, value);
        const double v = key == QLatin1String("x") ? probe.transform.x
                         : key == QLatin1String("y") ? probe.transform.y
                         : key == QLatin1String("scale") ? probe.transform.scale
                         : key == QLatin1String("rotation") ? probe.transform.rotation
                         : key == QLatin1String("opacity") ? probe.opacity
                                                           : value;
        const qint64 local = std::clamp<qint64>(at - s.start, 0, s.duration);
        for (KeyPoint &k : *track) {
            if (std::abs(k.t - local) < 20'000) {
                k.v = v;
                return;
            }
        }
        track->append(KeyPoint{local, v, QStringLiteral("ease-in-out")});
        std::sort(track->begin(), track->end(), [](const KeyPoint &a, const KeyPoint &b) { return a.t < b.t; });
    });
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
    else if (key == QLatin1String("shape")) t.shape = v.toString() == QLatin1String("rounded") ? QString() : v.toString();
    else if (key == QLatin1String("shadow")) t.shadow = v.toString();
    else if (key == QLatin1String("gradient")) t.gradient = v.toString();
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
    if (!mtg::applyTextPreset(preset, &seg.text, &seg.transform, &seg.motion))
        mtg::applyTextPreset(QStringLiteral("title"), &seg.text, &seg.transform, &seg.motion);
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
        return mtg::applyTextPreset(preset, &s->text, &s->transform, &s->motion);
    });
}

QStringList Project::textPresets() const
{
    return textPresetNames();
}

QStringList Project::textShapes() const
{
    return textShapeNames();
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

void Project::setMotion(const QString &segmentId, const QString &slot, const QString &name, double seconds)
{
    if (!name.isEmpty() && !mtg::motionNames(slot).contains(name)) return;
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        if (!s) return false;
        const qint64 us = seconds > 0 ? qint64(seconds * 1e6) : 0;
        if (slot == QLatin1String("in")) {
            s->motion.in = name;
            if (us > 0) s->motion.inUs = us;
        } else if (slot == QLatin1String("out")) {
            s->motion.out = name;
            if (us > 0) s->motion.outUs = us;
        } else if (slot == QLatin1String("loop")) {
            s->motion.loop = name;
        } else {
            return false;
        }
        return true;
    });
}

QStringList Project::motionNames(const QString &slot) const
{
    return mtg::motionNames(slot);
}

void Project::addKeyframe(const QString &segmentId, qint64 t)
{
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        if (!s) return false;
        const qint64 local = std::clamp<qint64>(t - s->start, 0, s->duration);
        // Value the clip shows right now (keyframes only, not entrance/exit motions).
        Segment still = *s;
        still.motion = Motion{};
        const FrameState now = animate(still, local);
        const QList<QPair<QString, double>> values{{"x", now.x}, {"y", now.y}, {"scale", now.scale},
                                                   {"rotation", now.rotation}, {"opacity", now.opacity}};
        for (const auto &[prop, v] : values) {
            QList<KeyPoint> &track = s->keys[prop];
            track.removeIf([&](const KeyPoint &k) { return std::abs(k.t - local) < 20'000; });
            track.append(KeyPoint{local, v, QStringLiteral("ease-in-out")});
            std::sort(track.begin(), track.end(), [](const KeyPoint &a, const KeyPoint &b) { return a.t < b.t; });
        }
        return true;
    });
}

void Project::clearKeyframes(const QString &segmentId)
{
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        if (!s || s->keys.isEmpty()) return false;
        s->keys.clear();
        return true;
    });
}

void Project::addEffect(const QString &segmentId, const QString &name, double amount)
{
    if (!mtg::effectNames().contains(name) && !mtg::momentNames().contains(name)) return;
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        if (!s) return false;
        s->effects.append(Effect{name, amount, {}});
        return true;
    });
}

void Project::removeEffect(const QString &segmentId, int index)
{
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        if (!s || index < 0 || index >= s->effects.size()) return false;
        s->effects.removeAt(index);
        return true;
    });
}

void Project::setEffectAmount(const QString &segmentId, int index, double amount, bool commit)
{
    if (const Segment *s = std::as_const(m_draft).segment(segmentId); !s || index < 0 || index >= s->effects.size()) return;
    liveOrCommit(segmentId, commit, [&](Segment &s) { s.effects[index].amount = std::clamp(amount, -1.0, 2.0); });
}

QString Project::addEffectLayer(const QString &name, qint64 at, qint64 duration)
{
    if (!mtg::effectNames().contains(name) && !mtg::momentNames().contains(name)) return {};
    Segment seg;
    seg.id = Draft::newId();
    seg.materialId = kEffectMaterialId;
    seg.start = std::max<qint64>(0, at);
    seg.duration = duration > 0 ? duration : (mtg::momentNames().contains(name) ? 500'000 : 3'000'000);
    seg.effects.append(Effect{name, 1.0, {}});
    const bool ok = edit([&](Draft &d) {
        d.ensureEffectMaterial();
        Track fresh;  // its own track, on top, so it affects everything beneath
        fresh.id = Draft::newId();
        fresh.role = TrackRole::Overlay;
        fresh.segments.append(seg);
        d.tracks.append(fresh);
        return true;
    });
    if (!ok) return {};
    setSelectedId(seg.id);
    return seg.id;
}

QStringList Project::effectNames() const
{
    QStringList names = mtg::effectNames();
    names.removeAll(QStringLiteral("lut"));  // needs a file; offered separately
    return names;
}

QStringList Project::momentNames() const
{
    return mtg::momentNames();
}

void Project::setTransition(const QString &segmentId, const QString &name, double seconds)
{
    if (!name.isEmpty() && !mtg::transitionNames().contains(name)) return;
    edit([&](Draft &d) {
        QList<Segment> &main = d.mainTrack().segments;
        for (qsizetype i = 1; i < main.size(); ++i) {
            if (main[i].id != segmentId) continue;
            main[i].transition.name = name;
            if (seconds > 0) main[i].transition.us = qint64(seconds * 1e6);
            return true;
        }
        return false;  // not on the main track, or the first clip
    });
}

void Project::setFraming(const QString &segmentId, const QString &framing)
{
    if (!mtg::framingNames().contains(framing)) return;
    edit([&](Draft &d) {
        Track *owner = nullptr;
        Segment *s = d.segment(segmentId, &owner);
        if (!s || owner->role != TrackRole::Main || s->framing == framing) return false;
        s->framing = framing;
        return true;
    });
}

QString Project::beatSync() const
{
    return m_draft.beatSync;
}

void Project::setBeatSync(const QString &mode)
{
    const QString m = mode == QLatin1String("beats") || mode == QLatin1String("bars") ? mode : QString();
    int moved = 0;
    const QString music = musicSegment(m_draft);
    edit([&](Draft &d) {
        if (d.beatSync == m && m.isEmpty()) return false;
        d.beatSync = m;
        if (!m.isEmpty() && !music.isEmpty()) moved = snapCutsToBeats(d, music, m);
        return true;
    });
    if (m.isEmpty()) return;
    if (music.isEmpty()) emit message(QStringLiteral("Add music to cut to the beat"));
    else emit message(moved ? QStringLiteral("Moved %1 cut%2 onto the %3").arg(moved).arg(moved == 1 ? "" : "s").arg(m == QLatin1String("bars") ? "bars" : "beat")
                            : QStringLiteral("The cuts are already on the beat"));
}

void Project::autoCaptions(const QString &style)
{
    if (m_captioning) return;
    if (const QString problem = whisperProblem(); !problem.isEmpty()) {
        emit message(problem);
        return;
    }
    m_captioning = true;
    emit captioningChanged();
    struct Job {
        Draft draft;
        int count = 0;
        QString error;
    };
    auto *job = new Job{m_draft};
    QThread *worker = QThread::create([job, style] { job->count = addCaptions(job->draft, style, &job->error); });
    connect(worker, &QThread::finished, this, [this, job, worker, style] {
        m_captioning = false;
        emit captioningChanged();
        if (job->count > 0) {
            // Bring over just the captions: the timeline may have changed meanwhile.
            edit([&](Draft &d) {
                removeCaptions(d);
                for (const Track &t : job->draft.tracks) {
                    bool captions = !t.segments.isEmpty();
                    for (const Segment &s : t.segments) captions &= s.origin == QLatin1String("captions");
                    if (captions) d.tracks.append(t);
                }
                d.ensureTextMaterial();
                d.captions = style;
                d.normalize();
                return true;
            });
            emit message(QStringLiteral("Added %1 caption%2").arg(job->count).arg(job->count == 1 ? "" : "s"));
        } else {
            emit message(job->error.isEmpty() ? QStringLiteral("No speech found to caption") : job->error);
        }
        delete job;
        worker->deleteLater();
    });
    worker->start();
}

void Project::clearCaptions()
{
    edit([](Draft &d) {
        const bool had = removeCaptions(d) > 0 || !d.captions.isEmpty();
        d.captions.clear();
        return had;
    });
}

QStringList Project::framingNames() const
{
    return mtg::framingNames();
}

QStringList Project::transitionNames() const
{
    return mtg::transitionNames();
}

void Project::setSpeed(const QString &segmentId, double speed, const QString &curve)
{
    if (!curve.isEmpty() && !speedCurveNames().contains(curve)) return;
    edit([&](Draft &d) {
        Segment *s = d.segment(segmentId);
        const Material *m = s ? d.material(s->materialId) : nullptr;
        if (!s || !m || m->kind != MaterialKind::Video) return false;
        speed = std::clamp(speed, 0.1, 8.0);
        // Keep the footage the clip shows; the timeline length follows.
        const qint64 span = s->speed > 0 ? s->sourceSpan() : s->duration;
        const qint64 available = m->durationUs - s->sourceStart;
        s->duration = std::max(kMinSegmentUs, qint64(double(std::min(span, available)) / speed));
        s->speed = speed;
        s->curve = curve;
        return true;
    });
}

QStringList Project::speedCurves() const
{
    return speedCurveNames();
}

bool Project::freezeFrame(qint64 t, double seconds)
{
    QString targetId;
    for (const Segment &s : m_draft.tracks[0].segments)
        if (s.contains(t) && t - s.start >= kMinSegmentUs / 2) targetId = s.id;
    if (targetId.isEmpty()) return false;
    const QString freezeId = Draft::newId();
    const bool ok = edit([&](Draft &d) {
        QList<Segment> &main = d.mainTrack().segments;
        for (qsizetype i = 0; i < main.size(); ++i) {
            if (main[i].id != targetId) continue;
            Segment &left = main[i];
            const qint64 local = t - left.start;
            Segment hold = left;
            hold.id = freezeId;
            hold.sourceStart = left.sourceAt(local);
            hold.speed = 0;
            hold.curve.clear();
            hold.duration = qint64(std::clamp(seconds, 0.1, 30.0) * 1e6);
            hold.motion = Motion{};
            hold.keys.clear();
            hold.transition = Transition{};
            Segment right = left;
            right.id = Draft::newId();
            right.sourceStart = hold.sourceStart;
            right.duration = left.duration - local;
            right.motion.in.clear();
            right.transition = Transition{};
            right.keys.clear();
            left.duration = local;
            left.motion.out.clear();
            left.curve.clear();
            right.curve.clear();
            // Decide before inserting: inserting can reallocate and invalidate `left`.
            const bool keepLeft = left.duration >= kMinSegmentUs;
            const bool keepRight = right.duration >= kMinSegmentUs;
            main.insert(i + 1, hold);
            if (keepRight) main.insert(i + 2, right);
            if (!keepLeft) main.removeAt(i);
            return true;
        }
        return false;
    });
    if (ok) setSelectedId(freezeId);
    return ok;
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
