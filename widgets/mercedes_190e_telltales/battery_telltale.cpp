#include "mercedes_190e_telltales/battery_telltale.h"

#include <QMetaObject>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSvgRenderer>

#include <spdlog/spdlog.h>

// Cap'n Proto includes
#include <capnp/message.h>
#include <capnp/serialize.h>
#include "vehicle_warnings.capnp.h"

#include <memory>

// Define static colors
const QColor Mercedes190EBatteryTelltale::ASSERTED_BACKGROUND = QColor(200, 50, 50);    // Medium red
const QColor Mercedes190EBatteryTelltale::NORMAL_BACKGROUND = QColor(60, 60, 60);       // Dark gray
const QColor Mercedes190EBatteryTelltale::ASSERTED_ICON = QColor(255, 255, 255);        // White when asserted
const QColor Mercedes190EBatteryTelltale::NORMAL_ICON = QColor(120, 120, 120);          // Light gray when normal

Mercedes190EBatteryTelltale::Mercedes190EBatteryTelltale(const Mercedes190EBatteryTelltaleConfig_t& cfg, QWidget *parent)
    : QWidget(parent)
    , _cfg{cfg}
    , mSvgRenderer(nullptr)
    , mAsserted(false)
{
    // Load the SVG renderer
    mSvgRenderer = new QSvgRenderer(QString(":/mercedes_190e_telltales/telltale_battery.svg"), this);
    
    if (!mSvgRenderer->isValid()) {
        SPDLOG_WARN("Failed to load battery telltale SVG");
    }
    
    // Initialize colors
    updateColors();
    
    // Set minimum size
    setMinimumSize(32, 32);
    
    // Enable repainting when needed
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

Mercedes190EBatteryTelltale::~Mercedes190EBatteryTelltale()
{
    // Qt handles cleanup automatically due to parent-child relationships
}

void Mercedes190EBatteryTelltale::setAsserted(bool asserted)
{
    if (mAsserted != asserted) {
        mAsserted = asserted;
        updateColors();
        update(); // Trigger a repaint
        emit assertedChanged(asserted);
    }
}

void Mercedes190EBatteryTelltale::updateColors()
{
    if (mAsserted) {
        mBackgroundColor = QColor::fromString(_cfg.warning_color);
        mIconColor = ASSERTED_ICON;
    } else {
        mBackgroundColor = QColor::fromString(_cfg.normal_color);
        mIconColor = NORMAL_ICON;
    }
}

QSize Mercedes190EBatteryTelltale::sizeHint() const
{
    return QSize(64, 64); // Default size
}

void Mercedes190EBatteryTelltale::paintEvent(QPaintEvent *event)
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

void Mercedes190EBatteryTelltale::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update(); // Ensure the widget repaints with new size
}

void Mercedes190EBatteryTelltale::setZenohSession(std::shared_ptr<zenoh::Session> session)
{
    _zenoh_session = session;
    
    // If we have a zenoh key configured, create the subscription
    if (!_cfg.zenoh_key.empty()) {
        createZenohSubscription();
    }
}

void Mercedes190EBatteryTelltale::createZenohSubscription()
{
    if (!_zenoh_session) {
        SPDLOG_WARN("Mercedes190EBatteryTelltale: Cannot create subscription - no Zenoh session");
        return;
    }
    
    if (_cfg.zenoh_key.empty()) {
        return; // No key configured
    }
    
    try {
        auto key_expr = zenoh::KeyExpr(_cfg.zenoh_key);
        
        _zenoh_subscriber = std::make_unique<zenoh::Subscriber<void>>(
            _zenoh_session->declare_subscriber(
                key_expr,
                [this](const zenoh::Sample& sample) {
                    try {
                        // Get the payload bytes
                        auto bytes = sample.get_payload().as_string();
                        
                        // Deserialize Cap'n Proto message
                        ::capnp::FlatArrayMessageReader message(
                            kj::arrayPtr(reinterpret_cast<const capnp::word*>(bytes.data()),
                                       bytes.size() / sizeof(capnp::word))
                        );
                        
                        BatteryWarning::Reader batteryWarning = message.getRoot<BatteryWarning>();
                        
                        // Extract the warning status
                        bool asserted = batteryWarning.getIsWarningActive();
                        
                        // Use Qt's queued connection to ensure thread safety
                        QMetaObject::invokeMethod(this, "onAssertedDataReceived", 
                                                Qt::QueuedConnection, 
                                                Q_ARG(bool, asserted));
                        
                    } catch (const std::exception& e) {
                        SPDLOG_ERROR("Mercedes190EBatteryTelltale: Error parsing data: {}", e.what());
                    }
                },
                zenoh::closures::none
            )
        );
        
        SPDLOG_INFO("Mercedes190EBatteryTelltale: Created subscription for key '{}'", _cfg.zenoh_key);
        
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Mercedes190EBatteryTelltale: Failed to create subscription for key '{}': {}", 
                     _cfg.zenoh_key, e.what());
    }
}

void Mercedes190EBatteryTelltale::onAssertedDataReceived(bool asserted)
{
    setAsserted(asserted);
}

#include "mercedes_190e_telltales/moc_battery_telltale.cpp"
