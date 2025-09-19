#ifndef MOTEC_C125_TACHOMETER_H
#define MOTEC_C125_TACHOMETER_H

#include "motec_c125_tachometer/config.h"

#include <QWidget>
#include <QFont>

#include <memory>
#include <string_view>

namespace pub_sub { class ZenohExpressionSubscriber; }

class QPainter;

class MotecC125Tachometer : public QWidget
{
    Q_OBJECT

public:
    using config_t = MotecC125TachometerConfig_t;
    static constexpr std::string_view kWidgetName = "motec_c125_tachometer";

    explicit MotecC125Tachometer(const MotecC125TachometerConfig_t& cfg, QWidget* parent = nullptr);
    const config_t& getConfig() const { return _cfg; }

    void setRpm(float rpm);

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

    MotecC125TachometerConfig_t _cfg;
    float _rpm; // current rpm

    // Fonts
    QFont _digitFont;

    // Optional live data support
    std::unique_ptr<pub_sub::ZenohExpressionSubscriber> _expression_parser;
};

#endif // MOTEC_C125_TACHOMETER_H



