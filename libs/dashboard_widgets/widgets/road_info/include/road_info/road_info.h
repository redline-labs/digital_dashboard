#ifndef ROAD_INFO_WIDGET_H
#define ROAD_INFO_WIDGET_H

// The road the vehicle is on.
//
// Subscribes nodes/map_match's horizon DIRECTLY rather than through the usual
// expression binding: ZenohExpressionSubscriber yields a double, and a road
// name is text. Everything else about the widget follows the ordinary
// registration -- one row in DASHBOARD_WIDGET_TABLE and it is in the palette,
// in the YAML decoder and reachable through widget_*_config.
//
// THREADING: the zenoh callback runs on an RX thread and may not touch Qt. It
// stores into a one-slot mailbox and posts a single queued invoke; the GUI-side
// handler drains it. Without that gate it is one QMetaCallEvent per message at
// 10 Hz forever -- the shape dashboard/widgets/map/map_widget.cpp settled on
// after the CarPlay widget fell behind doing it the other way.

#include <memory>
#include <string>

#include <QLabel>
#include <QWidget>

#include "dashboard/delivery_ticker.h"
#include "dashboard/widget_types.h"
#include "road_info/config.h"

class RoadInfoWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = RoadInfoConfig_t;
    static constexpr std::string_view kFriendlyName = "Road Info";
    static constexpr widget_type_t kWidgetType = widget_type_t::road_info;

    explicit RoadInfoWidget(const RoadInfoConfig_t& cfg, QWidget* parent = nullptr);
    ~RoadInfoWidget() override;

    const config_t& getConfig() const { return _cfg; }

    // What the widget last decoded. Exposed so a test can assert on the value
    // behind the pixels rather than on the pixels.
    struct State
    {
        bool haveHorizon { false };
        bool matched { false };
        std::string name;
        std::string ref;
        bool hasPosted { false };
        std::uint16_t postedKph { 0 };
        std::uint8_t confidence { 0 };
        float sigmaM { 0.0F };
    };

    State state() const { return _state; }

  private:
    void applyConfig();
    void refresh();
    void setState(State state);

    RoadInfoConfig_t _cfg;

    QLabel* _name { nullptr };
    QLabel* _detail { nullptr };

    State _state;

    // A dashboard::TypedSubscription<MapHorizon, State>, held by its base so
    // this header stays free of capnp: decoded on the RX thread, delivered
    // coalesced on the GUI thread.
    std::unique_ptr<dashboard::DeliveryTarget> _subscription;
};

#endif // ROAD_INFO_WIDGET_H
