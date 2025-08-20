#ifndef BACKGROUND_RECT_WIDGET_H
#define BACKGROUND_RECT_WIDGET_H

#include "background_rect/config.h"

#include <QWidget>
#include <string_view>

class QPainter;

class BackgroundRectWidget : public QWidget
{
	Q_OBJECT

public:
	using config_t = BackgroundRectConfig_t;
	static constexpr std::string_view kWidgetName = "background_rect";

	explicit BackgroundRectWidget(const BackgroundRectConfig_t& cfg, QWidget* parent = nullptr);

protected:
	void paintEvent(QPaintEvent* event) override;

private:
	void drawRect(QPainter* painter);

	BackgroundRectConfig_t _cfg;
};

#endif // BACKGROUND_RECT_WIDGET_H
