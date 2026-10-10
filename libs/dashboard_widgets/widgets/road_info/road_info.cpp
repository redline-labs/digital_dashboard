#include "road_info/road_info.h"

#include <QFont>
#include <QVBoxLayout>

#include <optional>

#include "dashboard/typed_subscription.h"
#include "map_horizon.capnp.h"
#include "qt_helpers/widget_colors.h"

namespace
{

// km/h -> mph, rounded. US signs are in mph and the graph stores km/h, so this
// conversion happens exactly once, here, on the way to the display.
int toMph(std::uint16_t kph)
{
    return static_cast<int>((kph * 1000 + 804) / 1609);
}

// ON A ZENOH RX THREAD: the horizon reduced to what the panel shows.
std::optional<RoadInfoWidget::State> decodeHorizon(::MapHorizon::Reader horizon)
{
    RoadInfoWidget::State next;
    next.haveHorizon = true;
    next.matched = horizon.getHasPosition();
    if (next.matched)
    {
        const auto position = horizon.getPosition();
        next.confidence = position.getConfidence();
        next.sigmaM = position.getSigmaM();
    }

    // The profile runs the vehicle is standing in. Profiles are
    // FILTERED, not switched: a kind this build does not know is
    // ignored, which is the whole reason the horizon can grow a
    // curvature profile later without breaking this widget.
    const std::uint32_t at = next.matched ? horizon.getPosition().getOffsetCm() : 0;
    for (const auto profile : horizon.getProfiles())
    {
        if (profile.getStartOffsetCm() > at || profile.getEndOffsetCm() < at)
        {
            continue;
        }
        const auto value = profile.getValue();
        switch (value.which())
        {
            case ::HorizonProfile::Value::ROAD_NAME:
                next.name = value.getRoadName().cStr();
                break;
            case ::HorizonProfile::Value::ROAD_REF:
                next.ref = value.getRoadRef().cStr();
                break;
            case ::HorizonProfile::Value::SPEED:
            {
                const auto speed = value.getSpeed();
                next.hasPosted = speed.getHasPosted();
                next.postedKph = speed.getPostedKph();
                break;
            }
            case ::HorizonProfile::Value::UNKNOWN:
            case ::HorizonProfile::Value::ROAD_CLASS:
            case ::HorizonProfile::Value::LANE_COUNT:
            case ::HorizonProfile::Value::SEGMENT:
                break;
        }
    }
    return next;
}

} // namespace

RoadInfoWidget::RoadInfoWidget(const RoadInfoConfig_t& cfg, QWidget* parent) :
    QWidget(parent),
    _cfg(cfg)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(0);

    _name = new QLabel(this);
    _detail = new QLabel(this);
    _name->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    _detail->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    layout->addWidget(_name);
    layout->addWidget(_detail);

    applyConfig();
    refresh();

    // Empty is a legitimate configuration: a layout that wants the panel
    // without a live bus, in the editor or in a screenshot.
    _subscription = dashboard::makeTypedSubscription<::MapHorizon, State>(
        _cfg.horizon_zenoh_key, &decodeHorizon, this, &RoadInfoWidget::setState);
}

RoadInfoWidget::~RoadInfoWidget() = default;

void RoadInfoWidget::setState(State state)
{
    _state = std::move(state);
    refresh();
}

void RoadInfoWidget::applyConfig()
{
    QFont nameFont(QString::fromStdString(_cfg.font), _cfg.name_font_size);
    nameFont.setBold(true);
    _name->setFont(nameFont);

    _detail->setFont(QFont(QString::fromStdString(_cfg.font), _cfg.detail_font_size));

    // qt_helpers::toQColor is the ONE way a configured colour becomes a QColor
    // in this tree -- an invalid one paints as transparent black and makes Qt
    // drop the whole stylesheet rule, which presents as an invisible widget
    // with nothing said about it.
    _name->setStyleSheet(QString("color: %1;")
                             .arg(qt_helpers::toQColor(_cfg.text_color).name(QColor::HexArgb)));
    _detail->setStyleSheet(QString("color: %1;")
                               .arg(qt_helpers::toQColor(_cfg.detail_color).name(QColor::HexArgb)));

    setStyleSheet(
        QString("background-color: %1;")
            .arg(qt_helpers::toQColor(_cfg.background_color, Qt::transparent).name(QColor::HexArgb)));

    _detail->setVisible(_cfg.show_ref || _cfg.show_speed || _cfg.show_confidence);
}

void RoadInfoWidget::refresh()
{
    const State current = state();

    if (!current.haveHorizon)
    {
        _name->setText(QString::fromStdString(_cfg.no_fix_text));
        _detail->clear();
        return;
    }
    if (!current.matched)
    {
        // A car park, a private drive, a road that is not in the map. NORMAL,
        // and distinct from having no fix at all.
        _name->setText(QString::fromStdString(_cfg.no_road_text));
        _detail->clear();
        return;
    }

    QString name;
    if (_cfg.show_name)
    {
        name = QString::fromStdString(current.name);
    }
    if (name.isEmpty() && !current.ref.empty())
    {
        // An unnamed road with a route number -- a freeway ramp, most often.
        // Better than a blank panel.
        name = QString::fromStdString(current.ref);
    }
    _name->setText(name);

    QStringList details;
    if (_cfg.show_ref && !current.ref.empty() && name != QString::fromStdString(current.ref))
    {
        details << QString::fromStdString(current.ref);
    }
    if (_cfg.show_speed)
    {
        if (current.hasPosted)
        {
            details << (_cfg.speed_in_mph ? QString("%1 mph").arg(toMph(current.postedKph))
                                          : QString("%1 km/h").arg(current.postedKph));
        }
        else
        {
            // NOT the free-flow speed. That number always exists and is what a
            // router costs with; showing it here would tell the driver a limit
            // nobody posted. See map_common.capnp.
            details << QString::fromStdString(_cfg.no_limit_text);
        }
    }
    if (_cfg.show_confidence)
    {
        // The percent sign is appended rather than written in the format
        // string: QString::arg has no %% escape, so "%1%%" renders as "100%%".
        details << QString::number(current.confidence) + "% ±" +
                       QString::number(static_cast<double>(current.sigmaM), 'f', 1) + " m";
    }

    _detail->setText(details.join("   "));
}
