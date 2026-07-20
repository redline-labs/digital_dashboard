#include "value_readout/value_readout.h"

#include <QPainter>

#include <spdlog/spdlog.h>

#include "dashboard/expression_subscription.h"
#include "dashboard/widget_fonts.h"

ValueReadoutWidget::ValueReadoutWidget(const ValueReadoutConfig_t& cfg, QWidget* parent)
	: QWidget(parent), _cfg{cfg}, _value{0.0}
{
	// Load fonts similar to other widgets
	QString family = dashboard::loadResourceFont(":/fonts/futura.ttf", "Helvetica");
	_labelFont = QFont(family, 14, QFont::DemiBold);
	_valueFont = QFont(family, 40, QFont::Bold);

	_expression_parser = dashboard::makeExpressionSubscription<double>(
		_cfg.schema_type, _cfg.value_expression, _cfg.zenoh_key,
		this, &ValueReadoutWidget::setValue, "value readout");
}

void ValueReadoutWidget::setValue(double value)
{
	_value = value;
	update();
}

void ValueReadoutWidget::paintEvent(QPaintEvent* e)
{
	Q_UNUSED(e);
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
		drawContents(&p);
}

void ValueReadoutWidget::drawContents(QPainter* painter)
{
	painter->save();

	QRectF bounds(0, 0, width(), height());

	// Colors
	const QColor labelColor(255, 165, 0); // orange
	const QColor valueColor(255, 255, 255); // white

	// Layout: horizontal alignment
	Qt::Alignment hAlign = Qt::AlignLeft;
	switch (_cfg.alignment) {
		case ValueReadoutAlignment::left:
			hAlign = Qt::AlignLeft;
			break;

		case ValueReadoutAlignment::right:
			hAlign = Qt::AlignRight;
			break;

		case ValueReadoutAlignment::center:
		default:
			hAlign = Qt::AlignHCenter;
			break;
	}

	// Scale fonts relative to a reference design size
	constexpr float kBaseWidth = 140.0f;
	constexpr float kBaseHeight = 90.0f;
	constexpr float kBaseLabelPt = 14.0f;
	constexpr float kBaseValuePt = 40.0f;
	constexpr float kMinPt = 6.0f;

	const float sx = bounds.width() / kBaseWidth;
	const float sy = bounds.height() / kBaseHeight;
	const float s = std::min(sx, sy);

	QFont scaledLabel = _labelFont;
	scaledLabel.setPointSizeF(std::max(kMinPt, kBaseLabelPt * s));
	QFont scaledValue = _valueFont;
	scaledValue.setPointSizeF(std::max(kMinPt, kBaseValuePt * s));

	// Label
	painter->setPen(labelColor);
	painter->setFont(scaledLabel);
	QRectF labelRect = bounds.adjusted(0, 0, 0, 0); // top strip
	painter->drawText(labelRect, hAlign | Qt::AlignTop, QString::fromStdString(_cfg.label_text));

	// Value
	painter->setPen(valueColor);
	painter->setFont(scaledValue);
	// Maintain relative vertical offset while allowing scaling
	QRectF valueRect = bounds.adjusted(0, -bounds.height() * 0.20f, 0, 0);

	QString valueText = QString::number(static_cast<int>(std::round(_value)));
	painter->drawText(valueRect, hAlign | Qt::AlignVCenter, valueText);

	painter->restore();
}

#include "value_readout/moc_value_readout.cpp"
