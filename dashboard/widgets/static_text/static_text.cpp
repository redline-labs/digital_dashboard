#include "static_text/static_text.h"

#include <QHBoxLayout>
#include <QFont>

#include <map>
#include <string>

#include <spdlog/spdlog.h>

#include "dashboard/widget_fonts.h"

StaticTextWidget::StaticTextWidget(const StaticTextConfig_t& cfg, QWidget* parent)
    : QWidget(parent), _cfg{cfg}, _label{new QLabel(this)}
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0,0,0,0);
    layout->addWidget(_label);
    setLayout(layout);

    applyConfig();
}

void StaticTextWidget::applyConfig()
{
    // Known bundled fonts are loaded from resources on demand; any other name
    // falls through to the system font lookup below.
    static const std::map<std::string, const char*> kResourceFonts = {
        {"DSEG7 Classic", ":/fonts/DSEG7Classic-Bold.ttf"},
        {"DSEG7 Classic Mini", ":/fonts/DSEG7ClassicMini-Bold.ttf"},
        {"DSEG14 Classic", ":/fonts/DSEG14Classic-Regular.ttf"},
        {"futura", ":/fonts/futura.ttf"},
        {"Futura", ":/fonts/futura.ttf"},
        {"futura.ttf", ":/fonts/futura.ttf"},
    };
    if (auto it = kResourceFonts.find(_cfg.font); it != kResourceFonts.end())
    {
        dashboard::loadResourceFont(it->second);
    }

    QFont font(QString::fromStdString(_cfg.font));
    font.setPointSize(static_cast<int>(_cfg.font_size));
    _label->setFont(font);

    _label->setText(QString::fromStdString(_cfg.text));
    _label->setStyleSheet(QString("color: %1;").arg(QString::fromStdString(_cfg.color.value())));
    _label->setAlignment(Qt::AlignCenter);
}

#include "static_text/moc_static_text.cpp"


