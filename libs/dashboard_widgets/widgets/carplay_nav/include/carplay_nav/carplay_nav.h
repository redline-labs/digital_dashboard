#ifndef CARPLAY_NAV_WIDGET_H_
#define CARPLAY_NAV_WIDGET_H_

#include "carplay_nav/config.h"
#include "carplay_nav/format.h"
#include "dashboard/widget_types.h"

#include "dashboard/typed_subscription.h"
#include "carplay_nav.capnp.h"

#include <QtWidgets/QWidget>
#include <QFont>
#include <QFontMetricsF>
#include <QPainterPath>
#include <QString>

#include <cstdint>
#include <memory>
#include <string_view>

// Supplemental CarPlay widget: the turn card. Subscribes to the driver node's
// route-guidance topic only -- no USB/AirPlay knowledge here, and it works
// whether or not the projected video surface is on screen.
class CarPlayNavWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = CarPlayNavConfig_t;

    CarPlayNavWidget(CarPlayNavConfig_t cfg, QWidget* parent = nullptr);
    ~CarPlayNavWidget();
    const config_t& getConfig() const { return _cfg; }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    // One guidance message, copied out on the zenoh thread. Each is a full
    // snapshot of the node's accumulated state, so the latest is all that
    // matters.
    struct Guidance
    {
        bool active = false;
        QString road_name;
        QString after_road_name;
        QString destination_name;
        float maneuver_angle_deg = 0.0f;
        float distance_to_maneuver_m = 0.0f;
        float distance_remaining_m = 0.0f;
        float time_remaining_sec = 0.0f;
        uint64_t eta_epoch_sec = 0;
    };

    // Qt thread.
    void setGuidance(Guidance guidance);

    void paintIdle(QPainter& p, const QRectF& bounds);
    void paintGuidance(QPainter& p, const QRectF& bounds);

    // Builds the maneuver arrow inside a unit box centred on the origin, so the
    // caller can scale it to whatever room the card leaves.
    static QPainterPath arrowPath(carplay_nav::ManeuverGlyph glyph);

    CarPlayNavConfig_t _cfg;

    // GUI thread only; written by the delivery tick, read by paint.
    Guidance _guidance;

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

    dashboard::TypedSubscriptionPtr<CarPlayNav, Guidance> _sub;
};

#endif  // CARPLAY_NAV_WIDGET_H_
