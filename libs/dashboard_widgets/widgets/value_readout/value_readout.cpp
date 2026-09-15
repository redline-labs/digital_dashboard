#include "value_readout/value_readout.h"

#include <QPainter>

#include <spdlog/spdlog.h>

#include "dashboard/expression_subscription.h"
#include "qt_helpers/widget_colors.h"
#include "qt_helpers/widget_fonts.h"

#include <cmath>

ValueReadoutWidget::ValueReadoutWidget(const ValueReadoutConfig_t& cfg, QWidget* parent)
	: QWidget(parent), _cfg{cfg}, _value{0.0}
{
	// Load fonts similar to other widgets
	QString family = qt_helpers::loadResourceFont(":/fonts/futura.ttf", "Helvetica");
	_labelFont = QFont(family, 14, QFont::DemiBold);
	_valueFont = QFont(family, 40, QFont::Bold);
	_labelFont.setItalic(_cfg.italic);
	_valueFont.setItalic(_cfg.italic);

	_expression_parser = dashboard::makeExpressionSubscription<double>(
		_cfg.schema_type, _cfg.value_expression, _cfg.zenoh_key,
		this, &ValueReadoutWidget::setValue, "value readout");
}

QString ValueReadoutWidget::renderValue(const ValueReadoutConfig_t& cfg, double value)
{
	QString text;

	switch (cfg.format)
	{
		case ValueReadoutFormat::lap_time:
		{
			// A lap time is a duration in seconds, shown the way a timing screen
			// shows it. Negative is not a lap time; it reads as a placeholder
			// rather than as "-1:00.00".
			if (!(value > 0.0))
			{
				return QStringLiteral("--:--.--");
			}
			// Round to hundredths first, so 119.999 becomes 2:00.00 and not
			// 1:60.00 -- rounding after the split is where that bug lives.
			const long long hundredths = std::llround(value * 100.0);
			const long long minutes = hundredths / 6000;
			const long long seconds = (hundredths / 100) % 60;
			const long long fraction = hundredths % 100;
			text = QString("%1:%2.%3")
			           .arg(minutes)
			           .arg(seconds, 2, 10, QChar('0'))
			           .arg(fraction, 2, 10, QChar('0'));
			return text;
		}

		case ValueReadoutFormat::number:
			text = QString::number(value, 'f', static_cast<int>(cfg.decimals));
			break;
	}

	// QString::number renders -0.4 at zero decimals as "-0"; a readout showing a
	// signed zero looks like a fault rather than a rounding.
	if (text.startsWith('-') && text.mid(1).toDouble() == 0.0)
	{
		text = text.mid(1);
	}

	if (cfg.show_sign && !text.startsWith('-'))
	{
		text.prepend('+');
	}

	if (!cfg.units.empty())
	{
		text += QString::fromStdString(cfg.units);
	}

	return text;
}

void ValueReadoutWidget::setValue(double value)
{
	if (!std::isfinite(value))
	{
		return;
	}

	const double clamped = std::clamp(value, -1.0e9, 1.0e9);
	const QString rendered = renderValue(_cfg, clamped);
	if (_value_valid && rendered == _rendered_text)
	{
		return;
	}

	_value = clamped;
	_rendered_text = rendered;
	_value_valid = true;
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

	const QColor labelColor = qt_helpers::toQColor(_cfg.label_color, QColor(255, 165, 0));
	const QColor valueColor = qt_helpers::toQColor(_cfg.value_color, Qt::white);

	// Layout: horizontal alignment. Every case is named and there is no
	// `default:`, so adding an alignment is a build error here rather than a
	// silent left-align at runtime.
	Qt::Alignment hAlign = Qt::AlignLeft;
	switch (_cfg.alignment) {
		case ValueReadoutAlignment::left:
			hAlign = Qt::AlignLeft;
			break;

		case ValueReadoutAlignment::right:
			hAlign = Qt::AlignRight;
			break;

		case ValueReadoutAlignment::center:
			hAlign = Qt::AlignHCenter;
			break;
	}

	// Scale fonts relative to a reference design size
	constexpr float kBaseWidth = 140.0f;
	constexpr float kBaseHeight = 90.0f;
	constexpr float kBaseLabelPt = 14.0f;
	constexpr float kBaseValuePt = 40.0f;
	constexpr float kMinPt = 6.0f;
	// Breathing space between the label baseline and the top of the value.
	constexpr float kGapFraction = 0.04f;
	// Horizontal inset, so text never butts up against the widget's edge and
	// look like it has been cut off by its neighbour.
	constexpr float kInsetFraction = 0.02f;

	const float sx = bounds.width() / kBaseWidth;
	const float sy = bounds.height() / kBaseHeight;
	const float s = std::min(sx, sy);

	QFont scaledLabel = _labelFont;
	scaledLabel.setPointSizeF(std::max(kMinPt, kBaseLabelPt * s));

	// The label owns a strip at the top sized to its own metrics, and the value
	// owns everything below it.
	//
	// This used to be two overlapping rectangles: the label was drawn AlignTop
	// in the full bounds while the value was drawn AlignVCenter in a rect
	// shifted UP by 20% of the height. At the sizes the MoTeC layouts use that
	// put ~12px of the value's ascenders through the label, and -- worse -- it
	// started the value's rect above the widget's own top edge, so a tall value
	// painted outside its box onto whichever widget was above it in the layout.
	const qreal inset = bounds.width() * kInsetFraction;
	QRectF labelRect;
	QRectF valueRect = bounds.adjusted(inset, 0, -inset, 0);
	const QString labelText = QString::fromStdString(_cfg.label_text);
	if (!labelText.isEmpty())
	{
		const QFontMetricsF label_fm(scaledLabel);
		const qreal label_height = std::min<qreal>(label_fm.height(), bounds.height() * 0.5);
		labelRect = QRectF(valueRect.left(), bounds.top(), valueRect.width(), label_height);
		valueRect.setTop(labelRect.bottom() + bounds.height() * kGapFraction);

		painter->setPen(labelColor);
		painter->setFont(scaledLabel);
		// Elided, not wrapped: a label that wrapped would push into the value's
		// half of the widget and collide with it.
		painter->drawText(labelRect, hAlign | Qt::AlignVCenter,
		                  label_fm.elidedText(labelText, Qt::ElideRight, labelRect.width()));
	}

	// The pre-rendered text, not a fresh conversion of _value: the formatting
	// has already been done where the value arrived, and doing it again here
	// would be per-frame work for a string that rarely changes.
	const QString valueText = _value_valid ? _rendered_text : renderValue(_cfg, 0.0);

	// Shrink the value until it fits the space the label left over. A readout
	// that silently draws wider than its own widget is how a "-48" ends up
	// sitting on top of its neighbour.
	QFont scaledValue = _valueFont;
	qreal value_pt = std::max(kMinPt, kBaseValuePt * s);
	for (int i = 0; i < 16; ++i)
	{
		scaledValue.setPointSizeF(value_pt);
		const QFontMetricsF value_fm(scaledValue);
		if ((value_fm.horizontalAdvance(valueText) <= valueRect.width() &&
		     value_fm.height() <= valueRect.height()) ||
		    value_pt <= kMinPt)
		{
			break;
		}
		value_pt = std::max<qreal>(kMinPt, value_pt * 0.92);
	}

	painter->setPen(valueColor);
	painter->setFont(scaledValue);
	painter->drawText(valueRect, hAlign | Qt::AlignVCenter, valueText);

	painter->restore();
}

#include "value_readout/moc_value_readout.cpp"
