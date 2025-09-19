#include "mercedes_190e_telltales/telltale.h"

#include <QMetaObject>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSvgRenderer>

#include <spdlog/spdlog.h>

// Expression parser
#include "pub_sub/zenoh_subscriber.h"

// Colors
static constexpr QColor kAssertedIcon = QColor(255, 255, 255);        // White when asserted
static constexpr QColor kNormalIcon = QColor(120, 120, 120);          // Light gray when normal

Mercedes190ETelltale::Mercedes190ETelltale(const Mercedes190ETelltaleConfig_t& cfg, QWidget *parent)
    : QWidget(parent)
    , _cfg{cfg}
    , mSvgRenderer(nullptr)
    , mAsserted(false)
{
    // Initialize expression parser
    try {
        _expression_parser = std::make_unique<pub_sub::ZenohExpressionSubscriber>(
            _cfg.schema_type,
            _cfg.condition_expression,
            _cfg.zenoh_key
        );
        
        if (!_expression_parser->isValid())
        {
            SPDLOG_ERROR("Invalid expression '{}' for schema '{}' in telltale", 
                        _cfg.condition_expression, reflection::enum_traits<pub_sub::schema_type_t>::to_string(_cfg.schema_type));
            _expression_parser.reset(); // Disable expression parsing
        }
        else
        {
            SPDLOG_INFO("Telltale initialized with expression: '{}' (schema: '{}')", 
                       _cfg.condition_expression, reflection::enum_traits<pub_sub::schema_type_t>::to_string(_cfg.schema_type));
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Failed to initialize expression parser for telltale: {}", e.what());
        _expression_parser.reset(); // Disable expression parsing
    }

    if (_expression_parser)
    {
        _expression_parser->setResultCallback<bool>([this](bool asserted)
        {
            QMetaObject::invokeMethod(this, "onConditionEvaluated", Qt::QueuedConnection, Q_ARG(bool, asserted));
        });
    }
    
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
        update(); // Trigger a repaint
    }
}

void Mercedes190ETelltale::updateColors()
{
    if (mAsserted)
    {
        mBackgroundColor = QColor::fromString(_cfg.warning_color);
        mIconColor = kAssertedIcon;
    }
    else
    {
        mBackgroundColor = QColor::fromString(_cfg.normal_color);
        mIconColor = kNormalIcon;
    }
}

QSize Mercedes190ETelltale::sizeHint() const
{
    return QSize(64, 64); // Default size
}

void Mercedes190ETelltale::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)
    
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    
    QRect widgetRect = rect();
    
    // Draw background rectangle with rounded corners
    painter.fillRect(widgetRect, mBackgroundColor);
    
    // Optionally add a subtle border
    painter.setPen(QPen(QColor(40, 40, 40), 1));
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

void Mercedes190ETelltale::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update(); // Ensure the widget repaints with new size
}

// Direct widget subscription removed; handled by zenoh_subscriber

void Mercedes190ETelltale::onConditionEvaluated(bool asserted)
{
    setAsserted(asserted);
}


// MOC include generated by Qt's automoc
#include "mercedes_190e_telltales/moc_telltale.cpp"


