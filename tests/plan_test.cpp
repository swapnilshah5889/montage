// AI-layer tests that need no real model:
//  - edit plans: round trip, validation, leniency
//  - both chat adapters against a local mock server replaying streamed responses

#include "ai/ChatProvider.h"
#include "ai/EditPlan.h"
#include "ai/PlanGuards.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cmath>
#include <cstdio>

using namespace mtg;
using namespace mtg::ai;

static int failures = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

static Draft sampleDraft()
{
    Draft d;
    auto add = [&](const char *id, MaterialKind k, const char *name, double secs, bool audio, int w, int h) {
        Material m;
        m.id = id;
        m.kind = k;
        m.name = name;
        m.durationUs = qint64(secs * 1e6);
        m.hasAudio = audio;
        m.hasVideo = k != MaterialKind::Audio;
        m.width = w;
        m.height = h;
        d.materials.append(m);
    };
    add("a", MaterialKind::Video, "beach.mp4", 12, true, 1920, 1080);
    add("b", MaterialKind::Video, "city.mov", 8, true, 1080, 1920);
    add("c", MaterialKind::Audio, "song.mp3", 95, true, 0, 0);
    add("d", MaterialKind::Image, "logo.png", 0, false, 800, 800);
    return d;
}

static void guardTests()
{
    const Draft base = sampleDraft();  // m1 beach 12s, m2 city 8s, m3 song 95s, m4 logo (photo)
    const MediaIndex index(base);

    CHECK(requestedLength("turn these into a 12s tiktok with the song") == 12);
    CHECK(requestedLength("Make a 10 second vertical video with the city first") == 10);
    CHECK(requestedLength("make the whole thing 9 seconds but keep all three shots") == 9);
    CHECK(requestedLength("Add a title for 3 seconds of the video") == 0);
    CHECK(requestedLength("Show my name as a lower third during the first 4 seconds") == 0);

    // A text-only request keeps the clips even if the model changed them.
    const QJsonObject current = QJsonDocument::fromJson(R"({"aspect": "original", "main": [{"media": "m1", "from": 0, "to": 6},
        {"media": "m2", "from": 0, "to": 6}], "overlays": [], "audio": [], "texts": []})").object();
    QJsonObject plan = QJsonDocument::fromJson(R"({"aspect": "16:9", "main": [{"media": "m1", "from": 0, "to": 4},
        {"media": "m2", "from": 0, "to": 4}], "overlays": [], "audio": [],
        "texts": [{"text": "Swapnil Shah", "at": 0, "duration": 4, "style": "lower-third"}]})").object();
    const QStringList kept = keepUnrequested("Show my name \"Swapnil Shah\" as a lower third during the first 4 seconds",
                                             current, base, index, &plan);
    CHECK(plan.value("main") == current.value("main"));
    CHECK(plan.value("aspect").toString() == "keep");
    CHECK(plan.value("texts").toArray().size() == 1);
    CHECK(kept.contains("the clips") && kept.contains("the shape"));
    // ...while a request about the clips may change them.
    QJsonObject plan2 = plan;
    plan2.insert("main", QJsonDocument::fromJson(R"([{"media": "m2", "from": 0, "to": 4}])").array());
    CHECK(keepUnrequested("remove the beach clip", current, base, index, &plan2).isEmpty() || plan2.value("main").toArray().size() == 1);
    CHECK(keepUnrequested("start over with just the city", current, base, index, &plan2).isEmpty());

    // Fit to a requested length: videos absorb the change, music follows the end.
    QJsonObject fit = QJsonDocument::fromJson(R"({"main": [{"media": "m2", "from": 0, "to": 5}, {"media": "m1", "from": 0, "to": 6},
        {"media": "m1", "from": 6, "to": 12}], "audio": [{"media": "m3", "at": 0, "from": 0, "to": 17, "volume": 0.3}],
        "texts": [{"text": "Bye", "at": 14, "duration": 3, "style": "caption"}]})").object();
    CHECK(fitToLength(12, base, index, &fit));
    PlanReport rf;
    const Draft df = draftFromPlan(base, fit, index, &rf);
    CHECK(rf.ok() && std::abs(df.duration() / 1e6 - 12) < 0.06);
    bool musicCovers = false, captionEnds = false;
    for (const Track &t : df.tracks)
        for (const Segment &s : t.segments) {
            if (t.role == TrackRole::Audio) musicCovers = std::abs(s.end() / 1e6 - df.tracks[0].segments.last().end() / 1e6) < 0.06;
            if (s.materialId == kTextMaterialId) captionEnds = std::abs(s.end() - df.tracks[0].segments.last().end()) < 60'000;
        }
    CHECK(musicCovers && captionEnds);

    // A logo "for 2 seconds" keeps its 2 seconds; the videos take up the rest.
    QJsonObject logo = QJsonDocument::fromJson(R"({"main": [{"media": "m4", "from": 0, "to": 2}, {"media": "m1", "from": 0, "to": 12},
        {"media": "m2", "from": 0, "to": 8}]})").object();
    CHECK(fitToLength(14, base, index, &logo));
    const QJsonArray lm = logo.value("main").toArray();
    CHECK(lm[0].toObject().value("to").toDouble() == 2);
    double sum = 0;
    for (const QJsonValue &v : lm) sum += v.toObject().value("to").toDouble() - v.toObject().value("from").toDouble();
    CHECK(std::abs(sum - 14) < 0.06);
    CHECK(requestedBackground("make a 5 second intro on a dark blue background") == "#00008B");
    CHECK(requestedBackground("with a red backdrop") == "#FF0000");
    CHECK(requestedBackground("add a title in the background music") == "");
    CHECK(parseColor("Navy") == "#000080" && !parseColor("deep purple").isEmpty() && parseColor("sky").isEmpty());

    // Already the right length: untouched.
    CHECK(!fitToLength(14, base, index, &logo));

    // Broken ranges (zero length, start past the file's end) still reach the requested length.
    QJsonObject broken = QJsonDocument::fromJson(R"({"main": [{"media": "m4", "from": 0, "to": 2}, {"media": "m1", "from": 2, "to": 12},
        {"media": "m2", "from": 14, "to": 14}]})").object();
    CHECK(fitToLength(20, base, index, &broken));
    PlanReport rb;
    const Draft db = draftFromPlan(base, broken, index, &rb);
    CHECK(rb.ok() && std::abs(db.duration() / 1e6 - 20) < 0.06);

    // "The logo at the end for 2 seconds" placed at the very end moves back to end with the video.
    PlanReport rl;
    const Draft dl = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 10}],
        "overlays": [{"media": "m4", "at": 10, "from": 0, "to": 2, "position": "center"}]})").object(), index, &rl);
    CHECK(rl.ok() && dl.duration() == 10'000'000);
    bool logoAtEnd = false;
    for (const Track &t : dl.tracks)
        for (const Segment &s : t.segments)
            if (t.role == TrackRole::Overlay) logoAtEnd = s.start == 8'000'000 && s.duration == 2'000'000;
    CHECK(logoAtEnd);
}

