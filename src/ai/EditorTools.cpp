#include "EditorTools.h"

#include "AdStyles.h"
#include "Brand.h"
#include "Finishing.h"
#include "engine/Scenes.h"
#include "EditPlan.h"
#include "Generators.h"
#include "PlanGuards.h"
#include "app/Exporter.h"
#include "app/Project.h"
#include "engine/Renderer.h"
#include "engine/VideoDecoder.h"

#include <QBuffer>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>
#include <cmath>

namespace mtg::ai {

namespace {

const QStringList kMediaExts{"mp4", "mov", "m4v", "mkv", "webm", "avi", "mts", "mp3", "wav", "m4a", "aac",
                             "flac", "ogg", "png", "jpg", "jpeg", "webp", "heic", "heif", "gif", "bmp", "tif", "tiff"};

bool isMediaFile(const QFileInfo &fi)
{
    return fi.isFile() && kMediaExts.contains(fi.suffix().toLower()) && !fi.fileName().startsWith(QLatin1Char('.'));
}

QString expandHome(QString path)
{
    path = path.trimmed();
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/"))) path.replace(0, 1, QDir::homePath());
    return QDir::cleanPath(path);
}

QString clock(double s)
{
    const int m = int(s) / 60;
    return QStringLiteral("%1:%2").arg(m).arg(s - m * 60, 4, 'f', 1, QLatin1Char('0'));
}

ToolResult error(const ToolCall &c, const QString &text)
{
    return ToolResult{c.id, c.name, text, {}, true};
}

QJsonObject schema(QJsonObject props, QJsonArray required = {})
{
    return QJsonObject{{"type", "object"}, {"properties", props}, {"required", required}, {"additionalProperties", false}};
}

}  // namespace

QByteArray contactSheet(const QList<QPair<QImage, QString>> &frames, int cellWidth)
{
    if (frames.isEmpty()) return {};
    const int cols = frames.size() <= 1 ? 1 : frames.size() <= 4 ? 2 : 3;
    const int rows = int((frames.size() + cols - 1) / cols);
    const QImage &first = frames.first().first;
    const double aspect = first.isNull() ? 16.0 / 9 : double(first.width()) / first.height();
    const int cellH = int(std::lround(cellWidth / std::clamp(aspect, 0.4, 3.0)));
    const int gap = 4;

    QImage sheet(cols * cellWidth + (cols - 1) * gap, rows * cellH + (rows - 1) * gap, QImage::Format_RGB32);
    sheet.fill(QColor(24, 24, 28));
    QPainter p(&sheet);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    QFont font;
    font.setPixelSize(15);
    font.setBold(true);
    p.setFont(font);
    for (int i = 0; i < frames.size(); ++i) {
        const QRect cell((i % cols) * (cellWidth + gap), (i / cols) * (cellH + gap), cellWidth, cellH);
        const QImage &img = frames[i].first;
        if (!img.isNull()) {
            const QSize s = img.size().scaled(cell.size(), Qt::KeepAspectRatio);
            p.drawImage(QRect(cell.center() - QPoint(s.width() / 2, s.height() / 2), s), img);
        }
        const QString label = frames[i].second;
        const QRect box(cell.left() + 6, cell.bottom() - 26, p.fontMetrics().horizontalAdvance(label) + 12, 22);
        p.fillRect(box, QColor(0, 0, 0, 190));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, label);
    }
    p.end();

    QByteArray jpeg;
    QBuffer buf(&jpeg);
    buf.open(QIODevice::WriteOnly);
    sheet.save(&buf, "JPG", 78);
    return jpeg;
}

EditorTools::EditorTools(Project *project, Exporter *exporter, QObject *parent)
    : QObject(parent)
    , m_project(project)
    , m_exporter(exporter)
    , m_generators(new Generators(this))
{
}

