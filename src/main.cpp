#include "app/Agent.h"
#include "app/AgentCli.h"
#include "app/Exporter.h"
#include "app/Player.h"
#include "app/Project.h"
#include "engine/ExportJob.h"
#include "engine/Renderer.h"
#include "ai/EditPlan.h"

#include <QCommandLineParser>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QKeyEvent>
#include <QQuickItem>
#include <QTimer>
#include <cstdio>

// Command-line hooks for scripted runs: build a timeline from files, export
// without a window, or render the UI to a PNG.
static void populate(Project &project, const QStringList &mainFiles, const QStringList &overlays)
{
    auto importOne = [&](const QString &file) -> QString {
        const QString path = QFileInfo(file).absoluteFilePath();
        project.importFiles({QUrl::fromLocalFile(path)});
        for (const QVariant &v : project.materials()) {
            const QVariantMap m = v.toMap();
            if (m.value("path").toString() == path) return m.value("id").toString();
        }
        return {};
    };
    for (const QString &f : mainFiles)
        if (const QString id = importOne(f); !id.isEmpty()) project.addToTimeline(id);
    qint64 at = 1'000'000;
    for (const QString &f : overlays) {
        if (const QString id = importOne(f); !id.isEmpty()) {
            const QString seg = project.addToTimeline(id, QStringLiteral("new-overlay"), at);
            project.setSegmentProperty(seg, QStringLiteral("scale"), 0.4);
            project.setSegmentProperty(seg, QStringLiteral("x"), 0.5);
            project.setSegmentProperty(seg, QStringLiteral("y"), -0.45);
            at += 1'000'000;
        }
    }
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("Montage"));
    QGuiApplication::setOrganizationName(QStringLiteral("Montage"));
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QCommandLineParser cli;
    cli.addHelpOption();
    const QCommandLineOption openOpt("open", "Open a .montage project.", "project");
    const QCommandLineOption importOpt("import", "Import a file onto the main/audio track.", "file");
    const QCommandLineOption overlayOpt("overlay", "Import a file as a picture-in-picture overlay.", "file");
    const QCommandLineOption saveOpt("save", "Save the project after loading.", "project");
    const QCommandLineOption exportOpt("export", "Export to MP4 without showing a window, then quit.", "mp4");
    const QCommandLineOption shotOpt("screenshot", "Render the window to a PNG, then quit.", "png");
    const QCommandLineOption seekOpt("seek", "Initial playhead position in seconds.", "seconds");
    const QCommandLineOption selectOpt("select-first", "Select the first main-track clip.");
    const QCommandLineOption playOpt("play", "Start playback once the window is up.");
    const QCommandLineOption textOpt("text", "Add a text clip at the start: <preset>:<text>.", "preset:text");
    const QCommandLineOption profileOpt("profile", "Assistant model profile id (e.g. ollama, claude).", "id");
    const QCommandLineOption agentOpt("agent", "Send a request to the assistant (repeatable), then quit.", "request");
    const QCommandLineOption evalOpt("eval", "Run an assistant eval file, then quit.", "cases.json");
    const QCommandLineOption mediaOpt("media", "Media folder for --eval.", "dir");
    const QCommandLineOption onlyOpt("only", "Run only eval cases whose name contains this.", "text");
    const QCommandLineOption typeOpt("ui-type", "Type into the chat box and press Enter once the window is up.", "text");
    const QCommandLineOption waitOpt("ui-wait", "Seconds before --screenshot is taken (default 3).", "seconds");
    const QCommandLineOption quitOpt("quit", "Quit after loading, importing and saving (no window).");
    const QCommandLineOption frameOpt("frame", "Render one frame at <seconds> to --frame-out, then quit.", "seconds");
    const QCommandLineOption frameOutOpt("frame-out", "PNG path for --frame.", "png");
    const QCommandLineOption planOpt("plan", "Apply an edit plan (JSON) to the project.", "plan.json");
    const QCommandLineOption formatsOpt("formats", "With --export: one file per shape, e.g. 9:16,1:1,16:9.", "shapes");
    const QCommandLineOption openUiOpt("ui-open", "Open popups / check controls by objectName (comma-separated) once the window is up.", "names");
    cli.addOptions({typeOpt, waitOpt, quitOpt, frameOpt, frameOutOpt, planOpt, openUiOpt, formatsOpt});
    cli.addOptions({openOpt, importOpt, overlayOpt, saveOpt, exportOpt, shotOpt, seekOpt, selectOpt, playOpt, textOpt,
                    profileOpt, agentOpt, evalOpt, mediaOpt, onlyOpt});
    cli.process(app);

    Project project(&app);
    Player player(&project);
    Exporter exporter(&project);
    Agent agent(&project, &exporter, &app);
    if (cli.isSet(profileOpt)) agent.setProfileId(cli.value(profileOpt));

    QObject::connect(&project, &Project::message, [](const QString &m) { std::fprintf(stderr, "%s\n", qPrintable(m)); });
    if (cli.isSet(openOpt)) project.load(QUrl::fromLocalFile(QFileInfo(cli.value(openOpt)).absoluteFilePath()));
    populate(project, cli.values(importOpt), cli.values(overlayOpt));
    for (const QString &t : cli.values(textOpt))
        project.addText(t.section(QLatin1Char(':'), 1).replace(QStringLiteral("\\n"), QStringLiteral("\n")), 0,
                        project.duration() > 0 ? project.duration() : 0, t.section(QLatin1Char(':'), 0, 0));

    if (cli.isSet(evalOpt))
        return AgentCli::runEval(agent, project, cli.value(evalOpt), cli.value(mediaOpt), cli.value(onlyOpt));
    if (cli.isSet(agentOpt)) {
        const int rc = AgentCli::runRequests(agent, project, cli.values(agentOpt));
        if (cli.isSet(saveOpt)) project.save(QUrl::fromLocalFile(QFileInfo(cli.value(saveOpt)).absoluteFilePath()));
        if (!cli.isSet(shotOpt)) return rc;
    }
    if (cli.isSet(planOpt)) {
        QFile f(cli.value(planOpt));
        if (!f.open(QIODevice::ReadOnly)) {
            std::fprintf(stderr, "cannot read %s\n", qPrintable(cli.value(planOpt)));
            return 2;
        }
        mtg::ai::PlanReport report;
        const mtg::Draft next = mtg::ai::draftFromPlan(project.draft(), QJsonDocument::fromJson(f.readAll()).object(),
                                                       mtg::ai::MediaIndex(project.draft()), &report);
        if (!report.text().isEmpty()) std::fprintf(stderr, "%s\n", qPrintable(report.text()));
        if (!report.ok()) return 1;
        project.applyDraft(next);
    }
    if (cli.isSet(saveOpt)) project.save(QUrl::fromLocalFile(QFileInfo(cli.value(saveOpt)).absoluteFilePath()));
    if (cli.isSet(frameOpt)) {
        mtg::Renderer renderer;
        renderer.setDraft(project.draft());
        const QImage img = renderer.renderVideo(qint64(cli.value(frameOpt).toDouble() * 1e6), renderer.fitCanvas(QSize(960, 960)));
        return img.save(cli.value(frameOutOpt)) ? 0 : 1;
    }
    if (cli.isSet(quitOpt) && !cli.isSet(exportOpt)) return 0;

    if (cli.isSet(exportOpt)) {
        const QStringList formats = cli.value(formatsOpt).split(QLatin1Char(','), Qt::SkipEmptyParts);
        const QStringList paths = Exporter::outputPaths(cli.value(exportOpt), formats);
        for (qsizetype i = 0; i < paths.size(); ++i) {
            const mtg::Draft d = formats.isEmpty() ? project.draft() : mtg::ai::resizedDraft(project.draft(), formats[i]);
            const mtg::ExportResult res = mtg::exportDraft(d, paths[i], [](double p) {
                std::fprintf(stderr, "\rexport %3d%%", int(p * 100));
                return true;
            });
            std::fprintf(stderr, "\n%s: %s\n", qPrintable(paths[i]), qPrintable(res.message));
            if (!res.ok) return 1;
        }
        return 0;
    }

    if (cli.isSet(seekOpt)) player.seek(qint64(cli.value(seekOpt).toDouble() * 1e6));
    if (cli.isSet(selectOpt) && !project.draft().tracks[0].segments.isEmpty())
        project.setSelectedId(project.draft().tracks[0].segments.first().id);

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule("Montage", "Main");

    if (cli.isSet(playOpt)) QTimer::singleShot(500, &player, &Player::play);
    // UI test hook: real key events into the chat box, as a person would type.
    if (cli.isSet(typeOpt) && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const QString text = cli.value(typeOpt);
        QTimer::singleShot(1000, &app, [window, text] {
            auto *input = window ? window->findChild<QQuickItem *>(QStringLiteral("chatInput")) : nullptr;
            if (!input) {
                std::fprintf(stderr, "ui-type: chat input not found\n");
                return;
            }
            input->forceActiveFocus(Qt::MouseFocusReason);
            std::fprintf(stderr, "ui-type: focus on %s\n", qPrintable(window->activeFocusItem() ? window->activeFocusItem()->objectName() : QStringLiteral("(none)")));
            for (const QChar c : text) {
                QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
                QKeyEvent release(QEvent::KeyRelease, 0, Qt::NoModifier, QString(c));
                QCoreApplication::sendEvent(window, &press);
                QCoreApplication::sendEvent(window, &release);
            }
            std::fprintf(stderr, "ui-type: field now holds \"%s\"\n", qPrintable(input->property("text").toString()));
            QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
            QCoreApplication::sendEvent(window, &enter);
            std::fprintf(stderr, "ui-type: after Enter the field holds \"%s\", agent busy=%d\n",
                         qPrintable(input->property("text").toString()), Agent::instance()->busy());
        });
    }

    // UI test hook: open a popup (or check a button) by objectName.
    if (cli.isSet(openUiOpt) && !engine.rootObjects().isEmpty()) {
        QObject *root = engine.rootObjects().first();
        const QStringList names = cli.value(openUiOpt).split(QLatin1Char(','), Qt::SkipEmptyParts);
        QTimer::singleShot(800, &app, [root, names] {
            for (const QString &name : names) {
                QObject *obj = root->findChild<QObject *>(name);
                if (!obj) {
                    std::fprintf(stderr, "ui-open: %s not found\n", qPrintable(name));
                    continue;
                }
                if (obj->metaObject()->indexOfMethod("open()") >= 0) QMetaObject::invokeMethod(obj, "open");
                else obj->setProperty("checked", true);
            }
        });
    }

    if (cli.isSet(shotOpt) && !engine.rootObjects().isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        const QString out = cli.value(shotOpt);
        const int waitMs = int((cli.isSet(waitOpt) ? cli.value(waitOpt).toDouble() : 3.0) * 1000);
        QTimer::singleShot(waitMs, &app, [window, out] {
            const bool ok = window && window->grabWindow().save(out);
            std::fprintf(stderr, "%s %s (playhead %.3fs, playing %d)\n", ok ? "saved" : "failed to save",
                         qPrintable(out), Player::instance()->position() / 1e6, Player::instance()->playing());
            QCoreApplication::exit(ok ? 0 : 1);
        });
    }
    return app.exec();
}