static void planTests()
{
    const Draft base = sampleDraft();
    const MediaIndex index(base);
    CHECK(index.alias("c") == "m3" && index.materialId("M2") == "b" && index.materialId("m9").isEmpty());

    const QJsonObject plan = QJsonDocument::fromJson(R"({
        "reply": "ok", "aspect": "9:16",
        "main": [{"media": "m2", "from": 0, "to": 5}, {"media": "m1", "from": "2", "to": "0:07"}, {"media": "m4", "from": 0, "to": 2}],
        "overlays": [{"media": "m4", "at": 1, "from": 0, "to": 3, "position": "top-right", "scale": 0.3}],
        "audio": [{"media": "m3", "at": 0, "from": 0, "to": 30, "volume": 0.3}],
        "texts": [{"text": "Weekend", "at": 0, "duration": 3, "style": "title", "color": "#FF3B30"},
                  {"text": "Bye", "at": 10, "duration": 3, "style": "caption"}],
        "export": false})").object();
    PlanReport report;
    const Draft d = draftFromPlan(base, plan, index, &report);
    CHECK(report.ok());
    CHECK(d.canvas.width == 1080 && d.canvas.height == 1920 && d.canvasLocked);
    CHECK(d.tracks[0].segments.size() == 3);
    CHECK(d.duration() == 12'000'000);  // 5 + 5 + 2; music and text trimmed to the video
    CHECK(d.tracks[0].segments[1].sourceStart == 2'000'000);
    CHECK(report.warnings.size() == 2);  // music cut to 12s, caption cut to end with the video
    int texts = 0, audio = 0, overlays = 0;
    for (const Track &t : d.tracks)
        for (const Segment &s : t.segments) {
            if (s.materialId == kTextMaterialId) ++texts;
            else if (t.role == TrackRole::Audio) ++audio;
            else if (t.role == TrackRole::Overlay) ++overlays;
        }
    CHECK(texts == 2 && audio == 1 && overlays == 1);

    // Round trip: plan -> draft -> plan -> draft gives the same timeline.
    const QJsonObject again = planFromDraft(d, MediaIndex(d));
    PlanReport r2;
    const Draft d2 = draftFromPlan(d, again, MediaIndex(d), &r2);
    CHECK(r2.ok() && r2.warnings.isEmpty());
    auto shape = [](const Draft &x) {
        QJsonObject j = x.toJson();
        QJsonArray tracks;
        for (const QJsonValue &t : j.value("tracks").toArray()) {
            QJsonArray segs;
            for (const QJsonValue &s : t.toObject().value("segments").toArray()) {
                QJsonObject o = s.toObject();
                o.remove("id");
                segs.append(o);
            }
            tracks.append(QJsonObject{{"role", t.toObject().value("role")}, {"segments", segs}});
        }
        j.insert("tracks", tracks);
        return j;
    };
    CHECK(shape(d) == shape(d2));
    if (shape(d) != shape(d2))
        std::fprintf(stderr, "%s\n---\n%s\n", QJsonDocument(shape(d)).toJson().constData(), QJsonDocument(shape(d2)).toJson().constData());

    // Errors that must be fixed by the model, with actionable messages.
    PlanReport r3;
    draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "m7", "from": 0, "to": 3},
        {"media": "m3", "from": 0, "to": 3}, {"media": "m1", "from": 13, "to": 14}, {"media": "m1", "from": 4, "to": 4}]})").object(),
                  index, &r3);
    CHECK(r3.errors.size() == 3);  // unknown id, audio in main, zero length; a start past the end is repaired
    CHECK(r3.text().contains("unknown media \"m7\". Use one of: m1, m2, m3, m4"));
    CHECK(r3.text().contains("m3 is an audio file"));
    CHECK(r3.text().contains("past the end of m1 (12s long); used its first 1s instead"));

    // Leniency: file names instead of ids, "to" beyond the file clamps with a warning.
    PlanReport r4;
    const Draft d4 = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "beach.mp4", "from": 10, "to": 20}]})").object(),
                                   index, &r4);
    CHECK(r4.ok() && r4.warnings.size() == 1 && d4.duration() == 2'000'000);

    // Repair: from/to written as times in the video (0-5, 5-10, 10-15) instead of file positions.
    PlanReport r5;
    const Draft d5 = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 5},
        {"media": "m2", "from": 5, "to": 10}, {"media": "m1", "from": 10, "to": 15}]})").object(), index, &r5);
    CHECK(r5.ok() && r5.warnings.size() == 1 && d5.duration() == 15'000'000);
    CHECK(d5.tracks[0].segments[2].sourceStart == 0 && d5.tracks[0].segments[1].sourceStart == 0);
    // ...but a valid plan that happens to be cumulative within the files is left alone.
    PlanReport r6;
    const Draft d6 = draftFromPlan(base, QJsonDocument::fromJson(R"({"main": [{"media": "m1", "from": 0, "to": 3},
        {"media": "m1", "from": 3, "to": 6}]})").object(), index, &r6);
    CHECK(r6.ok() && r6.warnings.isEmpty() && d6.tracks[0].segments[1].sourceStart == 3'000'000);

    // Text-only videos: no media needed, background colour round-trips.
    PlanReport r7;
    const Draft d7 = draftFromPlan(Draft(), QJsonDocument::fromJson(R"({"background": "#1e3a8a", "main": [],
        "texts": [{"text": "Intro", "at": 0, "duration": 4, "style": "title"}]})").object(), MediaIndex(Draft()), &r7);
    CHECK(r7.ok() && r7.warnings.isEmpty() && d7.duration() == 4'000'000 && d7.canvas.background == "#1E3A8A");
    CHECK(planFromDraft(d7, MediaIndex(d7)).value("background").toString() == "#1E3A8A");
    CHECK(Draft::fromJson(d7.toJson()).canvas.background == "#1E3A8A");

    CHECK(seconds(QJsonValue("1:30")) == 90 && seconds(QJsonValue("2.5s")) == 2.5 && std::isnan(seconds(QJsonValue("abc"))));
    CHECK(planSchema().value("required").toArray().size() == 7);
}

