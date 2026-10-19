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
    Q_INVOKABLE void setSegmentProperty(const QString &segmentId, const QString &key, double value, bool commit = true);
    Q_INVOKABLE void resetTransform(const QString &segmentId);
    Q_INVOKABLE void setAspect(const QString &aspect);

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

private:
    template <typename F>
    bool edit(F &&fn);
    void place(mtg::Draft &d, mtg::Segment seg, const QString &trackId, mtg::TrackRole role) const;
    void applyCanvasFromFirstClip(mtg::Draft &d) const;
    void notify();

    mtg::Draft m_draft;
    QList<mtg::Draft> m_undo;
    QList<mtg::Draft> m_redo;
    std::optional<mtg::Draft> m_liveEditBase;
    QString m_selected;
    QUrl m_file;
    bool m_dirty = false;
    QString m_thumbDir;

    static inline Project *s_instance = nullptr;
};
