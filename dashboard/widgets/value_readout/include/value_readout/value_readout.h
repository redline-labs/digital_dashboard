#ifndef VALUE_READOUT_WIDGET_H
#define VALUE_READOUT_WIDGET_H

#include "value_readout/config.h"
#include "dashboard/widget_types.h"

#include <QWidget>
#include <QFont>

#include <memory>
#include <string_view>

namespace pub_sub { class ZenohExpressionSubscriber; }

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

private slots:
	void onValueEvaluated(double v);

private:
	void drawContents(QPainter* painter);

	ValueReadoutConfig_t _cfg;
	double _value; // current value

	QFont _labelFont;
	QFont _valueFont;

    std::unique_ptr<pub_sub::ZenohExpressionSubscriber> _expression_parser;
};

#endif // VALUE_READOUT_WIDGET_H
