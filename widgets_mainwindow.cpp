#include "widgets_mainwindow.h"
#include <QLabel> // For a label if needed, not strictly for slider now
#include <QHBoxLayout> // Ensure this is included for QHBoxLayout
#include <QVBoxLayout> // For controls layout if needed
#include <QTimer> // For QTimer
#include <QCheckBox> // For battery telltale toggle

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
    mSpeedSlider = new QSlider(Qt::Horizontal, this);
    mSpeedSlider->setRange(0, 1250); // 0 to 125.0 MPH
    mSpeedSlider->setValue(0);
    speedoLayout->addWidget(mSpeedSlider);
    connect(mSpeedSlider, &QSlider::valueChanged, this, &WidgetsMainWindow::onSpeedChanged);
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
    onSpeedChanged(mSpeedSlider->value());
    onRpmChanged(mRpmSlider->value());
    
    setWindowTitle("Mercedes Instrument Cluster");
    resize(1000, 400); // Adjusted size for two gauges
}

WidgetsMainWindow::~WidgetsMainWindow()
{
    // Qt handles child widget deletion automatically
}

void WidgetsMainWindow::onSpeedChanged(int value)
{
    if (mSpeedometer)
        mSpeedometer->setSpeed(value / 10.0f);
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