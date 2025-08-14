#include "static_text/static_text.h"

#include <QHBoxLayout>
#include <QFont>
#include <QFontDatabase>

#include <spdlog/spdlog.h>

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
    // Try to match a resource font if provided name looks like known resources
    // Caller should pass the family name; we also try to load common fonts from resources.
    // Attempt to load DSEG7 if specified
    if (_cfg.font == "DSEG7 Classic")
    {
        int id = QFontDatabase::addApplicationFont(":/fonts/DSEG7Classic-Bold.ttf");
        if (id == -1) {
            SPDLOG_WARN("StaticTextWidget: failed to load DSEG7Classic-Bold.ttf, using system font");
        }
    }
    else if (_cfg.font == "DSEG7 Classic Mini")
    {
        int id = QFontDatabase::addApplicationFont(":/fonts/DSEG7ClassicMini-Bold.ttf");
        if (id == -1) {
            SPDLOG_WARN("StaticTextWidget: failed to load DSEG7ClassicMini-Bold.ttf, using system font");
        }
    }
    else if (_cfg.font == "DSEG14 Classic")
    {
        int id = QFontDatabase::addApplicationFont(":/fonts/DSEG14Classic-Regular.ttf");
        if (id == -1) {
            SPDLOG_WARN("StaticTextWidget: failed to load DSEG14Classic-Regular.ttf, using system font");
        }
    }
    else if (_cfg.font == "futura" || _cfg.font == "Futura" || _cfg.font == "futura.ttf")
    {
        int id = QFontDatabase::addApplicationFont(":/fonts/futura.ttf");
        if (id == -1) {
            SPDLOG_WARN("StaticTextWidget: failed to load futura.ttf, using system font");
        }
    }

    QFont font(QString::fromStdString(_cfg.font));
    font.setPointSize(static_cast<int>(_cfg.font_size));
    _label->setFont(font);

    _label->setText(QString::fromStdString(_cfg.text));
    _label->setStyleSheet(QString("color: %1;").arg(QString::fromStdString(_cfg.color)));
    _label->setAlignment(Qt::AlignCenter);
}

#include "static_text/moc_static_text.cpp"