QList<ToolSpec> EditorTools::specs(bool vision) const
{
    QList<ToolSpec> out;
    out << ToolSpec{
        "get_project",
        "Returns the media list (ids like m1), a timeline summary and the current edit plan. Call this when you need "
        "the exact current plan before changing it, or to re-check the project after the user edited it by hand.",
        schema({})};
    out << ToolSpec{
        "apply_plan",
        "Replaces the whole timeline with the edit plan you pass, as one undoable step. Call this for every change to "
        "the video: start from the current plan, modify it, and pass the complete result. Returns errors (nothing "
        "changed) or adjustments the editor made, plus a summary of the new timeline. Times are seconds. main = clips "
        "played back to back (from/to are seconds inside the file); overlays = picture-in-picture shown at 'at'; audio "
        "= music or sound starting at 'at'; texts = on-screen text. Set export to true only if the user asked for a file.",
        schema({{"plan", planSchema()}}, {"plan"})};
    if (vision) {
        out << ToolSpec{
            "look_at_media",
            "Shows frames sampled across one video (or the photo itself) with timestamps, so you can see what each "
            "file contains. Call this before choosing shots or trims when the request depends on content (best "
            "moments, people, scenery, matching a mood), and to find exact in/out points.",
            schema({{"media", QJsonObject{{"type", "string"}, {"description", "Media id, e.g. m1"}}},
                    {"count", QJsonObject{{"type", "integer"}, {"description", "Frames to sample, 1-12 (default 6)"}}},
                    {"from", QJsonObject{{"type", "number"}, {"description", "Optional: start of the range to sample, seconds"}}},
                    {"to", QJsonObject{{"type", "number"}, {"description", "Optional: end of the range to sample, seconds"}}}},
                   {"media"})};
        out << ToolSpec{
            "render_preview",
            "Renders frames of the edited video at the given timeline times (seconds), exactly as the export will look. "
            "Call this after apply_plan to check framing, text placement and legibility before you finish.",
            schema({{"times", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "number"}}},
                                          {"description", "Up to 8 timeline times in seconds"}}}},
                   {"times"})};
    }
    out << ToolSpec{
        "find_moments",
        "Searches the scene index of every imported video and photo for shots matching a description (\"dog running on "
        "the beach\", \"close-up of the product\", \"people laughing\") and returns each match with its file, shot "
        "number, time range, description and quality. Use it to find footage by what it shows.",
        schema({{"query", QJsonObject{{"type", "string"}}}}, {"query"})};
    out << ToolSpec{
        "find_media",
        "Lists video, audio and image files in a folder (and its direct subfolders). Call this when the user mentions "
        "a folder or asks what files they have. Accepts ~ for the home folder.",
        schema({{"folder", QJsonObject{{"type", "string"}}}}, {"folder"})};
    out << ToolSpec{
        "import_media",
        "Imports media files, or every media file in a folder, into the project and returns their ids. Call this before "
        "using files that aren't in the media list yet.",
        schema({{"paths", QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}}}}, {"paths"})};
    out << ToolSpec{
        "generate_media",
        "Makes new media with the local AI models and imports it, returning the new media ids: an image (product "
        "backgrounds, b-roll stills; describe the scene, no text in it), a voiceover (the exact words to say, with a "
        "voice: af_heart or af_bella female US, am_michael or am_puck male US, bf_emma female UK, bm_george male UK) "
        "or music (instrumental, from a description of genre, instruments and mood, plus its length in seconds). "
        "Takes 10-60 seconds per item. Use it when the user asks for a voiceover, music or a picture they don't "
        "have; prefer their own media otherwise. You can also put the same items in a plan's \"generate\" list.",
        schema({{"items", QJsonObject{{"type", "array"},
                                      {"items", QJsonObject{{"type", "object"},
                                                            {"properties", QJsonObject{{"kind", QJsonObject{{"type", "string"}, {"enum", QJsonArray::fromStringList(generateKinds())}}},
                                                                                       {"prompt", QJsonObject{{"type", "string"}, {"description", "Image or music description"}}},
                                                                                       {"text", QJsonObject{{"type", "string"}, {"description", "Voiceover words"}}},
                                                                                       {"voice", QJsonObject{{"type", "string"}, {"enum", QJsonArray::fromStringList(voiceNames())}}},
                                                                                       {"seconds", QJsonObject{{"type", "number"}, {"description", "Music length"}}},
                                                                                       {"aspect", QJsonObject{{"type", "string"}, {"description", "Image shape, e.g. 9:16; defaults to the video's"}}}}},
                                                            {"required", QJsonArray{"kind"}}}}}}},
               {"items"})};
    out << ToolSpec{
        "export_video",
        "Renders the timeline to an MP4 in the user's Movies/Montage folder and returns the file path. Only call this "
        "when the user asks to export, render, save or download the video. For several platforms at once (\"for TikTok "
        "and YouTube\", \"all formats\") pass formats: each is a separate file laid out for that shape, with clips "
        "reframed around their subject and text re-placed.",
        schema({{"name", QJsonObject{{"type", "string"}, {"description", "File name without extension"}}},
                {"formats", QJsonObject{{"type", "array"},
                                        {"items", QJsonObject{{"type", "string"}, {"enum", QJsonArray::fromStringList(exportFormatNames())}}},
                                        {"description", "Shapes to export, e.g. [\"9:16\", \"1:1\", \"16:9\"]. Omit for the current shape only."}}}})};
    return out;
}

