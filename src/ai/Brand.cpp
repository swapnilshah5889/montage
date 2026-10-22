#include "Brand.h"

#include "AdStyles.h"
#include "EditPlan.h"

#include <QColor>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <cmath>

namespace mtg::ai {

namespace {

bool matches(const QString &text, const char *pattern)
{
    return QRegularExpression(QLatin1String(pattern)).match(text).hasMatch();
}

// Black or white, whichever reads better on `background`.
QString readableOn(const QString &background)
{
    const QColor c(background);
    if (!c.isValid()) return QStringLiteral("#FFFFFF");
    auto lin = [](double v) { return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); };
    const double l = 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
    return l > 0.4 ? QStringLiteral("#111111") : QStringLiteral("#FFFFFF");
}

double planLength(const QJsonObject &plan)
{
    double length = 0;
    for (const QJsonValue &v : plan.value("main").toArray())
        if (const double s = itemSeconds(v.toObject()); !std::isnan(s)) length += s;
    for (const QJsonValue &v : plan.value("texts").toArray()) {
        const QJsonObject t = v.toObject();
        const double end = seconds(t.value("at")) + seconds(t.value("duration"));
        if (!std::isnan(end)) length = std::max(length, end);
    }
    return length;
}

}  // namespace

QVariantMap BrandKit::toMap() const
{
    return {{"name", name}, {"primary", primary}, {"accent", accent}, {"text", text}, {"font", font},
            {"logo", logo}, {"logoPosition", logoPosition}, {"style", style}, {"enabled", enabled}};
}

BrandKit BrandKit::fromMap(const QVariantMap &m)
{
    BrandKit k;
    k.name = m.value("name").toString();
    k.primary = m.value("primary").toString();
    k.accent = m.value("accent").toString();
    k.text = m.value("text", QStringLiteral("#FFFFFF")).toString();
    k.font = m.value("font").toString();
    k.logo = m.value("logo").toString();
    k.logoPosition = m.value("logoPosition", QStringLiteral("top-right")).toString();
    k.style = m.value("style").toString();
    k.enabled = m.value("enabled").toBool();
    return k;
}

BrandKit loadBrandKit()
{
    // Tests and scripted runs: a kit as JSON, without touching saved settings.
    if (const QByteArray json = qgetenv("MONTAGE_BRAND_JSON"); !json.isEmpty())
        return BrandKit::fromMap(QJsonDocument::fromJson(json).object().toVariantMap());
    QSettings s;
    return BrandKit::fromMap(s.value(QStringLiteral("brand/kit")).toMap());
}

void saveBrandKit(const BrandKit &kit)
{
    QSettings s;
    s.setValue(QStringLiteral("brand/kit"), kit.toMap());
}

bool brandApplies(const QString &request, const BrandKit &kit)
{
    if (kit.isEmpty()) return false;
    return kit.enabled
           || matches(request.toLower(), R"(\b(brand(ed|ing)?|on[- ]brand|brand kit|my colou?rs|our colou?rs|company colou?rs|house style|my logo|our logo)\b)");
}

