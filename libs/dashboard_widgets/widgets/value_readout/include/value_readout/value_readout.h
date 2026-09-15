#ifndef VALUE_READOUT_WIDGET_H
#define VALUE_READOUT_WIDGET_H

#include "value_readout/config.h"
#include "dashboard/stale_aware.h"
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QFont>
#include <QString>

#include <memory>
#include <string_view>
#include <vector>

#include "dashboard/expression_subscription.h"

class QPainter;

class ValueReadoutWidget : public QWidget, public dashboard::StaleAware
{
	Q_OBJECT

public:
	using config_t = ValueReadoutConfig_t;
	static constexpr std::string_view kFriendlyName = "Value Readout";
	static constexpr widget_type_t kWidgetType = widget_type_t::value_readout;

	explicit ValueReadoutWidget(const ValueReadoutConfig_t& cfg, QWidget* parent = nullptr);
	const config_t& getConfig() const { return _cfg; }

	void setValue(double value);

	// The one binding this widget has. Dashes replace the number while the
	// stream is quiet: a readout holding its last value looks like a reading.
	void setValueStale(bool stale);

	std::vector<std::string_view> staleBindings() const override { return {"value"}; }
	void setBindingStale(std::string_view binding, bool stale) override;
	bool isBindingStale(std::string_view binding) const override;

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	void drawContents(QPainter* painter);

	// Renders `value` the way this readout is configured to show it. Static and
	// config-taking so a test can reach it without a widget or a display.
	static QString renderValue(const ValueReadoutConfig_t& cfg, double value);

	ValueReadoutConfig_t _cfg;
	double _value; // current value

	// What is actually drawn. Repaints are decided on this rather than on
	// _value, because the text changes far less often than the reading does --
	// with `decimals: 0`, a gauge moving continuously still redraws "95" over
	// and over. Comparing the rendered string covers every format, where
	// comparing a rounded integer only covered the integer one.
	QString _rendered_text;
	bool _value_valid = false;
	bool _stale = false;

	QFont _labelFont;
	QFont _valueFont;

    dashboard::ExpressionSubscriptionPtr<double> _expression_parser;
};

#endif // VALUE_READOUT_WIDGET_H
