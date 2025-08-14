#ifndef MOTEC_CDL3_TACHOMETER_H
#define MOTEC_CDL3_TACHOMETER_H

#include "motec_cdl3_tachometer/config.h"
#include "zenoh.hxx"

#include <QWidget>
#include <QFont>

#include <memory>
#include <string_view>
#include <array>
#include <vector>

namespace expression_parser {
class ExpressionParser;
}

class QPainter;

class MotecCdl3Tachometer : public QWidget {
    Q_OBJECT

public:
    using config_t = MotecCdl3TachometerConfig_t;
    static constexpr std::string_view kWidgetName = "motec_cdl3_tachometer";

    explicit MotecCdl3Tachometer(const MotecCdl3TachometerConfig_t& cfg, QWidget* parent = nullptr);

    void setZenohSession(std::shared_ptr<zenoh::Session> session);

protected:
    void paintEvent(QPaintEvent* event) override;

private slots:
    void setRpm(float rpm);

private:
    // drawing helpers
    void drawSweepBands(QPainter* painter); // segmented arc fill for current RPM
    void drawTicksAndLabels(QPainter* painter); // triangles and single-digit labels

    // mapping helpers
    float mapRpmToAngleDeg(float rpm) const;  // non-constant radius sweep
    float mapAngleToRadius(float angle_deg) const; // varying radius function for the path

    // precomputed arc-length LUT (baseline ellipse) for even spacing
    void buildArcLUT();
    float angleAtU(float u) const; // 0..1 -> angle along the arc using LUT
    void buildStaticGeometry();    // precompute per-segment and per-tick geometry

    MotecCdl3TachometerConfig_t _cfg;
    float _rpm; // current rpm

    // Fonts
    QFont _segmentFont; // DSEG-like font for labels

    // Optional live data
    std::shared_ptr<zenoh::Session> _zenoh_session;
    std::unique_ptr<expression_parser::ExpressionParser> _expression_parser;

    // LUT storage
    static constexpr int kLutSamples = 512;
    std::array<float, kLutSamples> _lutAngles{};
    std::array<float, kLutSamples> _lutLengths{};

    // Precomputed geometry
    static constexpr int kSegments = 46;
    std::array<float, kSegments> _segmentStartAngles{};  // a0 per segment
    std::array<float, kSegments> _segmentSpanDeg{};      // span (deg) per segment
    std::array<float, kSegments> _segmentLengthPx{};     // pen width per segment
    std::array<float, kSegments> _segmentRectAX{};       // rect a (x radius) per segment
    std::array<float, kSegments> _segmentRectBY{};       // rect b (y radius) per segment

    std::vector<float> _tickAngles; // 0..max_rpm step 1000
};

#endif // MOTEC_CDL3_TACHOMETER_H


