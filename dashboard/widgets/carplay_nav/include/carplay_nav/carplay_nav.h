#ifndef CARPLAY_NAV_WIDGET_H_
#define CARPLAY_NAV_WIDGET_H_

#include "carplay_nav/config.h"
#include "carplay_nav/format.h"
#include "dashboard/widget_types.h"

#include "pub_sub/zenoh_subscriber.h"
#include "carplay_nav.capnp.h"

#include <QtWidgets/QWidget>
#include <QFont>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QString>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

// Supplemental CarPlay widget: the turn card. Subscribes to the driver node's
// route-guidance topic only -- no USB/AirPlay knowledge here, and it works
// whether or not the projected video surface is on screen.
class CarPlayNavWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = CarPlayNavConfig_t;
    static constexpr std::string_view kFriendlyName = "CarPlay Navigation";
    static constexpr widget_type_t kWidgetType = widget_type_t::carplay_nav;

    CarPlayNavWidget(CarPlayNavConfig_t cfg, QWidget* parent = nullptr);
    ~CarPlayNavWidget();
    const config_t& getConfig() const { return _cfg; }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    // Runs on the zenoh subscriber thread.
    void onNav(CarPlayNav::Reader reader);

    void paintIdle(QPainter& p, const QRectF& bounds);
    void paintGuidance(QPainter& p, const QRectF& bounds);

    // Builds the maneuver arrow inside a unit box centred on the origin, so the
    // caller can scale it to whatever room the card leaves.
    static QPainterPath arrowPath(carplay_nav::ManeuverGlyph glyph);

    CarPlayNavConfig_t _cfg;

    // Guards everything below; written by the subscriber thread, read by paint.
    std::mutex _mutex;
    bool _active = false;
    QString _road_name;
    QString _after_road_name;
    QString _destination_name;
    float _maneuver_angle_deg = 0.0f;
    float _distance_to_maneuver_m = 0.0f;
    float _distance_remaining_m = 0.0f;
    float _time_remaining_sec = 0.0f;
    uint64_t _eta_epoch_sec = 0;

    // Paint-path caches, for the same reason now_playing has them: the font
    // family came from a QFontDatabase registration and the metrics were rebuilt
    // on every repaint, and both derive from a scale that moves far more slowly
    // than the frame rate.
    QString _font_family;
    void rebuildFontsFor(qreal scale);
    qreal _font_scale = -1.0;
    QFont _distance_font;
    QFont _road_font;
    QFont _detail_font;
    std::unique_ptr<QFontMetricsF> _distance_fm;
    std::unique_ptr<QFontMetricsF> _road_fm;
    std::unique_ptr<QFontMetricsF> _detail_fm;

    std::unique_ptr<pub_sub::ZenohTypedSubscriber<CarPlayNav>> _sub;
};

#endif  // CARPLAY_NAV_WIDGET_H_
