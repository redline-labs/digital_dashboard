#ifndef CIRCLE_TACHOMETER_H
#define CIRCLE_TACHOMETER_H

#include "circle_tachometer/config.h"
#include "zenoh.hxx"

#include <QWidget>
#include <QFont>

#include <memory>
#include <string_view>

namespace expression_parser {
class ExpressionParser;
}

class QPainter;

class CircleTachometer : public QWidget {
    Q_OBJECT

public:
    using config_t = CircleTachometerConfig_t;
    static constexpr std::string_view kWidgetName = "circle_tachometer";

    explicit CircleTachometer(const CircleTachometerConfig_t& cfg, QWidget* parent = nullptr);

    void setRpm(float rpm);
    void setZenohSession(std::shared_ptr<zenoh::Session> session);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void onRpmEvaluated(float rpm);

private:
    void drawDial(QPainter* painter);
    void drawBackdrop(QPainter* painter);
    void drawFilledArc(QPainter* painter);
    void drawTicks(QPainter* painter);
    void drawCenterDigit(QPainter* painter);

    float clampRpm(float rpm) const;

    CircleTachometerConfig_t _cfg;
    float _rpm; // current rpm

    // Fonts
    QFont _digitFont;

    // Optional live data support
    std::shared_ptr<zenoh::Session> _zenoh_session;
    std::unique_ptr<expression_parser::ExpressionParser> _expression_parser;
};

#endif // CIRCLE_TACHOMETER_H