// ------------------------------------------------------------ mock server

struct Canned {
    int status = 200;
    QByteArray contentType = "text/event-stream";
    QByteArray body;
};

class MockServer : public QTcpServer
{
public:
    QList<Canned> replies;
    QList<QByteArray> requests;  // raw header block + body of each request
    QList<QJsonObject> bodies;

    MockServer() { listen(QHostAddress::LocalHost, 0); connect(this, &QTcpServer::newConnection, this, &MockServer::accept); }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(serverPort()); }

private:
    void accept()
    {
        while (QTcpSocket *s = nextPendingConnection()) {
            auto *buf = new QByteArray;
            connect(s, &QTcpSocket::readyRead, s, [this, s, buf] {
                *buf += s->readAll();
                const int headerEnd = buf->indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                int length = 0;
                for (const QByteArray &line : buf->left(headerEnd).split('\n'))
                    if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
                if (buf->size() < headerEnd + 4 + length) return;
                requests << *buf;
                bodies << QJsonDocument::fromJson(buf->mid(headerEnd + 4, length)).object();
                const Canned c = replies.isEmpty() ? Canned{500, "text/plain", "no reply queued"} : replies.takeFirst();
                QByteArray resp = "HTTP/1.1 " + QByteArray::number(c.status) + (c.status == 200 ? " OK" : " Error")
                                  + "\r\nContent-Type: " + c.contentType + "\r\nContent-Length: " + QByteArray::number(c.body.size())
                                  + "\r\nConnection: close\r\n\r\n" + c.body;
                s->write(resp);
                s->disconnectFromHost();
                buf->clear();
            });
            connect(s, &QTcpSocket::disconnected, s, [s, buf] {
                delete buf;
                s->deleteLater();
            });
        }
    }
};

