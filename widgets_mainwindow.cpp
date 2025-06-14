#include "widgets_mainwindow.h"
#include <QLabel> // For a label if needed, not strictly for slider now
#include <QHBoxLayout> // Ensure this is included for QHBoxLayout
#include <QVBoxLayout> // For controls layout if needed
#include <QTimer> // For QTimer
#include <QCheckBox> // For battery telltale toggle
#include <QMetaObject> // For Qt::QueuedConnection
#include <spdlog/spdlog.h> // For logging

WidgetsMainWindow::WidgetsMainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    mCentralWidget = new QWidget(this);
    setCentralWidget(mCentralWidget);

    mMainLayout = new QHBoxLayout(mCentralWidget);

    // --- Speedometer Section (Left) ---
    QWidget *speedoContainer = new QWidget();
    QVBoxLayout *speedoLayout = new QVBoxLayout(speedoContainer);
    
    mSpeedometer = new SpeedometerWidgetMPH(this);
    speedoLayout->addWidget(mSpeedometer);

    QLabel *speedLabel = new QLabel("MPH");
    speedoLayout->addWidget(speedLabel);
    mMainLayout->addWidget(speedoContainer, 1);

    // --- Tachometer Section (Middle) ---
    QWidget *tachoContainer = new QWidget();
    QVBoxLayout *tachoLayout = new QVBoxLayout(tachoContainer);

    mTachometer = new TachometerWidget(this);
    tachoLayout->addWidget(mTachometer);

    QLabel *rpmLabel = new QLabel("RPM");
    tachoLayout->addWidget(rpmLabel);
    mRpmSlider = new QSlider(Qt::Horizontal, this);
    mRpmSlider->setRange(0, 7000); // 0 to 7000 RPM
    mRpmSlider->setValue(0);
    tachoLayout->addWidget(mRpmSlider);
    connect(mRpmSlider, &QSlider::valueChanged, this, &WidgetsMainWindow::onRpmChanged);
    mMainLayout->addWidget(tachoContainer, 1);

    // --- Sparkline Section (Right) ---
    QWidget *sparklineContainer = new QWidget();
    QVBoxLayout *sparklineVLayout = new QVBoxLayout(sparklineContainer);

    mSparkline = new SparklineItem("Signal", this); // Provide units
    mSparkline->setFixedSize(250, 80); // Example size, adjust as needed
    sparklineVLayout->addWidget(mSparkline);

    QLabel *sparklineLabel = new QLabel("Input Signal");
    sparklineVLayout->addWidget(sparklineLabel);
    mSparklineSlider = new QSlider(Qt::Horizontal, this);
    mSparklineSlider->setRange(0, 100);
    mSparklineSlider->setValue(50);
    sparklineVLayout->addWidget(mSparklineSlider);
    connect(mSparklineSlider, &QSlider::valueChanged, this, &WidgetsMainWindow::updateSparkline);
    
    // --- Battery Telltale Section ---
    mBatteryTelltale = new BatteryTelltaleWidget(this);
    mBatteryTelltale->setFixedSize(64, 64);
    sparklineVLayout->addWidget(mBatteryTelltale);
    
    QCheckBox *batteryCheckBox = new QCheckBox("Battery Warning");
    batteryCheckBox->setChecked(false);
    sparklineVLayout->addWidget(batteryCheckBox);
    connect(batteryCheckBox, &QCheckBox::toggled, this, &WidgetsMainWindow::onBatteryTelltaleToggled);
    
    mMainLayout->addWidget(sparklineContainer, 1);

    // Initialize sparkline with the slider's current value
    updateSparkline(mSparklineSlider->value());
    // Initialize other widgets too
    onRpmChanged(mRpmSlider->value());
    
    // Initialize Zenoh session and subscriber
    initializeZenoh();
    
    setWindowTitle("Mercedes Instrument Cluster");
    resize(1000, 400); // Adjusted size for two gauges
}

WidgetsMainWindow::~WidgetsMainWindow()
{
    // Qt handles child widget deletion automatically
}



void WidgetsMainWindow::onRpmChanged(int value)
{
    if (mTachometer)
    {
        mTachometer->setRpm(static_cast<float>(value));
    }
}

void WidgetsMainWindow::updateSparkline(int value)
{
    if (mSparkline) {
        mSparkline->addDataPoint(static_cast<double>(value));
    }
}

void WidgetsMainWindow::onBatteryTelltaleToggled(bool checked)
{
    if (mBatteryTelltale) {
        mBatteryTelltale->setAsserted(checked);
    }
}

void WidgetsMainWindow::updateTachometer()
{
    // Example: int newRpm = mRpmSlider->value() + (qrand() % 200 - 100);
    // if (mTachometer) mTachometer->setRpm(newRpm > 0 ? newRpm : 0);
}

void WidgetsMainWindow::updateSpeedometer()
{
    // Example: int newSpeed = mSpeedSlider->value() + (qrand() % 10 - 5);
    // if (mSpeedometer) mSpeedometer->setSpeed(newSpeed > 0 ? newSpeed : 0);
}

void WidgetsMainWindow::initializeZenoh()
{
    try {
        // Create Zenoh configuration
        auto config = zenoh::Config::create_default();
        
        // Open Zenoh session
        mZenohSession = std::make_unique<zenoh::Session>(zenoh::Session::open(std::move(config)));
        SPDLOG_INFO("Zenoh session opened successfully");
        
        // Create subscriber for vehicle speed
        auto key_expr = zenoh::KeyExpr("vehicle/speed_mps");
        
        mSpeedSubscriber = std::make_unique<zenoh::Subscriber<void>>(
            mZenohSession->declare_subscriber(
                key_expr,
                [this](const zenoh::Sample& sample) {
                    try {
                        // Convert payload to string and then to double
                        const auto& payload = sample.get_payload();
                        std::string speed_str = payload.as_string();
                        double speed_mps = std::stod(speed_str);
                        
                        // Call slot via Qt's queued connection to ensure thread safety
                        QMetaObject::invokeMethod(this, "onSpeedDataReceived", 
                                                Qt::QueuedConnection, 
                                                Q_ARG(double, speed_mps));

                    } catch (const std::exception& e) {
                        SPDLOG_ERROR("Error processing speed data: {}", e.what());
                    }
                },
                zenoh::closures::none
            )
        );
        
        spdlog::info("Zenoh speed subscriber created for key: vehicle/speed_mps");
        
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Failed to initialize Zenoh: {}", e.what());
        // Continue without Zenoh - the application can still function with manual sliders
    }
}

void WidgetsMainWindow::onSpeedDataReceived(double speedMps)
{
    // Convert m/s to mph (1 m/s = 2.23694 mph)
    double speedMph = speedMps * 2.23694;
    
    // Update the speedometer widget
    if (mSpeedometer) {
        mSpeedometer->setSpeed(static_cast<float>(speedMph));
    }
}