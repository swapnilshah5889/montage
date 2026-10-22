#include "app/Agent.h"
#include "app/AgentCli.h"
#include "app/Exporter.h"
#include "app/Player.h"
#include "app/Project.h"
#include "app/Setup.h"
#include "engine/Beats.h"
#include "engine/Scenes.h"
#include "engine/Speech.h"
#include "engine/TextEdit.h"
#include "engine/ExportJob.h"
#include "engine/MediaProbe.h"
#include "engine/Renderer.h"
#include "ai/EditPlan.h"
#include "ai/Finishing.h"
#include "ai/MediaIndexer.h"
#include "ai/Generators.h"

#include <QCommandLineParser>
#include <QEventLoop>
#include <QJsonArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
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
    const QCommandLineOption planOpt("plan", "Apply an edit plan (JSON) to the project; its \"generate\" items are made first.", "plan.json");
    const QCommandLineOption scenesOpt("scenes", "Print the scene index (shots, quality, labels) of a video or photo, then quit.", "file");
    const QCommandLineOption describeOpt("describe", "With --scenes: also describe each shot with the local vision model.");
    const QCommandLineOption transcriptOpt("transcript", "Print what's said in a file (lines and fillers), then quit.", "file");
    const QCommandLineOption cleanOpt("clean-speech", "Cut fillers (um, uh) and long pauses from the project's clips before saving or exporting.");
    const QCommandLineOption iconOpt("render-icon", "Draw the app icon (1024 px PNG), then quit.", "png");
    const QCommandLineOption beatsOpt("beats", "Print the beat grid of an audio or video file, then quit.", "file");
    const QCommandLineOption formatsOpt("formats", "With --export: one file per shape, e.g. 9:16,1:1,16:9.", "shapes");
    const QCommandLineOption openUiOpt("ui-open", "Open popups / check controls by objectName (comma-separated) once the window is up.", "names");
    cli.addOptions({typeOpt, waitOpt, quitOpt, frameOpt, frameOutOpt, planOpt, openUiOpt, formatsOpt, beatsOpt, scenesOpt, describeOpt, transcriptOpt, cleanOpt, iconOpt});
    cli.addOptions({openOpt, importOpt, overlayOpt, saveOpt, exportOpt, shotOpt, seekOpt, selectOpt, playOpt, textOpt,
                    profileOpt, agentOpt, evalOpt, mediaOpt, onlyOpt});
    cli.process(app);

    Project project(&app);
    Player player(&project);
    Exporter exporter(&project);
    Agent agent(&project, &exporter, &app);
    Setup setup(&agent, &app);
    if (cli.isSet(scenesOpt)) {
        mtg::Material m;
        QString err;
        if (!mtg::probeMedia(QFileInfo(cli.value(scenesOpt)).absoluteFilePath(), QDir::tempPath(), &m, &err)) {
            std::fprintf(stderr, "%s\n", qPrintable(err));
            return 1;
        }
        QElapsedTimer timer;
        timer.start();
        mtg::SceneIndex x = mtg::sceneIndex(m);
        if (cli.isSet(describeOpt)) {
            mtg::ai::MediaIndexer indexer;
            mtg::Draft d;
            d.materials << m;
            indexer.watch(d);
            while (indexer.busy()) QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 100);
            x = mtg::cachedSceneIndex(m);
        }
        std::printf("%d shot(s) in %.1fs\n", int(x.shots.size()), timer.elapsed() / 1000.0);
        for (qsizetype i = 0; i < x.shots.size(); ++i) {
            const mtg::Shot &s = x.shots[i];
            const auto best = s.bestWindow(2'000'000);
            std::printf("[%d] %.2f-%.2fs sharp %.2f steady %.2f exposure %.2f motion %.2f faces %d score %.2f best2s %.2f-%.2f tags: %s%s\n",
                        int(i + 1), s.start / 1e6, s.end / 1e6, s.sharpness, s.stability, s.exposure, s.motion, s.faces, s.score(),
                        best.first / 1e6, best.second / 1e6, qPrintable(s.tags.join(", ")),
                        s.description.isEmpty() ? "" : qPrintable(QStringLiteral(" | \"%1\" %2 appeal %3").arg(s.description, s.framing).arg(s.appeal)));
        }
        return x.isEmpty() ? 1 : 0;
    }
    if (cli.isSet(iconOpt)) {
        // macOS app icon: a rounded tile (824 px in a 1024 canvas, Apple's grid),
        // teal to indigo, with a film strip's sprocket edges and a play triangle.
        QImage img(1024, 1024, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::transparent);
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF tile(100, 100, 824, 824);
        QPainterPath shape;
        shape.addRoundedRect(tile, 185, 185);
        p.fillPath(shape.translated(0, 12), QColor(0, 0, 0, 70));  // soft drop shadow
        QLinearGradient g(tile.topLeft(), tile.bottomRight());
        g.setColorAt(0, QColor("#1FD1C1"));
        g.setColorAt(1, QColor("#3B3FD8"));
        p.fillPath(shape, g);
        p.setClipPath(shape);
        // Sprocket holes down both sides.
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 60));
        for (int i = 0; i < 6; ++i) {
            const double y = 190 + i * 118;
            p.drawRoundedRect(QRectF(150, y, 56, 72), 14, 14);
            p.drawRoundedRect(QRectF(818, y, 56, 72), 14, 14);
        }
        // Play triangle with rounded corners.
        QPainterPath play;
        play.moveTo(430, 330);
        play.lineTo(430, 694);
        play.lineTo(720, 512);
        play.closeSubpath();
        p.setBrush(Qt::white);
        p.setPen(QPen(Qt::white, 70, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPath(play);
        p.end();
        return img.save(cli.value(iconOpt)) ? 0 : 1;
    }
    if (cli.isSet(transcriptOpt)) {
        mtg::Material m;
        QString err;
        if (!mtg::probeMedia(QFileInfo(cli.value(transcriptOpt)).absoluteFilePath(), QDir::tempPath(), &m, &err)) {
            std::fprintf(stderr, "%s\n", qPrintable(err));
            return 1;
        }
        const mtg::Transcript t = mtg::fileTranscript(m);
        if (!t.ok()) {
            std::fprintf(stderr, "%s\n", qPrintable(t.error));
            return 1;
        }
        int fillers = 0, pauses = 0;
        const auto cuts = mtg::speechCuts(m, t, 0, m.durationUs, {}, &fillers, &pauses);
        const QList<mtg::Line> lines = mtg::transcriptLines(t);
        std::printf("%d words, %d lines, %d fillers, %d long pauses (language %s)\n", int(t.words.size()), int(lines.size()), fillers, pauses,
                    qPrintable(t.language));
        for (qsizetype i = 0; i < lines.size(); ++i)
            std::printf("%d) %.2f-%.2fs %s\n", int(i + 1), lines[i].start / 1e6, lines[i].end / 1e6, qPrintable(lines[i].text));
        for (const auto &c : cuts) std::printf("cut %.2f-%.2fs\n", c.first / 1e6, c.second / 1e6);
        return 0;
    }
    if (cli.isSet(beatsOpt)) {
        mtg::Material m;
        QString err;
        if (!mtg::probeMedia(QFileInfo(cli.value(beatsOpt)).absoluteFilePath(), QDir::tempPath(), &m, &err)) {
            std::fprintf(stderr, "%s\n", qPrintable(err));
            return 1;
        }
        const mtg::BeatGrid g = mtg::beatGrid(m);
        QStringList beats, bars, accents;
        for (double b : g.beats) beats << QString::number(b, 'f', 2);
        for (double b : g.downbeats) bars << QString::number(b, 'f', 2);
        for (double b : g.accents) accents << QString::number(b, 'f', 2);
        std::printf("bpm %.1f\nbeats %s\nbars %s\naccents %s\n", g.bpm, qPrintable(beats.join(' ')), qPrintable(bars.join(' ')),
                    qPrintable(accents.join(' ')));
        return g.isEmpty() ? 1 : 0;
    }
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
        QJsonObject plan = QJsonDocument::fromJson(f.readAll()).object();
        if (!plan.value("generate").toArray().isEmpty()) {  // make the media it asks for first
            mtg::ai::Generators generators;
            QObject::connect(&generators, &mtg::ai::Generators::itemProgress, [](int i, double p, const QString &note) {
                std::fprintf(stderr, "\rgenerate %d: %3d%% %-40s", i + 1, int(p * 100), qPrintable(note.left(40)));
            });
            QEventLoop loop;
            generators.resolve(plan, project.draft(),
                               [&](const QStringList &paths) -> const mtg::Draft & {
                                   QList<QUrl> urls;
                                   for (const QString &p : paths) urls << QUrl::fromLocalFile(p);
                                   project.importFiles(urls);
                                   return project.draft();
                               },
                               [&](const mtg::ai::GenResolution &r) {
                                   std::fprintf(stderr, "\n");
                                   for (const QString &e : r.errors) std::fprintf(stderr, "%s\n", qPrintable(e));
                                   plan = r.plan;
                                   loop.quit();
                               });
            loop.exec();
        }
        mtg::ai::PlanReport report;
        mtg::Draft next = mtg::ai::draftFromPlan(project.draft(), plan, mtg::ai::MediaIndex(project.draft()), &report);
        if (!report.text().isEmpty()) std::fprintf(stderr, "%s\n", qPrintable(report.text()));
        if (!report.ok()) return 1;
        if (mtg::ai::needsFinishing(next)) {  // beat sync, captions
            QStringList problems;
            const QStringList notes = mtg::ai::finishDraft(next, &problems);
            for (const QString &n : notes + problems) std::fprintf(stderr, "%s\n", qPrintable(n));
        }
        project.applyDraft(next);
    }
    if (cli.isSet(cleanOpt)) {
        mtg::Draft d = project.draft();
        for (const mtg::Material &m : d.materials)
            if (m.hasAudio && m.kind != mtg::MaterialKind::Text) mtg::fileTranscript(m);  // transcribe first
        int fillers = 0, pauses = 0;
        const qint64 removed = mtg::cleanSpeech(d, {}, &fillers, &pauses);
        std::fprintf(stderr, "cut %d fillers and %d pauses: %.2fs shorter\n", fillers, pauses, removed / 1e6);
        project.applyDraft(d);
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

    // Interactive session: autosave for crash recovery, cleared on a normal quit.
    project.enableAutosave();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &project, &Project::endSession);

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
            for (const QString &spec : names) {
                // "object.property=value" sets a property; otherwise open/click/check the object.
                const QString name = spec.section(QLatin1Char('.'), 0, 0).section(QLatin1Char('='), 0, 0);
                QObject *obj = root->findChild<QObject *>(name);
                if (obj && spec.contains(QLatin1Char('='))) {
                    obj->setProperty(qPrintable(spec.section(QLatin1Char('.'), 1).section(QLatin1Char('='), 0, 0)), spec.section(QLatin1Char('='), 1));
                    continue;
                }
                if (!obj) {
                    std::fprintf(stderr, "ui-open: %s not found\n", qPrintable(spec));
                    continue;
                }
                if (obj->metaObject()->indexOfMethod("open()") >= 0) QMetaObject::invokeMethod(obj, "open");
                else if (obj->metaObject()->indexOfMethod("click()") >= 0) QMetaObject::invokeMethod(obj, "click");  // buttons and tabs
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
