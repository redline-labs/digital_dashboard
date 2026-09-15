#ifndef SEGMENT_READOUT_WIDGET_H
#define SEGMENT_READOUT_WIDGET_H

#include "segment_readout/config.h"
#include "dashboard/stale_aware.h"
#include "dashboard/widget_types.h"
#include "dashboard/expression_subscription.h"

#include <QFont>
#include <QSize>
#include <QString>
#include <QWidget>

#include <memory>
#include <string_view>
#include <vector>

class QPainter;

class SegmentReadoutWidget : public QWidget, public dashboard::StaleAware
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

  public:
    // Every cell goes dark: on a seven-segment display that is what no signal
    // looks like, and the ghosts behind it keep the shape of the readout.
    void setValueStale(bool stale);

    std::vector<std::string_view> staleBindings() const override { return {"value"}; }
    void setBindingStale(std::string_view binding, bool stale) override;
    bool isBindingStale(std::string_view binding) const override;

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

    bool _stale = false;

    QString _segment_family;
    QString _caption_family;
    QFont _segment_font;
    QFont _caption_font;
    QSize _font_size_for;

    dashboard::ExpressionSubscriptionPtr<double> _expression_parser;
};

#endif // SEGMENT_READOUT_WIDGET_H