QString EditorTools::projectState(bool withPlan) const
{
    const Draft &d = m_project->draft();
    const MediaIndex index(d);
    QString s = QStringLiteral("MEDIA:\n%1\n\nTIMELINE:\n%2").arg(describeMedia(d, index), summarize(d, index));
    if (withPlan)
        s += QStringLiteral("\n\nCURRENT PLAN:\n") + QString::fromUtf8(QJsonDocument(planFromDraft(d, index)).toJson(QJsonDocument::Compact));
    return s;
}

void EditorTools::run(const ToolCall &call, bool vision, const std::function<void(ToolResult)> &done)
{
    if (!call.inputOk) {
        // Return the raw arguments so the model can see what went wrong.
        const QJsonObject err{{"INVALID_JSON", call.rawInput}};
        done(error(call, QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Compact))));
        return;
    }
    const QString &n = call.name;
    if (n == QLatin1String("get_project")) {
        done(ToolResult{call.id, n, projectState(true)});
    } else if (n == QLatin1String("apply_plan")) {
        const QJsonValue planValue = call.input.value("plan");
        const QJsonObject plan = planValue.isString() ? QJsonDocument::fromJson(planValue.toString().toUtf8()).object() : planValue.toObject();
        if (plan.value("generate").toArray().isEmpty()) {
            applyAndExport(call, {}, done);
            return;
        }
        resolveGenerated(plan, [this, call, done](const GenResolution &r) {
            if (r.cancelled) {
                done(error(call, QStringLiteral("Generation was cancelled; nothing was changed.")));
                return;
            }
            const Draft &d = m_project->draft();
            const MediaIndex index(d);
            QJsonObject resolved = r.plan;
            QStringList notes = r.errors;
            notes += placeGenerated(r.generated, m_request, d, index, &resolved);
            if (duckUnderVoiceover(m_request, d, index, &resolved)) notes << QStringLiteral("the music dips while the voiceover speaks");
            QStringList made;
            for (auto it = r.made.cbegin(); it != r.made.cend(); ++it) made << QStringLiteral("%1 is now %2").arg(it.key(), it.value());
            if (!made.isEmpty()) notes.prepend(QStringLiteral("generated media: ") + made.join(", "));
            ToolCall c = call;
            c.input.insert("plan", resolved);
            applyAndExport(c, notes, done);
        });
    } else if (n == QLatin1String("generate_media")) {
        generateMedia(call, done);
    } else if (n == QLatin1String("look_at_media") && vision) {
        done(lookAtMedia(call));
    } else if (n == QLatin1String("render_preview") && vision) {
        done(renderPreview(call));
    } else if (n == QLatin1String("find_moments")) {
        done(findMoments(call));
    } else if (n == QLatin1String("find_media")) {
        done(findMedia(call));
    } else if (n == QLatin1String("import_media")) {
        done(importMedia(call));
    } else if (n == QLatin1String("export_video")) {
        const QString name = call.input.value("name").toString(m_project->name());
        QStringList formats;
        for (const QJsonValue &v : call.input.value("formats").toArray()) formats << v.toString();
        exportVideo(name, formats, [call, done](bool ok, const QString &msg) {
            done(ToolResult{call.id, call.name, ok ? QStringLiteral("Exported to ") + msg : msg, {}, !ok});
        });
    } else {
        done(error(call, QStringLiteral("Unknown tool \"%1\".").arg(n)));
    }
}

