#ifndef STATIC_TEXT_WIDGET_H
#define STATIC_TEXT_WIDGET_H

#include "static_text/config.h"

#include <QWidget>
#include <QLabel>

#include <string_view>

class StaticTextWidget : public QWidget {
    Q_OBJECT

public:
    using config_t = StaticTextConfig_t;
    static constexpr std::string_view kWidgetName = "static_text";

    explicit StaticTextWidget(const StaticTextConfig_t& cfg, QWidget* parent = nullptr);

private:
    void applyConfig();

    StaticTextConfig_t _cfg;
    QLabel* _label;
};

#endif // STATIC_TEXT_WIDGET_H


