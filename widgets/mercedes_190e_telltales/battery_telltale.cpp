#include "mercedes_190e_telltales/battery_telltale.h"
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSvgRenderer>
#include <QDebug>

// Define static colors
const QColor BatteryTelltaleWidget::ASSERTED_BACKGROUND = QColor(200, 50, 50);    // Medium red
const QColor BatteryTelltaleWidget::NORMAL_BACKGROUND = QColor(60, 60, 60);       // Dark gray
const QColor BatteryTelltaleWidget::ASSERTED_ICON = QColor(255, 255, 255);        // White when asserted
const QColor BatteryTelltaleWidget::NORMAL_ICON = QColor(120, 120, 120);          // Light gray when normal

BatteryTelltaleWidget::BatteryTelltaleWidget(QWidget *parent)
    : QWidget(parent)
    , mSvgRenderer(nullptr)
    , mAsserted(false)
{
    // Load the SVG renderer
    mSvgRenderer = new QSvgRenderer(QString(":/mercedes_190e_telltales/telltale_battery.svg"), this);
    
    if (!mSvgRenderer->isValid()) {
        qWarning() << "Failed to load battery telltale SVG";
    }
    
    // Initialize colors
    updateColors();
    
    // Set minimum size
    setMinimumSize(32, 32);
    
    // Enable repainting when needed
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

BatteryTelltaleWidget::~BatteryTelltaleWidget()
{
    // Qt handles cleanup automatically due to parent-child relationships
}

void BatteryTelltaleWidget::setAsserted(bool asserted)
{
    if (mAsserted != asserted) {
        mAsserted = asserted;
        updateColors();
        update(); // Trigger a repaint
        emit assertedChanged(asserted);
    }
}

void BatteryTelltaleWidget::updateColors()
{
    if (mAsserted) {
        mBackgroundColor = ASSERTED_BACKGROUND;
        mIconColor = ASSERTED_ICON;
    } else {
        mBackgroundColor = NORMAL_BACKGROUND;
        mIconColor = NORMAL_ICON;
    }
}

QSize BatteryTelltaleWidget::sizeHint() const
{
    return QSize(64, 64); // Default size
}

void BatteryTelltaleWidget::paintEvent(QPaintEvent *event)
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

void BatteryTelltaleWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    update(); // Ensure the widget repaints with new size
} 

#include "mercedes_190e_telltales/moc_battery_telltale.cpp"