void EditorTools::applyAndExport(const ToolCall &call, const QStringList &notes, const std::function<void(ToolResult)> &done)
{
    applyPlan(call, [this, call, notes, done](ToolResult r) {
        if (!notes.isEmpty() && !r.isError) r.text = QStringLiteral("Also: ") + notes.join("; ") + QStringLiteral("\n") + r.text;
        const bool wantsExport = !r.isError && call.input.value("plan").toObject().value("export").toBool();
        if (!wantsExport) {
            done(r);
            return;
        }
        QStringList formats;
        for (const QJsonValue &v : call.input.value("plan").toObject().value("export_formats").toArray()) formats << v.toString();
        exportVideo(m_project->name(), formats, [r, done](bool ok, const QString &msg) mutable {
            r.text += QStringLiteral("\n\n") + (ok ? QStringLiteral("Exported to ") + msg : QStringLiteral("Export failed: ") + msg);
            done(r);
        });
    });
}

void EditorTools::resolveGenerated(const QJsonObject &plan, const std::function<void(const GenResolution &)> &done)
{
    m_generators->resolve(plan, m_project->draft(),
                          [this](const QStringList &paths) -> const Draft & {
                              QList<QUrl> urls;
                              for (const QString &p : paths) urls << QUrl::fromLocalFile(p);
                              if (!urls.isEmpty()) m_project->importFiles(urls);  // no-op for files already imported
                              return m_project->draft();
                          },
                          done);
}

void EditorTools::cancelGeneration()
{
    m_generators->cancel();
}

void EditorTools::generateMedia(const ToolCall &call, const std::function<void(ToolResult)> &done)
{
    QJsonArray items;
    for (const QJsonValue &v : call.input.value("items").toArray()) {
        QJsonObject o = v.toObject();
        o.remove("as");  // named g1, g2... in order
        items.append(o);
    }
    QStringList warnings;
    if (generateRequests(QJsonObject{{"generate", items}}, &warnings).isEmpty()) {
        done(error(call, QStringLiteral("Nothing to generate. ") + warnings.join(' ')));
        return;
    }
    resolveGenerated(QJsonObject{{"generate", items}}, [this, call, done](const GenResolution &r) {
        if (r.cancelled) {
            done(error(call, QStringLiteral("Generation was cancelled.")));
            return;
        }
        const Draft &d = m_project->draft();
        const MediaIndex index(d);
        const QStringList lines = describeMedia(d, index).split('\n');
        QStringList made;
        for (const QString &alias : r.generated) made << lines.value(int(index.materialIds().indexOf(index.materialId(alias))));
        QString text = made.isEmpty() ? QString() : QStringLiteral("Generated and imported:\n") + made.join('\n');
        if (!r.errors.isEmpty()) text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + QStringLiteral("Failed:\n") + r.errors.join('\n');
        done(ToolResult{call.id, call.name, text, {}, made.isEmpty()});
    });
}

