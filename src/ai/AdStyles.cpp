#include "AdStyles.h"

#include "EditPlan.h"

#include <QJsonArray>
#include <QSet>
#include <cmath>

namespace mtg::ai {

namespace {

struct Spec {
    QStringList transitions;  // cycled across cuts
    double transitionSec;
    QString look;             // colour look on main clips
    double lookAmount;
    bool punchOnCuts;         // zoom-punch moment on every cut
    bool flashFirstCut;
    QString textIn, textOut;
};

Spec spec(const QString &style)
{
    if (style == QLatin1String("energetic"))
        return {{"whip", "zoom", "glitch", "push-left"}, 0.35, "vivid", 0.6, true, true, "pop", "fade"};
    if (style == QLatin1String("bold"))
        return {{"push-left", "zoom", "push-up", "flash"}, 0.3, "dramatic", 0.7, true, false, "slide-up", "fade"};
    if (style == QLatin1String("playful"))
        return {{"spin", "slide-left", "circle", "push-up"}, 0.45, "warm", 0.6, false, false, "drop", "pop"};
    if (style == QLatin1String("elegant"))
        return {{"crossfade", "fade-white", "blur"}, 0.7, "cinematic", 0.6, false, false, "fade", "fade"};
    if (style == QLatin1String("luxury"))
        return {{"fade-black", "blur", "crossfade"}, 0.9, "cinematic", 0.9, false, false, "blur-in", "fade"};
    if (style == QLatin1String("minimal"))
        return {{"crossfade"}, 0.4, "", 0, false, false, "fade", "fade"};
    return {};
}

constexpr double kPunchSec = 0.35;
constexpr double kFlashSec = 0.25;

double num(const QJsonValue &v, double fallback)
{
    const double d = seconds(v);
    return std::isnan(d) ? fallback : d;
}

double round2(double v)
{
    return std::round(v * 100) / 100;
}

// Timeline length of each main item and the cut times between them.
QList<double> mainLengths(const QJsonArray &main)
{
    QList<double> out;
    for (const QJsonValue &v : main) {
        const double s = itemSeconds(v.toObject());
        out << (std::isnan(s) ? 3 : std::max(0.1, s));
    }
    return out;
}

QString clipKey(const QJsonObject &o)
{
    return QStringLiteral("%1|%2|%3").arg(o.value("media").toString()).arg(round2(num(o.value("from"), 0))).arg(round2(num(o.value("to"), 0)));
}

bool presetMoves(const QString &style, bool in)
{
    TextStyle st;
    Transform tr;
    Motion mv;
    applyTextPreset(style, &st, &tr, &mv);
    return in ? !mv.in.isEmpty() : !mv.out.isEmpty();
}

// Removes what `s` would have added, so a new style can replace it.
void strip(const Spec &s, QJsonObject *plan)
{
    QJsonArray main = plan->value("main").toArray();
    for (int i = 0; i < main.size(); ++i) {
        QJsonObject o = main[i].toObject();
        if (s.transitions.contains(o.value("transition").toString())
            && std::abs(num(o.value("transition_duration"), 0.5) - s.transitionSec) < 0.051) {
            o.remove("transition");
            o.remove("transition_duration");
        }
        const QJsonArray fx = o.value("effects").toArray();
        if (!s.look.isEmpty() && fx.size() == 1 && fx[0].toObject().value("name").toString() == s.look) o.remove("effects");
        if (o.value("loop").toString() == QLatin1String("ken-burns")) o.remove("loop");
        main[i] = o;
    }
    plan->insert("main", main);

    QJsonArray moments;
    for (const QJsonValue &v : plan->value("moments").toArray()) {
        const QJsonObject o = v.toObject();
        const QString e = o.value("effect").toString();
        const double d = num(o.value("duration"), 0);
        const bool auto_ = (e == QLatin1String("zoom-punch") && std::abs(d - kPunchSec) < 0.051)
                           || (e == QLatin1String("flash") && std::abs(d - kFlashSec) < 0.051);
        if (!auto_) moments.append(o);
    }
    plan->insert("moments", moments);

    QJsonArray texts = plan->value("texts").toArray();
    for (int i = 0; i < texts.size(); ++i) {
        QJsonObject o = texts[i].toObject();
        const QString preset = o.value("style").toString();
        if (!presetMoves(preset, true) && o.value("in").toString() == s.textIn) o.remove("in");
        if (!presetMoves(preset, false) && o.value("out").toString() == s.textOut) o.remove("out");
        texts[i] = o;
    }
    plan->insert("texts", texts);
}

}  // namespace

QStringList adStyleNames()
{
    return {"energetic", "bold", "playful", "elegant", "luxury", "minimal"};
}

QStringList applyAdStyle(const QJsonObject &before, const Draft &d, const MediaIndex &index, QJsonObject *plan)
{
    const QString previousStyle = before.value("style").toString();
    if (!plan->contains("style") && !previousStyle.isEmpty()) plan->insert("style", previousStyle);
    const QString style = plan->value("style").toString().trimmed().toLower();
    const bool restyle = style != previousStyle;
    if (restyle && adStyleNames().contains(previousStyle)) strip(spec(previousStyle), plan);
    if (!adStyleNames().contains(style)) return {};
    plan->insert("style", style);
    const Spec s = spec(style);

    // With the style unchanged, only new things are styled.
    QSet<QString> oldClips, oldTexts;
    for (const QJsonValue &v : before.value("main").toArray()) oldClips << clipKey(v.toObject());
    for (const QJsonValue &v : before.value("texts").toArray()) oldTexts << v.toObject().value("text").toString();
    auto fresh = [&](const QJsonObject &o) { return restyle || !oldClips.contains(clipKey(o)); };

    QJsonArray main = plan->value("main").toArray();
    const QList<double> lengths = mainLengths(main);
    QList<double> cuts;  // only cuts next to styled clips
    double cursor = 0;
    for (int i = 0; i < main.size(); ++i) {
        QJsonObject o = main[i].toObject();
        const bool isNew = fresh(o);
        const bool cutNew = i > 0 && (isNew || fresh(main[i - 1].toObject()));
        const QString id = index.materialId(o.value("media").toString());
        const Material *m = id.isEmpty() ? nullptr : d.material(id);
        if (cutNew) {
            cuts << cursor;
            if (!o.contains("transition") && !s.transitions.isEmpty()) {
                o.insert("transition", s.transitions[(i - 1) % s.transitions.size()]);
                o.insert("transition_duration", round2(std::min(s.transitionSec, 0.8 * std::min(lengths[i - 1], lengths[i]))));
            }
        }
        if (isNew && !s.look.isEmpty() && !o.contains("effects") && m && m->kind != MaterialKind::Audio)
            o.insert("effects", QJsonArray{QJsonObject{{"name", s.look}, {"amount", s.lookAmount}}});
        if (isNew && m && m->kind == MaterialKind::Image && !o.contains("loop") && !o.contains("keys"))
            o.insert("loop", "ken-burns");
        cursor += lengths[i];
        main[i] = o;
    }
    plan->insert("main", main);

    QJsonArray moments = plan->value("moments").toArray();
    auto momentNear = [&](double t) {
        for (const QJsonValue &v : moments)
            if (std::abs(num(v.toObject().value("at"), -10) - t) < 0.2) return true;
        return false;
    };
    for (int k = 0; k < cuts.size(); ++k) {
        if (s.flashFirstCut && restyle && k == 0 && !momentNear(cuts[k]))
            moments.append(QJsonObject{{"effect", "flash"}, {"at", round2(cuts[k])}, {"duration", kFlashSec}});
        else if (s.punchOnCuts && !momentNear(cuts[k]))
            moments.append(QJsonObject{{"effect", "zoom-punch"}, {"at", round2(cuts[k])}, {"duration", kPunchSec}, {"amount", 0.8}});
    }
    if (!moments.isEmpty()) plan->insert("moments", moments);

    QJsonArray texts = plan->value("texts").toArray();
    for (int i = 0; i < texts.size(); ++i) {
        QJsonObject o = texts[i].toObject();
        if (!restyle && oldTexts.contains(o.value("text").toString())) continue;
        const QString preset = o.value("style").toString();
        if (!o.contains("in") && !presetMoves(preset, true)) o.insert("in", s.textIn);
        if (!o.contains("out") && !presetMoves(preset, false)) o.insert("out", s.textOut);
        texts[i] = o;
    }
    plan->insert("texts", texts);
    // Music-driven edits cut on the music: beats for lively styles, whole bars for calm ones.
    if (restyle && !plan->contains("beat_sync") && !plan->value("audio").toArray().isEmpty())
        plan->insert("beat_sync", style == QLatin1String("elegant") || style == QLatin1String("luxury") ? "bars" : "beats");
    if (!restyle) return {};
    return {QStringLiteral("styled the edit as %1").arg(style)};
}

}  // namespace mtg::ai
