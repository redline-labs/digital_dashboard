#ifndef VALUE_READOUT_WIDGET_H
#define VALUE_READOUT_WIDGET_H

#include "value_readout/config.h"
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QFont>

#include <memory>
#include <string_view>

#include "dashboard/expression_subscription.h"

class QPainter;

class ValueReadoutWidget : public QWidget
{
	Q_OBJECT

public:
	using config_t = ValueReadoutConfig_t;
	static constexpr std::string_view kFriendlyName = "Value Readout";
	static constexpr widget_type_t kWidgetType = widget_type_t::value_readout;

	explicit ValueReadoutWidget(const ValueReadoutConfig_t& cfg, QWidget* parent = nullptr);
	const config_t& getConfig() const { return _cfg; }

	void setValue(double value);

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	void drawContents(QPainter* painter);

	ValueReadoutConfig_t _cfg;
	double _value; // current value

	// What is actually drawn: a rounded integer. Repaints are decided on this
	// rather than on _value, because the text changes far less often than the
	// reading does. _value_valid distinguishes "never set" from "set to 0".
	long long _rendered_value = 0;
	bool _value_valid = false;

	QFont _labelFont;
	QFont _valueFont;

    dashboard::ExpressionSubscriptionPtr<double> _expression_parser;
};

#endif // VALUE_READOUT_WIDGET_H