void EditorTools::applyPlan(const ToolCall &call, const std::function<void(ToolResult)> &done)
{
    const QJsonValue planValue = call.input.value("plan");
    QJsonObject plan = planValue.toObject();
    if (planValue.isString())  // some models send the plan as a JSON string
        plan = QJsonDocument::fromJson(planValue.toString().toUtf8()).object();
    if (plan.isEmpty()) {
        done(error(call, QStringLiteral("\"plan\" must be an edit plan object.")));
        return;
    }

    const Draft &current = m_project->draft();
    const MediaIndex index(current);
    const QJsonObject before = planFromDraft(current, index);
    QStringList styled = resolveShots(current, index, &plan);
    styled += arrangeLines(m_request, current, index, &plan);
    styled += resolveLines(current, index, &plan);
    styled += applySpeechRequests(m_request, current, index, &plan);
    styled += pickBestMoments(m_request, current, index, &plan);
    styled += applySmartRequests(m_request, current, index, &plan);
    styled += applyBrand(m_request, loadBrandKit(), before, current, index, &plan);
    styled += applyAdStyle(before, current, index, &plan);
    PlanReport report;
    const Draft next = draftFromPlan(current, plan, index, &report);
    if (!report.ok()) {
        done(error(call, report.text()));
        return;
    }
    report.warnings = styled + report.warnings;
    auto apply = [this, call, done, report](const Draft &built, const QStringList &notes, const QStringList &problems) mutable {
        report.warnings += notes;
        report.warnings += problems;
        m_project->applyDraft(built);
        const Draft &now = m_project->draft();
        QString text = report.warnings.isEmpty() ? QStringLiteral("Applied.") : report.text();
        text += QStringLiteral("\n\n") + summarize(now, MediaIndex(now));
        done(ToolResult{call.id, call.name, text});
    };
    if (needsFinishing(next)) finishAsync(next, apply);
    else apply(next, {}, {});
}

void EditorTools::finishAsync(const Draft &draft, const std::function<void(const Draft &, QStringList, QStringList)> &done)
{
    struct Result {
        Draft draft;
        QStringList notes, problems;
    };
    auto *result = new Result{draft, {}, {}};
    QThread *worker = QThread::create([result] { result->notes = finishDraft(result->draft, &result->problems); });
    connect(worker, &QThread::finished, this, [worker, result, done] {
        done(result->draft, result->notes, result->problems);
        delete result;
        worker->deleteLater();
    });
    worker->start();
}

ToolResult EditorTools::findMoments(const ToolCall &call) const
{
    const QString query = call.input.value("query").toString().trimmed();
    if (query.isEmpty()) return error(call, QStringLiteral("Give a query describing the shot."));
    const Draft &d = m_project->draft();
    const MediaIndex index(d);
    QStringList lines;
    int unindexed = 0;
    for (const QString &id : index.materialIds()) {
        const Material *m = d.material(id);
        if (m->kind != MaterialKind::Video && m->kind != MaterialKind::Image) continue;
        const SceneIndex x = cachedSceneIndex(*m);
        if (x.isEmpty()) {
            ++unindexed;
            continue;
        }
        for (const ShotMatch &hit : findShots(x, query, 0.25)) {
            const Shot &s = x.shots[hit.shot];
            lines << QStringLiteral("%1 shot %2: %3-%4s \"%5\" (%6, quality %7, match %8)")
                         .arg(index.alias(id))
                         .arg(hit.shot + 1)
                         .arg(s.start / 1e6, 0, 'f', 1)
                         .arg(s.end / 1e6, 0, 'f', 1)
                         .arg(s.description.isEmpty() ? s.tags.join(", ") : s.description, s.framing.isEmpty() ? QStringLiteral("-") : s.framing)
                         .arg(s.score(), 0, 'f', 2)
                         .arg(hit.match, 0, 'f', 2);
        }
    }
    QString text = lines.isEmpty() ? QStringLiteral("No shots match \"%1\".").arg(query) : lines.mid(0, 12).join('\n');
    if (unindexed) text += QStringLiteral("\n(%1 file(s) still being indexed.)").arg(unindexed);
    return ToolResult{call.id, call.name, text};
}