QStringList applyBrand(const QString &request, const BrandKit &kit, const QJsonObject &before, const Draft &d,
                       const MediaIndex &index, QJsonObject *plan)
{
    if (!brandApplies(request, kit)) return {};
    const QString req = request.toLower();
    if (matches(req, R"(\b(remove|delete|without|get rid|no) (the |my |our )?(brand|branding|logo|colou?rs)\b)")) return {};
    const bool asked = matches(req, R"(\b(brand(ed|ing)?|on[- ]brand|brand kit|my colou?rs|our colou?rs|company colou?rs|house style)\b)");
    const bool namesColour = matches(req, R"(\b(colou?r|red|blue|green|yellow|white|black|orange|purple|pink|gold|grey|gray)\b|#[0-9a-f]{3,6})");
    const bool namesFont = matches(req, R"(\bfonts?\b|\btypeface\b)");
    QStringList notes;

    // House style for a new ad, unless the request names a feel of its own.
    const bool emptyBefore = before.value("main").toArray().isEmpty() && before.value("texts").toArray().isEmpty();
    const bool newAd = before.value("style").toString().isEmpty() && !plan->value("style").toString().isEmpty();
    const bool namesStyle = matches(req, R"(\b(energetic|energy|hype|bold|punchy|playful|fun|elegant|classy|luxury|luxurious|premium|minimal|minimalist|clean|style|vibe|mood|feel)\b)");
    if (!kit.style.isEmpty() && adStyleNames().contains(kit.style) && !namesStyle && before.value("style").toString().isEmpty()
        && (newAd || emptyBefore) && plan->value("style").toString() != kit.style) {
        plan->insert("style", kit.style);
        notes << QStringLiteral("used the brand's %1 style").arg(kit.style);
    }

    // Text: brand font everywhere new; brand colours on the ad text styles.
    QSet<QString> oldTexts;
    for (const QJsonValue &v : before.value("texts").toArray()) oldTexts << v.toObject().value("text").toString();
    const QString accent = kit.accent.isEmpty() ? kit.primary : kit.accent;
    QJsonArray texts = plan->value("texts").toArray();
    int styled = 0;
    for (qsizetype i = 0; i < texts.size(); ++i) {
        QJsonObject o = texts[i].toObject();
        if (!asked && oldTexts.contains(o.value("text").toString())) continue;
        const QString preset = o.value("style").toString();
        bool changed = false;
        if (!kit.font.isEmpty() && !namesFont) {
            changed |= o.value("font").toString() != kit.font;
            o.insert("font", kit.font);
        }
        if (!namesColour) {
            QString box, colour;
            if ((preset == QLatin1String("cta") || preset == QLatin1String("lower-third")) && !kit.primary.isEmpty()) box = kit.primary;
            else if ((preset == QLatin1String("badge") || preset == QLatin1String("price")) && !accent.isEmpty()) box = accent;
            if (!box.isEmpty()) {
                colour = readableOn(box);
                o.insert("box", box.toUpper());
                o.insert("color", colour);
                o.remove("gradient");
                changed = true;
            } else if ((preset == QLatin1String("title") || preset == QLatin1String("headline") || preset == QLatin1String("end-card")
                        || preset == QLatin1String("kinetic-caption")) && !kit.text.isEmpty()) {
                o.insert("color", kit.text.toUpper());
                o.insert("gradient", "none");  // the preset's gradient would tint the brand colour
                changed = true;
            }
        }
        if (changed) ++styled;
        texts[i] = o;
    }
    if (styled) {
        plan->insert("texts", texts);
        notes << QStringLiteral("applied brand colours and font to %1 text%2").arg(styled).arg(styled == 1 ? "" : "s");
    }

    // Text-only videos sit on the brand colour.
    if (plan->value("main").toArray().isEmpty() && !kit.primary.isEmpty() && !matches(req, R"(\b(background|backdrop)\b)")) {
        const QColor bg(plan->value("background").toString());
        if (!bg.isValid() || bg == QColor(Qt::black)) plan->insert("background", kit.primary.toUpper());
    }

    // The logo as a corner watermark: on new edits and new ads, or when asked for.
    const bool newEdit = before.value("texts").toArray().isEmpty() && before.value("overlays").toArray().isEmpty()
                         && (emptyBefore || newAd);
    if (!kit.logo.isEmpty() && (newEdit || asked || matches(req, R"(\blogo\b)"))) {
        QString alias;
        for (const QString &id : index.materialIds())
            if (const Material *m = d.material(id); m && QFileInfo(m->path) == QFileInfo(kit.logo)) alias = index.alias(id);
        bool used = false;
        for (const char *key : {"main", "overlays"})
            for (const QJsonValue &v : plan->value(QLatin1String(key)).toArray()) used |= v.toObject().value("media").toString() == alias;
        if (used && !matches(req, R"(\b(logo)\b)")) {
            // The model placed the logo itself: keep it, in the brand's spot.
            QJsonArray overlays = plan->value("overlays").toArray();
            for (qsizetype i = 0; i < overlays.size(); ++i) {
                QJsonObject o = overlays[i].toObject();
                if (o.value("media").toString() != alias) continue;
                o.insert("position", kit.logoPosition);
                o.insert("scale", 0.14);
                overlays[i] = o;
            }
            plan->insert("overlays", overlays);
        }
        const double length = planLength(*plan);
        if (!alias.isEmpty() && !used && length > 0) {
            QJsonArray overlays = plan->value("overlays").toArray();
            overlays.append(QJsonObject{{"media", alias}, {"at", 0}, {"from", 0}, {"to", std::round(length * 100) / 100},
                                        {"position", kit.logoPosition}, {"scale", 0.14}});
            plan->insert("overlays", overlays);
            notes << QStringLiteral("added your logo");
        }
    }
    return notes;
}

}  // namespace mtg::ai
