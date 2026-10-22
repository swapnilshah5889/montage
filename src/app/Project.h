#pragma once

#include "model/Draft.h"

#include <QObject>
#include <QQmlEngine>
#include <QUrl>
#include <QVariant>
#include <optional>

// The editing API the QML UI talks to. Every edit goes through edit(), which
// snapshots the draft for undo, normalizes it and notifies listeners.
class Project : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString name READ name WRITE setName NOTIFY changed)
    Q_PROPERTY(QVariantList materials READ materials NOTIFY changed)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY changed)
    Q_PROPERTY(qint64 duration READ duration NOTIFY changed)
    Q_PROPERTY(int canvasWidth READ canvasWidth NOTIFY changed)
    Q_PROPERTY(int canvasHeight READ canvasHeight NOTIFY changed)
    Q_PROPERTY(double fps READ fps NOTIFY changed)
    Q_PROPERTY(QString aspect READ aspect NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    Q_PROPERTY(bool dirty READ dirty NOTIFY changed)
    Q_PROPERTY(QUrl fileUrl READ fileUrl NOTIFY changed)
    Q_PROPERTY(QString selectedId READ selectedId WRITE setSelectedId NOTIFY selectionChanged)
    Q_PROPERTY(QVariantMap selection READ selection NOTIFY selectionChanged)
    Q_PROPERTY(QString beatSync READ beatSync NOTIFY changed)
    Q_PROPERTY(bool captioning READ captioning NOTIFY captioningChanged)
    // What's said on the main track, word by word, for the transcript panel.
    Q_PROPERTY(QVariantList transcript READ transcript NOTIFY changed)

public:
    // Not default-constructible on purpose: QML would otherwise build its
    // own instance instead of using create().
    explicit Project(QObject *parent);
    static Project *instance() { return s_instance; }
    static Project *create(QQmlEngine *, QJSEngine *engine);

    const mtg::Draft &draft() const { return m_draft; }

    QString name() const { return m_draft.name; }
    void setName(const QString &name);
    QVariantList materials() const;
    QVariantList tracks() const;
    qint64 duration() const { return m_draft.duration(); }
    int canvasWidth() const { return m_draft.canvas.width; }
    int canvasHeight() const { return m_draft.canvas.height; }
    double fps() const { return m_draft.canvas.fps; }
    QString aspect() const;
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool dirty() const { return m_dirty; }
    QUrl fileUrl() const { return m_file; }
    QString selectedId() const { return m_selected; }
    void setSelectedId(const QString &id);
    QVariantMap selection() const;

    // Media
    Q_INVOKABLE int importFiles(const QList<QUrl> &urls);
    Q_INVOKABLE void removeMaterial(const QString &materialId);

    // Timeline edits. trackId may be "new-overlay" / "new-audio"; at < 0
    // means "end of the main track".
    Q_INVOKABLE QString addToTimeline(const QString &materialId, const QString &trackId = {}, qint64 at = -1);
    Q_INVOKABLE void moveSegment(const QString &segmentId, const QString &trackId, qint64 start);
    Q_INVOKABLE void trimSegment(const QString &segmentId, qint64 newStart, qint64 newDuration);
    Q_INVOKABLE bool splitAt(qint64 t);
    Q_INVOKABLE void deleteSelected();

    // Inspector edits. commit=false applies live (slider drag) and records
    // one undo step when the matching commit=true call arrives.
    // `at` is the playhead (timeline time): when the property is keyframed,
    // the keyframe there is edited (or added) instead of the resting value.
    Q_INVOKABLE void setSegmentProperty(const QString &segmentId, const QString &key, double value, bool commit = true,
                                        qint64 at = -1);
    Q_INVOKABLE void resetTransform(const QString &segmentId);

    // Motion presets (slot: in | out | loop; empty name removes) and keyframes.
    Q_INVOKABLE void setMotion(const QString &segmentId, const QString &slot, const QString &name, double seconds = 0);
    Q_INVOKABLE QStringList motionNames(const QString &slot) const;
    // Records the clip's current position, scale, rotation and opacity as a
    // keyframe at timeline time `t`.
    Q_INVOKABLE void addKeyframe(const QString &segmentId, qint64 t);
    Q_INVOKABLE void clearKeyframes(const QString &segmentId);

    // GPU effects on a clip, and effect moments (adjustment layers that
    // affect everything beneath them for a while).
    Q_INVOKABLE void addEffect(const QString &segmentId, const QString &name, double amount = 1.0);
    Q_INVOKABLE void removeEffect(const QString &segmentId, int index);
    Q_INVOKABLE void setEffectAmount(const QString &segmentId, int index, double amount, bool commit = true);
    Q_INVOKABLE QString addEffectLayer(const QString &name, qint64 at, qint64 duration = 0);
    Q_INVOKABLE QStringList effectNames() const;
    Q_INVOKABLE QStringList momentNames() const;

    // Transition into a main-track clip from the one before it (empty name removes).
    Q_INVOKABLE void setTransition(const QString &segmentId, const QString &name, double seconds = 0);
    // Main track: how a clip of another shape fills the frame (auto, fill, fit, fit-blur).
    Q_INVOKABLE void setFraming(const QString &segmentId, const QString &framing);
    Q_INVOKABLE QStringList framingNames() const;

    // Smart edit: cut to the music ("beats", "bars", or "" for off), and
    // captions of what's said (transcribed locally, in the background).
    // Media details (like the scene index) changed outside an edit.
    void mediaInfoChanged();
    QString beatSync() const;
    Q_INVOKABLE void setBeatSync(const QString &mode);
    bool captioning() const { return m_captioning; }
    Q_INVOKABLE void autoCaptions(const QString &style);
    Q_INVOKABLE void clearCaptions();

    // Text-based editing.
    QVariantList transcript() const;
    // Cuts [start, end) out of the main track (words selected in the transcript).
    Q_INVOKABLE void cutRange(qint64 start, qint64 end);
    // Cuts fillers (um, uh) and/or long pauses from every talking clip.
    Q_INVOKABLE void cleanSpeechNow(bool fillers, bool pauses);
    Q_INVOKABLE QStringList transitionNames() const;

    // Speed keeps the clip's source footage and changes its length (faster =
    // shorter). curve: "" | ramp-up | ramp-down | hero | bullet.
    Q_INVOKABLE void setSpeed(const QString &segmentId, double speed, const QString &curve = {});
    Q_INVOKABLE QStringList speedCurves() const;
    // Holds the main-track frame at timeline time `t` for `seconds`.
    Q_INVOKABLE bool freezeFrame(qint64 t, double seconds = 2.0);

    // Text clips. Presets: title, subtitle, caption, lower-third, end-card.
    Q_INVOKABLE QString addText(const QString &text, qint64 at = 0, qint64 duration = 0,
                                const QString &preset = QStringLiteral("title"));
    Q_INVOKABLE void setTextProperty(const QString &segmentId, const QString &key, const QVariant &value,
                                     bool commit = true);
    Q_INVOKABLE void applyTextPreset(const QString &segmentId, const QString &preset);
    Q_INVOKABLE QStringList textPresets() const;
    Q_INVOKABLE QStringList textShapes() const;
    Q_INVOKABLE void setAspect(const QString &aspect);

    // Replaces the whole timeline in one undoable edit (used by the assistant).
    bool applyDraft(const mtg::Draft &draft);

    // Group edits (e.g. one assistant request) into a single undo step.
    void beginUndoGroup();
    void endUndoGroup();

    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    Q_INVOKABLE void newProject();
    Q_INVOKABLE bool save(const QUrl &url = {});
    Q_INVOKABLE bool load(const QUrl &url);

    // Snap `t` to the playhead or a segment edge within `threshold`.
    Q_INVOKABLE qint64 snap(qint64 t, qint64 threshold, qint64 playhead, const QString &excludeSegment = {}) const;
    Q_INVOKABLE QString formatTime(qint64 us, bool frames = true) const;

signals:
    void changed();      // structure changed: rebuild views
    void liveChanged();  // values changed during a drag: re-render only
    void selectionChanged();
    void message(const QString &text);
    void captioningChanged();

private:
    template <typename F>
    bool edit(F &&fn);
    template <typename F>
    void liveOrCommit(const QString &segmentId, bool commit, F &&apply);
    void place(mtg::Draft &d, mtg::Segment seg, const QString &trackId, mtg::TrackRole role) const;
    void applyCanvasFromFirstClip(mtg::Draft &d) const;
    void notify();

    mtg::Draft m_draft;
    QList<mtg::Draft> m_undo;
    QList<mtg::Draft> m_redo;
    std::optional<mtg::Draft> m_liveEditBase;
    int m_groupDepth = 0;
    bool m_groupPushed = false;
    QString m_selected;
    bool m_captioning = false;
    QUrl m_file;
    bool m_dirty = false;
    QString m_thumbDir;

    static inline Project *s_instance = nullptr;
};
