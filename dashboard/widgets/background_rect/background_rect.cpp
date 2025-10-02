#include "background_rect/background_rect.h"

#include <QPainter>
#include <QLinearGradient>

BackgroundRectWidget::BackgroundRectWidget(const BackgroundRectConfig_t& cfg, QWidget* parent)
	: QWidget(parent), _cfg{cfg}
{}

void BackgroundRectWidget::paintEvent(QPaintEvent* e)
{
	Q_UNUSED(e);
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	drawRect(&p);
}

void BackgroundRectWidget::drawRect(QPainter* painter)
{
	QRectF r(0, 0, width(), height());

	if (_cfg.colors.empty())
	{
		painter->fillRect(r, QColor(QString::fromStdString("#000000")));
		return;
	}
	else if (_cfg.colors.size() == 1)
	{
		painter->fillRect(r, QColor(QString::fromStdString(_cfg.colors.front().value())));
		return;
	}

	const bool vertical = (_cfg.direction == GradientDirection::vertical);
	QLinearGradient grad(vertical ? r.center().x() : r.left(), vertical ? r.top() : r.center().y(),
	                     vertical ? r.center().x() : r.right(), vertical ? r.bottom() : r.center().y());

	const int n = static_cast<int>(_cfg.colors.size());
	for (int i = 0; i < n; ++i)
	{
		const double pos = static_cast<double>(i) / static_cast<double>(n - 1);
		grad.setColorAt(pos, QColor(QString::fromStdString(_cfg.colors[static_cast<size_t>(i)].value())));
	}

	painter->fillRect(r, grad);
}

#include "background_rect/moc_background_rect.cpp"


