#pragma once

#include "model/Draft.h"
#include "ChatProvider.h"

#include <QImage>
#include <QObject>
#include <functional>

class Project;
class Exporter;

namespace mtg::ai {

class Generators;
struct GenResolution;

// The editor's capabilities as model tools. All edits go through Project, so
// they show up live and undo like manual edits.
class EditorTools : public QObject
{
    Q_OBJECT
public:
    EditorTools(Project *project, Exporter *exporter, QObject *parent = nullptr);

    QList<ToolSpec> specs(bool vision) const;

    // Runs one call; `done` is called exactly once (export finishes later).
    void run(const ToolCall &call, bool vision, const std::function<void(ToolResult)> &done);

    // Project state block given to the model with each request.
    QString projectState(bool withPlan) const;

    // Imports media files and folders named in free text (e.g. "~/Movies/Trip").
    QStringList importMentionedPaths(const QString &text);

    // Frames sampled from the first few videos/photos, one sheet per file.
    QList<QByteArray> mediaSheets(int maxFiles) const;

    // The user's request being worked on (brand kit and styles key off it).
    void setRequest(const QString &request) { m_request = request; }

    // Exports to Movies/Montage. With `formats`, one file per shape. `done`
    // gets the written paths (one per line) or the error.
    void exportVideo(const QString &name, const QStringList &formats, const std::function<void(bool, QString)> &done);

    // Makes the media a plan's "generate" list asks for, imports it and binds
    // the plan's references to it. Progress comes from generators().
    void resolveGenerated(const QJsonObject &plan, const std::function<void(const GenResolution &)> &done);
    void cancelGeneration();

    // Runs the finishing steps a built draft asks for (beat sync, captions)
    // on a worker thread; `done` gets the result, notes and problems on this one.
    void finishAsync(const Draft &draft, const std::function<void(const Draft &, QStringList, QStringList)> &done);
    Generators *generators() const { return m_generators; }

private:
    ToolResult lookAtMedia(const ToolCall &call) const;
    ToolResult renderPreview(const ToolCall &call) const;
    void applyPlan(const ToolCall &call, const std::function<void(ToolResult)> &done);
    ToolResult findMedia(const ToolCall &call) const;
    ToolResult findMoments(const ToolCall &call) const;
    ToolResult importMedia(const ToolCall &call);
    void generateMedia(const ToolCall &call, const std::function<void(ToolResult)> &done);
    void applyAndExport(const ToolCall &call, const QStringList &notes, const std::function<void(ToolResult)> &done);

    Project *m_project;
    Exporter *m_exporter;
    Generators *m_generators;
    QString m_request;
};

// Lays frames out in a labelled grid and encodes it as JPEG.
QByteArray contactSheet(const QList<QPair<QImage, QString>> &frames, int cellWidth = 320);

}  // namespace mtg::ai
