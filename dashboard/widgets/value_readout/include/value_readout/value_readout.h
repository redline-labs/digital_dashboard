#ifndef VALUE_READOUT_WIDGET_H
#define VALUE_READOUT_WIDGET_H

#include "value_readout/config.h"

#include <QWidget>
#include <QFont>

#include <memory>
#include <string_view>

namespace zenoh_subscriber {
class ZenohSubscriber;
}

class QPainter;

class ValueReadoutWidget : public QWidget
{
	Q_OBJECT

public:
	using config_t = ValueReadoutConfig_t;
	static constexpr std::string_view kWidgetName = "value_readout";

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

	std::unique_ptr<zenoh_subscriber::ZenohSubscriber> _expression_parser;
};

#endif // VALUE_READOUT_WIDGET_H
