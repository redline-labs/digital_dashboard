#include "mercedes_190e_telltales/battery_telltale.h"

#include <QMetaObject>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QSvgRenderer>

#include <spdlog/spdlog.h>

// Cap'n Proto includes
#include <capnp/message.h>
#include <capnp/serialize.h>

// ExprTk includes
#include "exprtk.hpp"

#include <memory>

Mercedes190EBatteryTelltale::Mercedes190EBatteryTelltale(const Mercedes190EBatteryTelltaleConfig_t& cfg, QWidget *parent)
    : QWidget(parent)
    , _cfg{cfg}
    , mSvgRenderer(nullptr)
    , mAsserted(false)
    , _expression(nullptr)
{
    // Load the SVG renderer
    mSvgRenderer = new QSvgRenderer(QString(":/mercedes_190e_telltales/telltale_battery.svg"), this);
    
    if (!mSvgRenderer->isValid()) {
        SPDLOG_WARN("Failed to load battery telltale SVG");
    }
    
    // Initialize data extractor
    _data_extractor.setSchemaType(_cfg.schema_type);
    
    // Initialize expression
    initializeExpression();
    
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
    if (mAsserted != asserted)
    {
        mAsserted = asserted;
        updateColors();
        update(); // Trigger a repaint
    }
}

void Mercedes190EBatteryTelltale::updateColors()
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
                        
                        // Extract variables using the generic extractor
                        auto variables = _data_extractor.extractVariables(bytes);
                        
                        // Use Qt's queued connection to ensure thread safety
                        QMetaObject::invokeMethod(this, "onDataReceived", 
                                                Qt::QueuedConnection, 
                                                Q_ARG(CapnpDataExtractor::VariableMap, variables));
                        
                    }
                    catch (const std::exception& e)
                    {
                        SPDLOG_ERROR("Error parsing data: {}", e.what());
                    }
                },
                zenoh::closures::none
            )
        );
        
        SPDLOG_INFO("Created subscription for key '{}' with schema type '{}'", 
                    _cfg.zenoh_key, _cfg.schema_type);
        
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to create subscription for key '{}': {}", 
                     _cfg.zenoh_key, e.what());
    }
}

void Mercedes190EBatteryTelltale::onDataReceived(const CapnpDataExtractor::VariableMap& variables)
{
    // Update all variables
    for (const auto& [name, value] : variables)
    {
        _variables[name] = value;
    }
    
    // Evaluate the condition and update telltale state
    bool shouldAssert = evaluateCondition();
    setAsserted(shouldAssert);
}

void Mercedes190EBatteryTelltale::initializeExpression()
{
    try
    {
        // Get available variables from the schema
        auto available_vars = _data_extractor.getAvailableVariables();

        // Log available variables
        if (!available_vars.empty())
        {
            std::string var_list;
            for (const auto& var : available_vars)
            {
                if (!var_list.empty()) var_list += ", ";
                var_list += var;
            }
            SPDLOG_INFO("Available variables for schema '{}': {}", _cfg.schema_type, var_list);
        }
        
        // Initialize variables with default values
        for (const auto& var_name : available_vars)
        {
            _variables[var_name] = 0.0; // Default value
        }
        
        // Create symbol table and add variables by reference
        _symbol_table = std::make_unique<exprtk::symbol_table<double>>();
        for (auto& [name, value] : _variables)
        {
            _symbol_table->add_variable(name, value);
        }
        
        // Create and compile expression
        _expression = std::make_unique<exprtk::expression<double>>();
        _expression->register_symbol_table(*_symbol_table);
        
        exprtk::parser<double> parser;
        if (!parser.compile(_cfg.condition_expression, *_expression))
        {
            SPDLOG_ERROR("Mercedes190EBatteryTelltale: Failed to compile expression '{}': {}", 
                         _cfg.condition_expression, parser.error());
            _expression.reset(); // Clear invalid expression
            _symbol_table.reset();
        }
        else
        {
            SPDLOG_INFO("Mercedes190EBatteryTelltale: Successfully compiled expression: '{}'", 
                        _cfg.condition_expression);
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Mercedes190EBatteryTelltale: Exception initializing expression: {}", e.what());
        _expression.reset();
        _symbol_table.reset();
    }
}

bool Mercedes190EBatteryTelltale::evaluateCondition()
{
    if (!_expression)
    {
        return true; // No data, default to warning.
    }
    
    try
    {
        // Variables are already linked by reference, so just evaluate
        double result = _expression->value();
        return result != 0.0;
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("Mercedes190EBatteryTelltale: Exception evaluating expression: {}", e.what());
        return true; // No data, default to warning.
    }
}


#include "mercedes_190e_telltales/moc_battery_telltale.cpp"
