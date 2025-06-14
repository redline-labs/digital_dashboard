#ifndef WidgetsMainWindow_H
#define WidgetsMainWindow_H

#include <QMainWindow>
#include <QTimer>
#include <QHBoxLayout>
#include <QSlider>
#include <memory>

QT_BEGIN_NAMESPACE
namespace Ui { class WidgetsMainWindow; }
QT_END_NAMESPACE

#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_telltales/battery_telltale.h"

// Zenoh includes
#include "zenoh.hxx"

class WidgetsMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    WidgetsMainWindow(QWidget *parent = nullptr);
    ~WidgetsMainWindow();

private slots:
    void updateTachometer();
    void updateSpeedometer();
    void updateSparkline(int value);
    void onRpmChanged(int value);
    void onBatteryTelltaleToggled(bool checked);
    void onSpeedDataReceived(double speedMps);

private:
    // Private methods
    void initializeZenoh();
    
    Ui::WidgetsMainWindow *ui;
    TachometerWidget *mTachometer;
    SpeedometerWidgetMPH *mSpeedometer;
    SparklineItem *mSparkline;
    BatteryTelltaleWidget *mBatteryTelltale;
    QSlider *mRpmSlider;
    QSlider *mSparklineSlider;
    QWidget *mCentralWidget;
    QHBoxLayout *mMainLayout;
    QTimer *mTimerRPM;
    QTimer *mTimerMPH;
    
    // Zenoh-related members
    std::unique_ptr<zenoh::Session> mZenohSession;
    std::unique_ptr<zenoh::Subscriber<void>> mSpeedSubscriber;
};
#endif // WidgetsMainWindow_H