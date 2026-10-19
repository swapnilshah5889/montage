#pragma once

#include "ChatProvider.h"

#include <QImage>
#include <QObject>
#include <functional>

class Project;
class Exporter;

namespace mtg::ai {

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

    void exportVideo(const QString &name, const std::function<void(bool, QString)> &done);

private:
    ToolResult lookAtMedia(const ToolCall &call) const;
    ToolResult renderPreview(const ToolCall &call) const;
    ToolResult applyPlan(const ToolCall &call);
    ToolResult findMedia(const ToolCall &call) const;
    ToolResult importMedia(const ToolCall &call);

    Project *m_project;
    Exporter *m_exporter;
};

// Lays frames out in a labelled grid and encodes it as JPEG.
QByteArray contactSheet(const QList<QPair<QImage, QString>> &frames, int cellWidth = 320);

}  // namespace mtg::ai