static QByteArray sse(const QList<QJsonObject> &events, bool named)
{
    QByteArray out;
    for (const QJsonObject &e : events) {
        if (named) out += "event: " + e.value("type").toString().toUtf8() + "\n";
        out += "data: " + QJsonDocument(e).toJson(QJsonDocument::Compact) + "\n\n";
    }
    return out;
}

template <typename T, typename Signal>
static bool waitFor(T *obj, Signal sig, int ms = 5000)
{
    QEventLoop loop;
    bool fired = false;
    QObject::connect(obj, sig, &loop, [&] { fired = true; loop.quit(); });
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
    return fired;
}

static void anthropicTests()
{
    MockServer server;
    const QJsonObject delta = QJsonObject{{"type", "content_block_delta"}};
    auto d = [&](int index, QJsonObject inner) {
        QJsonObject e = delta;
        e.insert("index", index);
        e.insert("delta", inner);
        return e;
    };
    server.replies << Canned{200, "text/event-stream", sse({
        QJsonObject{{"type", "message_start"}, {"message", QJsonObject{{"id", "msg_1"}}}},
        QJsonObject{{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "thinking"}, {"thinking", ""}}}},
        d(0, {{"type", "thinking_delta"}, {"thinking", "Checking the clips first."}}),
        d(0, {{"type", "signature_delta"}, {"signature", "sig-abc"}}),
        QJsonObject{{"type", "content_block_stop"}, {"index", 0}},
        QJsonObject{{"type", "content_block_start"}, {"index", 1}, {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}},
        d(1, {{"type", "text_delta"}, {"text", "Let me look."}}),
        QJsonObject{{"type", "content_block_stop"}, {"index", 1}},
        QJsonObject{{"type", "content_block_start"}, {"index", 2},
                    {"content_block", QJsonObject{{"type", "tool_use"}, {"id", "toolu_1"}, {"name", "look_at_media"}, {"input", QJsonObject{}}}}},
        d(2, {{"type", "input_json_delta"}, {"partial_json", "{\"med"}}),
        d(2, {{"type", "input_json_delta"}, {"partial_json", "ia\": \"m1\", \"cou"}}),
        d(2, {{"type", "input_json_delta"}, {"partial_json", "nt\": 4}"}}),
        QJsonObject{{"type", "content_block_stop"}, {"index", 2}},
        QJsonObject{{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", "tool_use"}}}},
        QJsonObject{{"type", "message_stop"}}}, true)};
    server.replies << Canned{200, "text/event-stream", sse({
        QJsonObject{{"type", "content_block_start"}, {"index", 0}, {"content_block", QJsonObject{{"type", "text"}, {"text", ""}}}},
        d(0, {{"type", "text_delta"}, {"text", "Done!"}}),
        QJsonObject{{"type", "content_block_stop"}, {"index", 0}},
        QJsonObject{{"type", "message_delta"}, {"delta", QJsonObject{{"stop_reason", "end_turn"}}}}}, true)};

    ChatProvider *p = ChatProvider::create("anthropic", nullptr);
    ModelProfile prof;
    prof.kind = "anthropic";
    prof.baseUrl = server.url();
    prof.model = "claude-opus-5-5";
    p->setProfile(prof, "test-key");
    p->reset("SYSTEM", {ToolSpec{"look_at_media", "desc", QJsonObject{{"type", "object"}}}});
    p->addUserMessage("make a video");

    TurnResult got;
    QStringList notes;
    QObject::connect(p, &ChatProvider::turnFinished, [&](const TurnResult &r) { got = r; });
    QObject::connect(p, &ChatProvider::progressNote, [&](const QString &n) { notes << n; });
    p->run();
    CHECK(waitFor(p, &ChatProvider::turnFinished));
    CHECK(got.stop == StopReason::ToolUse && got.text == "Let me look.");
    CHECK(got.calls.size() == 1 && got.calls[0].id == "toolu_1" && got.calls[0].input.value("count").toInt() == 4);
    CHECK(notes == QStringList{"Checking the clips first."});

    const QByteArray head1 = server.requests.value(0).toLower();  // header names are case-insensitive
    CHECK(head1.contains("x-api-key: test-key") && head1.contains("anthropic-version: 2023-06-01"));
    CHECK(head1.contains("server-side-fallback-2026-07-01") && head1.contains("thinking-display-updates-2026-08-18"));
    const QJsonObject b1 = server.bodies.value(0);
    CHECK(b1.value("model").toString() == "claude-opus-5-5" && b1.value("stream").toBool());
    CHECK(b1.value("fallbacks").toString() == "default");
    CHECK(b1.value("thinking").toObject().value("display").toString() == "updates");
    CHECK(b1.value("output_config").toObject().value("effort").toString() == "medium");
    CHECK(b1.value("system").toArray().at(0).toObject().contains("cache_control"));
    CHECK(b1.value("tools").toArray().at(0).toObject().value("eager_input_streaming").toBool());

    p->addToolResults({ToolResult{"toolu_1", "look_at_media", "4 frames", {QByteArray("JPEGDATA")}, false}});
    p->run();
    CHECK(waitFor(p, &ChatProvider::turnFinished));
    CHECK(got.stop == StopReason::EndTurn && got.text == "Done!");

    // The assistant turn is replayed verbatim (thinking + signature, parsed tool input),
    // followed by one user message holding the tool_result with its image.
    const QJsonArray msgs = server.bodies.value(1).value("messages").toArray();
    CHECK(msgs.size() == 3);
    const QJsonArray assistant = msgs.at(1).toObject().value("content").toArray();
    CHECK(assistant.at(0).toObject().value("type") == "thinking" && assistant.at(0).toObject().value("signature") == "sig-abc");
    CHECK(assistant.at(2).toObject().value("input").toObject().value("media") == "m1");
    const QJsonObject result = msgs.at(2).toObject().value("content").toArray().at(0).toObject();
    CHECK(result.value("type") == "tool_result" && result.value("tool_use_id") == "toolu_1");
    CHECK(result.value("content").toArray().at(1).toObject().value("type") == "image");
    delete p;
}

