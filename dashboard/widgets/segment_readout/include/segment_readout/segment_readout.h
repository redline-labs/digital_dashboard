#ifndef SEGMENT_READOUT_WIDGET_H
#define SEGMENT_READOUT_WIDGET_H

#include "segment_readout/config.h"
#include "dashboard/widget_types.h"
#include "dashboard/expression_subscription.h"

#include <QFont>
#include <QSize>
#include <QString>
#include <QWidget>

#include <memory>
#include <string_view>

class QPainter;

class SegmentReadoutWidget : public QWidget
{
    Q_OBJECT

  public:
    using config_t = SegmentReadoutConfig_t;
    static constexpr std::string_view kFriendlyName = "Segment Readout";
    static constexpr widget_type_t kWidgetType = widget_type_t::segment_readout;

    explicit SegmentReadoutWidget(const SegmentReadoutConfig_t& cfg, QWidget* parent = nullptr);
    const config_t& getConfig() const { return _cfg; }

  public slots:
    void setValue(double value);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    // Rebuilds the segment font for the current widget size. Cheap to call every
    // frame; does nothing unless the size actually moved.
    void rebuildFontFor(const QSize& size);

    SegmentReadoutConfig_t _cfg;

    // The string the value renders to, and the full-house string of the same
    // width drawn behind it.
    QString _text;
    QString _ghost;
    QString _prefix;

    QString _segment_family;
    QString _caption_family;
    QFont _segment_font;
    QFont _caption_font;
    QSize _font_size_for;

    dashboard::ExpressionSubscriptionPtr<double> _expression_parser;
};

#endif // SEGMENT_READOUT_WIDGET_H
