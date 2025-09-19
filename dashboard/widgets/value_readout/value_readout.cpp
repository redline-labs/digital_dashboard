#include "value_readout/value_readout.h"

#include <QPainter>
#include <QFontDatabase>
#include <QMetaObject>

#include <spdlog/spdlog.h>

#include "pub_sub/zenoh_subscriber.h"

ValueReadoutWidget::ValueReadoutWidget(const ValueReadoutConfig_t& cfg, QWidget* parent)
	: QWidget(parent), _cfg{cfg}, _value{0.0}
{
	// Load fonts similar to other widgets
	int fontId = QFontDatabase::addApplicationFont(":/fonts/futura.ttf");
	QString family = (fontId == -1)
		? QStringLiteral("Helvetica")
		: QFontDatabase::applicationFontFamilies(fontId).at(0);

	_labelFont = QFont(family, 14, QFont::DemiBold);
	_valueFont = QFont(family, 40, QFont::Bold);

    try {
        _expression_parser = std::make_unique<pub_sub::ZenohExpressionSubscriber>(
			_cfg.schema_type, _cfg.value_expression, _cfg.zenoh_key);
		if (_expression_parser->isValid()) {
			_expression_parser->setResultCallback<double>([this](double v) {
				QMetaObject::invokeMethod(this, "onValueEvaluated", Qt::QueuedConnection, Q_ARG(double, v));
			});
		} else {
			_expression_parser.reset();
		}
	} catch (const std::exception& e) {
		SPDLOG_ERROR("ValueReadout expression parser init failed: {}", e.what());
		_expression_parser.reset();
	}
}

void ValueReadoutWidget::setValue(double value)
{
	_value = value;
	update();
}

void ValueReadoutWidget::onValueEvaluated(double v)
{
	setValue(v);
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