ToolResult EditorTools::lookAtMedia(const ToolCall &call) const
{
    const Draft &d = m_project->draft();
    const MediaIndex index(d);
    const QString alias = call.input.value("media").toString();
    const Material *m = d.material(index.materialId(alias));
    if (!m) return error(call, QStringLiteral("Unknown media \"%1\".\n%2").arg(alias, describeMedia(d, index)));
    if (m->kind == MaterialKind::Audio) return error(call, alias + QStringLiteral(" is audio; there is nothing to see."));

    QList<QPair<QImage, QString>> frames;
    if (m->kind == MaterialKind::Image) {
        QImageReader reader(m->path);
        reader.setAutoTransform(true);
        frames.append({reader.read(), m->name});
    } else {
        VideoDecoder dec;
        if (!dec.open(m->path)) return error(call, QStringLiteral("Could not open ") + m->name);
        const double len = m->durationUs / 1e6;
        const double from = std::clamp(call.input.value("from").toDouble(0), 0.0, len);
        const double to = std::clamp(call.input.value("to").toDouble(len), from, len);
        const int count = std::clamp(call.input.value("count").toInt(6), 1, 12);
        const QSize size = dec.displaySize().scaled(400, 400, Qt::KeepAspectRatio);
        for (int i = 0; i < count; ++i) {
            const double t = from + (to - from) * (i + 0.5) / count;
            frames.append({dec.frameAt(qint64(t * 1e6), size), alias + QStringLiteral(" @ ") + clock(t)});
        }
    }
    QString info = QStringLiteral("%1: %2").arg(alias, describeMedia(d, index).split('\n').value(int(index.materialIds().indexOf(m->id))));
    return ToolResult{call.id, call.name, info, {contactSheet(frames)}};
}

ToolResult EditorTools::renderPreview(const ToolCall &call) const
{
    const Draft &d = m_project->draft();
    if (d.duration() <= 0) return error(call, QStringLiteral("The timeline is empty."));
    Renderer renderer;
    renderer.setDraft(d);
    const QSize size = renderer.fitCanvas(QSize(480, 480));
    QList<QPair<QImage, QString>> frames;
    const QJsonArray times = call.input.value("times").toArray();
    for (int i = 0; i < std::min<int>(8, int(times.size())); ++i) {
        const double t = std::clamp(seconds(times[i]), 0.0, d.duration() / 1e6 - 0.001);
        if (std::isnan(t)) continue;
        frames.append({renderer.renderVideo(qint64(t * 1e6), size), clock(t)});
    }
    if (frames.isEmpty()) return error(call, QStringLiteral("Pass times in seconds, e.g. [0, 2.5, 6]."));
    return ToolResult{call.id, call.name, QStringLiteral("Frames of the edited video (timeline times)."), {contactSheet(frames, size.width())}};
}

ToolResult EditorTools::findMedia(const ToolCall &call) const
{
    const QString folder = expandHome(call.input.value("folder").toString());
    QFileInfo root(folder);
    if (!root.isDir()) return error(call, QStringLiteral("Folder not found: ") + folder);
    QStringList lines;
    QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext() && lines.size() < 200) {
        const QFileInfo fi(it.next());
        if (QDir(folder).relativeFilePath(fi.absoluteFilePath()).count(QLatin1Char('/')) > 1) continue;  // one level deep
        if (isMediaFile(fi)) lines << QStringLiteral("%1  (%2 MB)").arg(fi.absoluteFilePath()).arg(fi.size() / 1e6, 0, 'f', 1);
    }
    lines.sort();
    return ToolResult{call.id, call.name, lines.isEmpty() ? QStringLiteral("No media files in ") + folder : lines.join('\n')};
}

