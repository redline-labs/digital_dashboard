#include "static_text/static_text.h"

#include "qt_helpers/widget_colors.h"
#include <QHBoxLayout>
#include <QFont>

#include <map>
#include <string>

#include <spdlog/spdlog.h>

#include "qt_helpers/widget_fonts.h"

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
    QFont font(qt_helpers::familyForName(QString::fromStdString(_cfg.font)));
    font.setPointSize(static_cast<int>(_cfg.font_size));
    _label->setFont(font);

    _label->setText(QString::fromStdString(_cfg.text));
    _label->setStyleSheet(QString("color: %1;").arg(qt_helpers::colorStyle(_cfg.color)));
    _label->setAlignment(Qt::AlignCenter);
}

#include "static_text/moc_static_text.cpp"


