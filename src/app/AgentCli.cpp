#include "AgentCli.h"

#include "Agent.h"
#include "Project.h"
#include "ai/EditPlan.h"

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>

using namespace mtg;
using namespace mtg::ai;

namespace {

void out(const QString &s)
{
    std::fprintf(stdout, "%s\n", qPrintable(s));
    std::fflush(stdout);
}

// Sends one request and waits for the agent to finish, echoing new chat rows.
bool ask(Agent &agent, const QString &request, double *seconds)
{
    ChatModel *chat = agent.messages();
    const int firstRow = chat->rowCount();
    QEventLoop loop;
    bool ok = false;
    auto conn = QObject::connect(&agent, &Agent::finished, &loop, [&](bool success) {
        ok = success;
        loop.quit();
    });
    QElapsedTimer timer;
    timer.start();
    agent.send(request);
    if (agent.busy()) loop.exec();
    QObject::disconnect(conn);
    if (seconds) *seconds = timer.elapsed() / 1000.0;
    for (int row = firstRow; row < chat->rowCount(); ++row) {
        const QModelIndex i = chat->index(row);
        out(QStringLiteral("  [%1] %2").arg(chat->data(i, ChatModel::KindRole).toString(), chat->data(i, ChatModel::TextRole).toString()));
    }
    return ok;
}

// ------------------------------------------------------------ checks

struct Ctx {
    const Draft &d;
    QStringList mainNames;
    double length;
};

QString mediaName(const Draft &d, const Segment &s)
{
    const Material *m = d.material(s.materialId);
    return m ? m->name : QString();
}

QList<const Segment *> segmentsOn(const Draft &d, TrackRole role, bool texts)
{
    QList<const Segment *> out;
    for (const Track &t : d.tracks)
        if (t.role == role)
            for (const Segment &s : t.segments)
                if ((s.materialId == kTextMaterialId) == texts) out << &s;
    return out;
}

// Returns an empty string when the check passes, otherwise why it failed.
QString check(const QJsonObject &c, const Ctx &x)
{
    const QString type = c.value("type").toString();
    const double eps = 0.05;
    if (type == QLatin1String("length")) {
        const double lo = c.value("min").toDouble(0), hi = c.value("max").toDouble(1e9);
        return x.length >= lo - eps && x.length <= hi + eps ? QString() : QStringLiteral("length %1s not in [%2, %3]").arg(x.length).arg(lo).arg(hi);
    }
    if (type == QLatin1String("aspect")) {
        const QString want = c.value("value").toString();
        const int w = want.section(':', 0, 0).toInt(), h = want.section(':', 1, 1).toInt();
        const bool ok = w > 0 && h > 0 && std::abs(double(x.d.canvas.width) / x.d.canvas.height - double(w) / h) < 0.01;
        return ok ? QString() : QStringLiteral("canvas %1x%2 is not %3").arg(x.d.canvas.width).arg(x.d.canvas.height).arg(want);
    }
    if (type == QLatin1String("background")) {
        const QColor want(c.value("value").toString()), have(x.d.canvas.background);
        return want == have ? QString() : QStringLiteral("background %1, wanted %2").arg(x.d.canvas.background, c.value("value").toString());
    }
    if (type == QLatin1String("main_count")) {
        const int n = int(x.mainNames.size());
        const int lo = c.value("min").toInt(0), hi = c.value("max").toInt(1000);
        return n >= lo && n <= hi ? QString() : QStringLiteral("%1 main clips, wanted %2-%3").arg(n).arg(lo).arg(hi);
    }
    if (type == QLatin1String("main_order")) {
        QStringList want;
        for (const QJsonValue &v : c.value("media").toArray()) want << v.toString();
        QStringList seen;  // order of first appearance
        for (const QString &n : x.mainNames)
            if (!seen.contains(n)) seen << n;
        return seen.mid(0, want.size()) == want ? QString() : QStringLiteral("order %1, wanted %2").arg(seen.join(","), want.join(","));
    }
    if (type == QLatin1String("first_main")) {
        return !x.mainNames.isEmpty() && x.mainNames.first() == c.value("media").toString()
                   ? QString() : QStringLiteral("first clip is %1").arg(x.mainNames.value(0));
    }
    if (type == QLatin1String("media_used") || type == QLatin1String("media_not_used")) {
        const QString name = c.value("media").toString();
        bool used = false;
        for (const Track &t : x.d.tracks)
            for (const Segment &s : t.segments) used |= mediaName(x.d, s) == name;
        const bool want = type == QLatin1String("media_used");
        return used == want ? QString() : QStringLiteral("%1 is%2 used").arg(name, used ? "" : " not");
    }
    if (type == QLatin1String("clip_max_len")) {
        for (const Segment &s : x.d.tracks[0].segments)
            if (s.duration / 1e6 > c.value("max").toDouble() + eps) return QStringLiteral("a main clip is %1s").arg(s.duration / 1e6);
        return {};
    }
    if (type == QLatin1String("text")) {
        const QString want = c.value("contains").toString();
        for (const Segment *s : segmentsOn(x.d, TrackRole::Overlay, true)) {
            if (!s->text.text.contains(want, Qt::CaseInsensitive)) continue;
            if (c.contains("color")) {
                const QColor col(s->text.color);
                const QString hue = c.value("color").toString();
                const bool ok = hue == QLatin1String("red") ? (col.red() > 170 && col.green() < 110 && col.blue() < 110) : col == QColor(hue);
                if (!ok) return QStringLiteral("text \"%1\" has color %2").arg(want, s->text.color);
            }
            if (c.contains("start_max") && s->start / 1e6 > c.value("start_max").toDouble() + eps)
                return QStringLiteral("text starts at %1s").arg(s->start / 1e6);
            if (c.value("ends_with_video").toBool() && std::abs(s->end() / 1e6 - x.length) > 0.35)
                return QStringLiteral("text ends at %1s, video at %2s").arg(s->end() / 1e6).arg(x.length);
            return {};
        }
        return QStringLiteral("no text containing \"%1\"").arg(want);
    }
    if (type == QLatin1String("text_count")) {
        const int n = int(segmentsOn(x.d, TrackRole::Overlay, true).size());
        return n >= c.value("min").toInt(0) && n <= c.value("max").toInt(1000) ? QString() : QStringLiteral("%1 texts").arg(n);
    }
    if (type == QLatin1String("audio")) {
        const QString name = c.value("media").toString();
        for (const Segment *s : segmentsOn(x.d, TrackRole::Audio, false)) {
            if (mediaName(x.d, *s) != name) continue;
            if (c.contains("max_volume") && s->volume > c.value("max_volume").toDouble() + 0.001)
                return QStringLiteral("music volume %1").arg(s->volume);
            if (c.contains("min_volume") && s->volume < c.value("min_volume").toDouble() - 0.001)
                return QStringLiteral("music volume %1").arg(s->volume);
            if (c.value("covers_video").toBool() && (s->start > 100'000 || s->end() / 1e6 < x.length - 0.3))
                return QStringLiteral("music covers %1-%2s of %3s").arg(s->start / 1e6).arg(s->end() / 1e6).arg(x.length);
            return {};
        }
        return QStringLiteral("no audio track uses %1").arg(name);
    }
    if (type == QLatin1String("overlay")) {
        const QString name = c.value("media").toString();
        for (const Segment *s : segmentsOn(x.d, TrackRole::Overlay, false)) {
            if (mediaName(x.d, *s) != name) continue;
            if (c.value("covers_video").toBool() && s->duration / 1e6 < x.length * 0.9)
                return QStringLiteral("overlay shows for %1s of %2s").arg(s->duration / 1e6).arg(x.length);
            if (c.contains("corner")) {
                const QString corner = c.value("corner").toString();
                const bool right = s->transform.x > 0.2, top = s->transform.y < -0.2;
                const bool ok = (corner.contains("right") == right) && (corner.contains("top") == top);
                if (!ok) return QStringLiteral("overlay at x=%1 y=%2").arg(s->transform.x).arg(s->transform.y);
            }
            return {};
        }
        return QStringLiteral("no overlay uses %1").arg(name);
    }
    return QStringLiteral("unknown check type ") + type;
}

}  // namespace

namespace AgentCli {

int runRequests(Agent &agent, Project &project, const QStringList &requests)
{
    bool allOk = true;
    for (const QString &r : requests) {
        out(QStringLiteral("> ") + r);
        double secs = 0;
        allOk &= ask(agent, r, &secs);
        out(QStringLiteral("  (%1s)").arg(secs, 0, 'f', 1));
    }
    const Draft &d = project.draft();
    out(QStringLiteral("\n") + summarize(d, MediaIndex(d)));
    return allOk ? 0 : 1;
}

int runEval(Agent &agent, Project &project, const QString &casesFile, const QString &mediaDir, const QString &only)
{
    QFile f(casesFile);
    if (!f.open(QIODevice::ReadOnly)) {
        out(QStringLiteral("Cannot read ") + casesFile);
        return 2;
    }
    const QJsonArray cases = QJsonDocument::fromJson(f.readAll()).object().value("cases").toArray();
    int passed = 0, total = 0;
    double totalSecs = 0;
    QStringList failedNames;
    out(QStringLiteral("Model: %1 (%2, %3 mode)\n")
            .arg(agent.profileMap().value("model").toString(), agent.profileMap().value("name").toString(),
                 agent.profileMap().value("guided").toBool() ? "guided" : "agent"));

    for (const QJsonValue &cv : cases) {
        const QJsonObject c = cv.toObject();
        const QString name = c.value("name").toString();
        if (!only.isEmpty() && !name.contains(only)) continue;
        ++total;
        project.newProject();
        agent.newChat();
        QList<QUrl> urls;
        for (const QJsonValue &m : c.value("media").toArray()) urls << QUrl::fromLocalFile(QDir(mediaDir).filePath(m.toString()));
        project.importFiles(urls);
        if (c.contains("setup")) {  // starting timeline, as a plan using m1.. in media order
            PlanReport report;
            const Draft d = draftFromPlan(project.draft(), c.value("setup").toObject(), MediaIndex(project.draft()), &report);
            if (!report.ok()) out(QStringLiteral("  setup errors: ") + report.errors.join("; "));
            project.applyDraft(d);
        }

        out(QStringLiteral("== %1").arg(name));
        double caseSecs = 0;
        bool finished = true;
        for (const QJsonValue &r : c.value("requests").toArray()) {
            out(QStringLiteral("> ") + r.toString());
            double secs = 0;
            finished &= ask(agent, r.toString(), &secs);
            caseSecs += secs;
        }
        totalSecs += caseSecs;

        const Draft &d = project.draft();
        Ctx ctx{d, {}, d.duration() / 1e6};
        for (const Segment &s : d.tracks[0].segments) ctx.mainNames << mediaName(d, s);
        QStringList problems;
        if (!finished) problems << QStringLiteral("agent did not finish");
        for (const QJsonValue &chk : c.value("checks").toArray()) {
            const QString why = check(chk.toObject(), ctx);
            if (!why.isEmpty()) problems << why;
        }
        if (problems.isEmpty()) {
            ++passed;
            out(QStringLiteral("  PASS (%1s)").arg(caseSecs, 0, 'f', 1));
        } else {
            failedNames << name;
            out(QStringLiteral("  FAIL (%1s): %2").arg(caseSecs, 0, 'f', 1).arg(problems.join("; ")));
            out(QStringLiteral("  result:\n") + summarize(d, MediaIndex(d)).replace('\n', "\n    "));
        }
    }
    out(QStringLiteral("\nSCORE %1/%2 passed, %3s total").arg(passed).arg(total).arg(totalSecs, 0, 'f', 0));
    if (!failedNames.isEmpty()) out(QStringLiteral("failed: ") + failedNames.join(", "));
    return passed == total ? 0 : 1;
}

}  // namespace AgentCli