QStringList EditorTools::importMentionedPaths(const QString &text)
{
    static const QRegularExpression re(QStringLiteral(R"((?:~|/(?:Users|Volumes|tmp|private))[^\s"'`,;]*)"));
    QList<QUrl> urls;
    QStringList names;
    for (auto it = re.globalMatch(text); it.hasNext();) {
        QString path = expandHome(it.next().captured(0));
        while (!QFileInfo::exists(path) && (path.endsWith(QLatin1Char('.')) || path.endsWith(QLatin1Char(')'))))
            path.chop(1);
        const QFileInfo fi(path);
        if (fi.isDir()) {
            QDirIterator dit(path, QDir::Files, QDirIterator::NoIteratorFlags);
            while (dit.hasNext() && urls.size() < 100) {
                const QFileInfo f(dit.next());
                if (isMediaFile(f)) urls << QUrl::fromLocalFile(f.absoluteFilePath());
            }
        } else if (isMediaFile(fi)) {
            urls << QUrl::fromLocalFile(fi.absoluteFilePath());
        }
    }
    if (urls.isEmpty()) return {};
    const int before = int(MediaIndex(m_project->draft()).materialIds().size());
    m_project->importFiles(urls);
    const Draft &d = m_project->draft();
    const MediaIndex index(d);
    for (int i = before; i < index.materialIds().size(); ++i) names << d.material(index.materialIds()[i])->name;
    return names;
}

ToolResult EditorTools::importMedia(const ToolCall &call)
{
    QStringList mentioned;
    for (const QJsonValue &v : call.input.value("paths").toArray()) mentioned << QLatin1Char(' ') + v.toString();
    const QStringList names = importMentionedPaths(mentioned.join(QString()));
    const Draft &d = m_project->draft();
    const QString media = describeMedia(d, MediaIndex(d));
    if (names.isEmpty())
        return ToolResult{call.id, call.name, QStringLiteral("Nothing new imported (missing, unsupported or already imported).\nMEDIA:\n") + media};
    return ToolResult{call.id, call.name, QStringLiteral("Imported %1 file(s).\nMEDIA:\n%2").arg(names.size()).arg(media)};
}

QList<QByteArray> EditorTools::mediaSheets(int maxFiles) const
{
    const Draft &d = m_project->draft();
    const MediaIndex index(d);
    QList<QByteArray> out;
    for (const QString &id : index.materialIds()) {
        if (out.size() >= maxFiles) break;
        const Material *m = d.material(id);
        if (!m->isVisual()) continue;
        ToolCall c;
        c.name = QStringLiteral("look_at_media");
        c.input = QJsonObject{{"media", index.alias(id)}, {"count", 4}};
        const ToolResult r = lookAtMedia(c);
        out += r.jpegs;
    }
    return out;
}

void EditorTools::exportVideo(const QString &name, const QStringList &formats, const std::function<void(bool, QString)> &done)
{
    if (m_project->duration() <= 0) {
        done(false, QStringLiteral("The timeline is empty."));
        return;
    }
    if (m_exporter->running()) {
        done(false, QStringLiteral("Another export is already running."));
        return;
    }
    QStringList shapes;
    for (const QString &f : formats)
        if (exportFormatNames().contains(f) && !shapes.contains(f)) shapes << f;
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation) + QStringLiteral("/Montage");
    QDir().mkpath(dir);
    QString base = name;
    base.replace(QRegularExpression(QStringLiteral(R"([/\\:*?"<>|]+)")), QStringLiteral("-"));
    if (base.trimmed().isEmpty()) base = QStringLiteral("Montage");
    QString path = dir + QLatin1Char('/') + base + QStringLiteral(".mp4");
    auto taken = [&](const QString &p) {
        for (const QString &out : Exporter::outputPaths(p, shapes))
            if (QFileInfo::exists(out)) return true;
        return false;
    };
    for (int i = 2; taken(path); ++i) path = QStringLiteral("%1/%2 (%3).mp4").arg(dir, base).arg(i);

    auto *conn = new QMetaObject::Connection;
    *conn = connect(m_exporter, &Exporter::finished, this,
                    [conn, done](bool ok, const QString &message, const QUrl &, const QStringList &files) {
                        QObject::disconnect(*conn);
                        delete conn;
                        done(ok, ok ? files.join('\n') : message);
                    });
    m_exporter->start(QUrl::fromLocalFile(path), shapes);
}

}  // namespace mtg::ai