static void openAiTests()
{
    MockServer server;
    auto chunk = [](QJsonObject delta, QJsonValue finish = QJsonValue()) {
        return QJsonObject{{"choices", QJsonArray{QJsonObject{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}}}};
    };
    // Tool-call arguments arrive split across two chunks.
    auto toolChunk = [&](QJsonObject call) {
        call.insert("index", 0);
        return chunk(QJsonObject{{"tool_calls", QJsonArray{call}}});
    };
    QByteArray body = sse({
        chunk({{"role", "assistant"}, {"content", "Sure, "}}),
        chunk({{"content", "editing."}}),
        toolChunk({{"id", "call_9"}, {"type", "function"},
                   {"function", QJsonObject{{"name", "apply_plan"}, {"arguments", "{\"plan\": {\"ma"}}}}),
        toolChunk({{"function", QJsonObject{{"arguments", "in\": []}}"}}}}),
        chunk({}, "tool_calls")}, false);
    body += "data: [DONE]\n\n";
    server.replies << Canned{200, "text/event-stream", body};
    server.replies << Canned{200, "text/event-stream", sse({chunk({{"content", "All set."}}), chunk({}, "stop")}, false) + "data: [DONE]\n\n"};
    // requestJson: schema rejected once, then a fenced JSON answer.
    server.replies << Canned{400, "application/json", R"({"error": {"message": "response_format json_schema not supported"}})"};
    server.replies << Canned{200, "application/json",
                             QJsonDocument(QJsonObject{{"choices", QJsonArray{QJsonObject{{"message", QJsonObject{
                                 {"content", "Here you go:\n```json\n{\"reply\": \"ok\", \"main\": []}\n```"}}}}}}}).toJson()};

    ChatProvider *p = ChatProvider::create("openai", nullptr);
    ModelProfile prof;
    prof.baseUrl = server.url() + "/v1";
    prof.model = "test-model";
    prof.vision = true;
    p->setProfile(prof, "sk-test");
    p->reset("SYSTEM", {ToolSpec{"apply_plan", "desc", QJsonObject{{"type", "object"}}}});
    p->addUserMessage("hi");
    TurnResult got;
    QString streamed;
    QObject::connect(p, &ChatProvider::turnFinished, [&](const TurnResult &r) { got = r; });
    QObject::connect(p, &ChatProvider::textDelta, [&](const QString &t) { streamed += t; });
    p->run();
    CHECK(waitFor(p, &ChatProvider::turnFinished));
    CHECK(got.stop == StopReason::ToolUse && streamed == "Sure, editing.");
    CHECK(got.calls.size() == 1 && got.calls[0].id == "call_9" && got.calls[0].inputOk);
    CHECK(got.calls[0].input.value("plan").toObject().contains("main"));
    CHECK(server.requests.value(0).toLower().contains("authorization: bearer sk-test"));
    CHECK(server.bodies.value(0).value("tools").toArray().at(0).toObject().value("type") == "function");

    p->addToolResults({ToolResult{"call_9", "apply_plan", "Applied.", {QByteArray("IMG")}, false}});
    p->run();
    CHECK(waitFor(p, &ChatProvider::turnFinished));
    CHECK(got.stop == StopReason::EndTurn && got.text == "All set.");
    const QJsonArray msgs = server.bodies.value(1).value("messages").toArray();
    // system, user, assistant(tool_calls), tool, user(images)
    CHECK(msgs.size() == 5);
    CHECK(msgs.at(2).toObject().value("tool_calls").toArray().size() == 1);
    CHECK(msgs.at(3).toObject().value("role") == "tool" && msgs.at(3).toObject().value("tool_call_id") == "call_9");
    CHECK(msgs.at(4).toObject().value("content").toArray().at(2).toObject().value("type") == "image_url");

    QJsonObject json;
    QString jsonErr;
    QObject::connect(p, &ChatProvider::jsonFinished, [&](const QJsonObject &o, const QString &, const QString &e) {
        json = o;
        jsonErr = e;
    });
    p->requestJson("SYS", "USER", planSchema());
    CHECK(waitFor(p, &ChatProvider::jsonFinished));
    CHECK(jsonErr.isEmpty() && json.value("reply") == "ok");
    CHECK(server.bodies.value(2).value("response_format").toObject().value("type") == "json_schema");
    CHECK(server.bodies.value(3).value("response_format").toObject().value("type") == "json_object");
    delete p;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    planTests();
    guardTests();
    anthropicTests();
    openAiTests();
    std::printf("%s (%d failure%s)\n", failures ? "FAILED" : "all ai tests passed", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
