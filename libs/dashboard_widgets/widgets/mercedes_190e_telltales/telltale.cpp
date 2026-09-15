#include "mercedes_190e_telltales/telltale.h"

#include <QPaintEvent>
#include <QSvgRenderer>

#include <spdlog/spdlog.h>

#include "dashboard/expression_subscription.h"
#include "dashboard/gauge_painting.h"
#include "qt_helpers/widget_colors.h"

#include <chrono>

// Colors
static constexpr QColor kAssertedIcon = QColor(255, 255, 255);        // White when asserted
static constexpr QColor kNormalIcon = QColor(120, 120, 120);          // Light gray when normal

Mercedes190ETelltale::Mercedes190ETelltale(const Mercedes190ETelltaleConfig_t& cfg, QWidget *parent)
    : qt_helpers::CachedPaintWidget(parent)
    , _cfg{cfg}
    , mSvgRenderer(nullptr)
    , mAsserted(false)
{
    _expression_parser = dashboard::makeExpressionSubscription<bool>(
        _cfg.schema_type, _cfg.condition_expression, _cfg.zenoh_key,
        this, &Mercedes190ETelltale::setAsserted, &Mercedes190ETelltale::setConditionStale,
        std::chrono::milliseconds(_cfg.stale_after_ms), "telltale condition");


    // Select SVG alias based on telltale type
    switch (_cfg.telltale_type)
    {
        case Mercedes190ETelltaleType::battery:
            mSvgAlias = ":/mercedes_190e_telltales/telltale_battery.svg";
            break;
        case Mercedes190ETelltaleType::brake_system:
            mSvgAlias = ":/mercedes_190e_telltales/telltale_brake_system.svg";
            break;
        case Mercedes190ETelltaleType::high_beam:
            mSvgAlias = ":/mercedes_190e_telltales/telltale_high_beam.svg";
            break;
        case Mercedes190ETelltaleType::windshield_washer:
            mSvgAlias = ":/mercedes_190e_telltales/telltale_windshield_washer.svg";
            break;
        default:
            mSvgAlias = ":/mercedes_190e_telltales/telltale_battery.svg";
            break;
    }

    // Load the SVG renderer
    mSvgRenderer = new QSvgRenderer(QString(mSvgAlias), this);
    
    if (!mSvgRenderer->isValid()) {
        SPDLOG_WARN("Failed to load telltale SVG: {}", mSvgAlias.toStdString());
    }

    // Initialize colors
    updateColors();
    
    // Set minimum size
    setMinimumSize(32, 32);
    
    // Enable repainting when needed
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

Mercedes190ETelltale::~Mercedes190ETelltale()
{
    // Qt handles cleanup automatically due to parent-child relationships
}

void Mercedes190ETelltale::setAsserted(bool asserted)
{
    if (mAsserted != asserted)
    {
        mAsserted = asserted;
        updateColors();
        invalidateStaticCache(); // Colors are baked into the cached layer
        update(); // Trigger a repaint
    }
}

void Mercedes190ETelltale::setConditionStale(bool stale)
{
    if (mStale == stale)
    {
        return;
    }
    mStale = stale;
    dashboard::publishStaleProperties(*this, *this);
    updateColors();
    invalidateStaticCache();  // Colours are baked into the cached layer
    update();
}

void Mercedes190ETelltale::setBindingStale(std::string_view binding, bool stale)
{
    if (binding == "condition")
    {
        setConditionStale(stale);
    }
}

bool Mercedes190ETelltale::isBindingStale(std::string_view binding) const
{
    return binding == "condition" && mStale;
}

void Mercedes190ETelltale::updateColors()
{
    if (mStale)
    {
        // Neither the warning colour nor the off colour: the lamp is showing
        // that it has nothing to show.
        mBackgroundColor = qt_helpers::toQColor(_cfg.normal_color).darker(150);
        mIconColor = gauge_paint::kStaleColor;
        return;
    }

    if (mAsserted)
    {
        mBackgroundColor = qt_helpers::toQColor(_cfg.warning_color);
        mIconColor = kAssertedIcon;
    }
    else
    {
        mBackgroundColor = qt_helpers::toQColor(_cfg.normal_color);
        mIconColor = kNormalIcon;
    }
}

QSize Mercedes190ETelltale::sizeHint() const
{
    return QSize(64, 64); // Default size
}

void Mercedes190ETelltale::paintDynamic(QPainter& /*painter*/)
{
    // Everything is rendered in the cached static underlay.
}

void Mercedes190ETelltale::paintStaticUnderlay(QPainter& painter)
{
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QRect widgetRect = rect();
    
    // Draw background rectangle with rounded corners
    painter.fillRect(widgetRect, mBackgroundColor);
    
    // Optionally add a subtle border. While the condition is stale it is
    // dashed, so "no data" reads differently from "off" even in a photograph.
    QPen border(mStale ? gauge_paint::kStaleColor : QColor(40, 40, 40), mStale ? 2 : 1);
    border.setStyle(mStale ? Qt::DashLine : Qt::SolidLine);
    painter.setPen(border);
    painter.drawRect(widgetRect.adjusted(0, 0, -1, -1));
    
    // Calculate icon size and position (centered, with some margin)
    int margin = qMax(4, qMin(widgetRect.width(), widgetRect.height()) / 8);
    QRect iconRect = widgetRect.adjusted(margin, margin, -margin, -margin);
    
    // Make sure icon maintains aspect ratio
    if (mSvgRenderer && mSvgRenderer->isValid()) {
        QSize svgSize = mSvgRenderer->defaultSize();
        if (svgSize.isValid()) {
            // Scale to fit while maintaining aspect ratio
            QSize scaledSize = svgSize.scaled(iconRect.size(), Qt::KeepAspectRatio);
            
            // Center the icon
            int x = iconRect.x() + (iconRect.width() - scaledSize.width()) / 2;
            int y = iconRect.y() + (iconRect.height() - scaledSize.height()) / 2;
            QRect finalIconRect(x, y, scaledSize.width(), scaledSize.height());
            
            // Create a colored version of the SVG
            // Since QSvgRenderer doesn't directly support color changes, 
            // we'll render to a pixmap and then colorize it
            QPixmap svgPixmap(finalIconRect.size());
            svgPixmap.fill(Qt::transparent);
            
            QPainter svgPainter(&svgPixmap);
            svgPainter.setRenderHint(QPainter::Antialiasing, true);
            mSvgRenderer->render(&svgPainter);
            svgPainter.end();
            
            // Apply color tint
            QPainter tintPainter(&svgPixmap);
            tintPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tintPainter.fillRect(svgPixmap.rect(), mIconColor);
            tintPainter.end();
            
            // Draw the colored icon
            painter.drawPixmap(finalIconRect, svgPixmap);
        }
    }
}

// MOC include generated by Qt's automoc
#include "mercedes_190e_telltales/moc_telltale.cpp"


