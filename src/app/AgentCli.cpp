#include "engine/Beats.h"
#include "AgentCli.h"

#include "Agent.h"
#include "Project.h"
#include "ai/EditPlan.h"
#include "ai/Generators.h"

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

// How loud a clip plays while someone talks: music that ducks dips to
// volume x duck there. "max_volume" checks this level ("quiet under the voice").
double underSpeech(const Segment &s)
{
    return s.duck > 0 ? s.volume * s.duck : s.volume;
}

bool hasEffect(const Segment &s, const QString &name)
{
    for (const Effect &e : s.effects)
        if (e.name == name) return true;
    return false;
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
            if (c.contains("start_min") && s->start / 1e6 < c.value("start_min").toDouble() - eps)
                return QStringLiteral("text starts at %1s").arg(s->start / 1e6);
            if (c.contains("shape") && s->text.shape != c.value("shape").toString())
                return QStringLiteral("text box is \"%1\"").arg(s->text.shape);
            if (c.contains("effect") && !hasEffect(*s, c.value("effect").toString()))
                return QStringLiteral("text has no %1 effect").arg(c.value("effect").toString());
            if (c.value("animated").toBool() && s->motion.in.isEmpty() && s->motion.out.isEmpty() && s->keys.isEmpty())
                return QStringLiteral("text \"%1\" doesn't move").arg(want);
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
            if (c.contains("max_volume") && underSpeech(*s) > c.value("max_volume").toDouble() + 0.001)
                return QStringLiteral("music volume %1").arg(underSpeech(*s));
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
    if (type == QLatin1String("style")) {
        const QString want = c.value("value").toString();
        const bool ok = want.isEmpty() ? !x.d.style.isEmpty() : (c.contains("any_of") ? false : x.d.style == want);
        bool anyOf = false;
        for (const QJsonValue &v : c.value("any_of").toArray()) anyOf |= x.d.style == v.toString();
        return ok || anyOf ? QString() : QStringLiteral("style is \"%1\"").arg(x.d.style);
    }
    if (type == QLatin1String("transitions")) {
        int n = 0;
        for (const Segment &s : x.d.tracks[0].segments)
            n += !s.transition.name.isEmpty() && (!c.contains("name") || s.transition.name == c.value("name").toString());
        return n >= c.value("min").toInt(0) && n <= c.value("max").toInt(1000) ? QString() : QStringLiteral("%1 transitions").arg(n);
    }
    if (type == QLatin1String("main_effect")) {  // clips on the main track with this effect (or any effect)
        const QString name = c.value("name").toString();
        int n = 0;
        for (const Segment &s : x.d.tracks[0].segments) n += name.isEmpty() ? !s.effects.isEmpty() : hasEffect(s, name);
        const int all = int(x.d.tracks[0].segments.size());
        const int lo = c.value("all").toBool() ? all : c.value("min").toInt(1);
        return n >= lo ? QString() : QStringLiteral("%1 of %2 clips have %3").arg(n).arg(all).arg(name.isEmpty() ? "an effect" : name);
    }
    if (type == QLatin1String("moment")) {  // an adjustment layer with this effect
        const QString name = c.value("effect").toString();
        QStringList seen;
        for (const Track &t : x.d.tracks)
            for (const Segment &s : t.segments) {
                if (s.materialId != kEffectMaterialId) continue;
                for (const Effect &e : s.effects) seen << QStringLiteral("%1@%2").arg(e.name).arg(s.start / 1e6);
                if (!hasEffect(s, name)) continue;
                const double at = s.start / 1e6;
                if (c.contains("at_min") && at < c.value("at_min").toDouble() - eps) continue;
                if (c.contains("at_max") && at > c.value("at_max").toDouble() + eps) continue;
                return {};
            }
        return QStringLiteral("no %1 moment (have %2)").arg(name, seen.isEmpty() ? "none" : seen.join(", "));
    }
    if (type == QLatin1String("speed")) {  // a main clip of this media is slowed or sped up
        const QString name = c.value("media").toString();
        for (const Segment &s : x.d.tracks[0].segments) {
            if (!name.isEmpty() && mediaName(x.d, s) != name) continue;
            if (c.value("slow").toBool() && ((s.speed > 0 && s.speed < 0.95) || !s.curve.isEmpty())) return {};
            if (c.value("fast").toBool() && s.speed > 1.05) return {};
        }
        return QStringLiteral("no %1 clip%2").arg(c.value("slow").toBool() ? "slowed" : "sped-up", name.isEmpty() ? "" : " of " + name);
    }
    if (type == QLatin1String("generated")) {  // media made by a local model: {"kind": "music", "max_volume": 0.3, ...}
        const QString kind = c.value("kind").toString();
        int n = 0;
        for (const Track &t : x.d.tracks)
            for (const Segment &s : t.segments) {
                const Material *m = x.d.material(s.materialId);
                if (!m || generatedKind(m->name) != kind) continue;
                ++n;
                if (c.contains("role") && roleName(t.role) != c.value("role").toString())
                    return QStringLiteral("generated %1 is on the %2 track").arg(kind, roleName(t.role));
                if (c.contains("max_volume") && underSpeech(s) > c.value("max_volume").toDouble() + 0.001)
                    return QStringLiteral("generated %1 volume %2").arg(kind).arg(underSpeech(s));
                if (c.contains("min_volume") && s.volume < c.value("min_volume").toDouble() - 0.001)
                    return QStringLiteral("generated %1 volume %2").arg(kind).arg(s.volume);
                if (c.value("covers_video").toBool() && (s.start > 100'000 || s.end() / 1e6 < x.length - 0.3))
                    return QStringLiteral("generated %1 covers %2-%3s of %4s").arg(kind).arg(s.start / 1e6).arg(s.end() / 1e6).arg(x.length);
            }
        if (c.contains("max")) return n <= c.value("max").toInt() ? QString() : QStringLiteral("%1 generated %2 clips").arg(n).arg(kind);
        return n > 0 ? QString() : QStringLiteral("no generated %1 on the timeline").arg(kind);
    }
    if (type == QLatin1String("motion")) {  // a clip of this media moves (any motion or keyframes)
        const QString name = c.value("media").toString();
        for (const Track &t : x.d.tracks)
            for (const Segment &s : t.segments)
                if (mediaName(x.d, s) == name && (!s.motion.in.isEmpty() || !s.motion.out.isEmpty() || !s.motion.loop.isEmpty() || !s.keys.isEmpty()))
                    return {};
        return name + QStringLiteral(" doesn't move");
    }
    if (type == QLatin1String("main_within")) {  // every main clip of this media lies inside one of `ranges` (seconds in the file)
        const QString name = c.value("media").toString();
        int seen = 0;
        for (const Segment &s : x.d.tracks[0].segments) {
            if (mediaName(x.d, s) != name) continue;
            ++seen;
            const double a = s.sourceStart / 1e6, b = (s.sourceStart + s.sourceSpan()) / 1e6;
            bool inside = false;
            for (const QJsonValue &r : c.value("ranges").toArray())
                inside |= a >= r.toArray().at(0).toDouble() - 0.15 && b <= r.toArray().at(1).toDouble() + 0.15;
            if (!inside) return QStringLiteral("%1 uses %2-%3s").arg(name).arg(a, 0, 'f', 2).arg(b, 0, 'f', 2);
        }
        return seen ? QString() : QStringLiteral("%1 isn't in the main track").arg(name);
    }
    if (type == QLatin1String("beat_sync")) {
        const QString want = c.value("value").toString();
        return x.d.beatSync == want ? QString() : QStringLiteral("beat_sync is \"%1\"").arg(x.d.beatSync);
    }
    if (type == QLatin1String("on_beat")) {  // every cut within `tolerance` of a beat of this music
        const QString name = c.value("media").toString();
        const double tol = c.value("tolerance").toDouble(0.08);
        for (const Segment *s : segmentsOn(x.d, TrackRole::Audio, false)) {
            const Material *m = x.d.material(s->materialId);
            if (!m || m->name != name) continue;
            const BeatGrid g = beatGrid(*m);
            QStringList off;
            for (qsizetype i = 1; i < x.d.tracks[0].segments.size(); ++i) {
                const double cut = x.d.tracks[0].segments[i].start / 1e6;
                double best = 1e9;
                for (double b : g.beats) best = std::min(best, std::abs(s->start / 1e6 + b - s->sourceStart / 1e6 - cut));
                if (best > tol) off << QString::number(cut, 'f', 2);
            }
            return off.isEmpty() ? QString() : QStringLiteral("cuts off the beat at %1s").arg(off.join(", "));
        }
        return QStringLiteral("no audio track uses %1").arg(name);
    }
    if (type == QLatin1String("captions")) {
        int n = 0;
        QString said;
        for (const Segment *s : segmentsOn(x.d, TrackRole::Overlay, true))
            if (s->origin == QLatin1String("captions")) ++n, said += s->text.text + QLatin1Char(' ');
        if (n < c.value("min").toInt(1)) return QStringLiteral("%1 auto captions").arg(n);
        if (c.contains("contains") && !said.contains(c.value("contains").toString(), Qt::CaseInsensitive))
            return QStringLiteral("captions say \"%1\"").arg(said.trimmed().left(80));
        return {};
    }
    if (type == QLatin1String("duck")) {
        const QString name = c.value("media").toString();
        for (const Segment *s : segmentsOn(x.d, TrackRole::Audio, false))
            if (mediaName(x.d, *s) == name) return s->duck > 0 ? QString() : QStringLiteral("%1 doesn't dip under speech").arg(name);
        return QStringLiteral("no audio track uses %1").arg(name);
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
    int passed = 0, total = 0, skipped = 0;
    double totalSecs = 0;
    QStringList failedNames;
    Generators generators;  // only asked whether the backends a case needs are up
    out(QStringLiteral("Model: %1 (%2, %3 mode)\n")
            .arg(agent.profileMap().value("model").toString(), agent.profileMap().value("name").toString(),
                 agent.profileMap().value("guided").toBool() ? "guided" : "agent"));

    for (const QJsonValue &cv : cases) {
        const QJsonObject c = cv.toObject();
        const QString name = c.value("name").toString();
        if (!only.isEmpty() && !name.contains(only)) continue;
        // Cases that generate media need the local models; skip them when one is down.
        QStringList down;
        for (const QJsonValue &k : c.value("requires").toArray())
            if (const QString why = generators.unavailable(k.toString()); !why.isEmpty()) down << k.toString() + ": " + why.section('\n', 0, 0);
        if (!down.isEmpty()) {
            ++skipped;
            out(QStringLiteral("== %1\n  SKIP (%2)").arg(name, down.join("; ")));
            continue;
        }
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

        // Let the scene index finish watching the media first (cached after the first run),
        // so every run sees the same shots.
        if (!agent.waitForMedia(600'000)) out(QStringLiteral("  (media index still building)"));

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
    out(QStringLiteral("\nSCORE %1/%2 passed, %3s total%4").arg(passed).arg(total).arg(totalSecs, 0, 'f', 0)
            .arg(skipped ? QStringLiteral(", %1 skipped (backends down)").arg(skipped) : QString()));
    if (!failedNames.isEmpty()) out(QStringLiteral("failed: ") + failedNames.join(", "));
    return passed == total ? 0 : 1;
}

}  // namespace AgentCli
