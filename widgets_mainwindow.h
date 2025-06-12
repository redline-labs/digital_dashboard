#ifndef WidgetsMainWindow_H
#define WidgetsMainWindow_H

#include <QMainWindow>
#include <QTimer>
#include <QHBoxLayout>
#include <QSlider>

QT_BEGIN_NAMESPACE
namespace Ui { class WidgetsMainWindow; }
QT_END_NAMESPACE

#include "mercedes_190e_tachometer/mercedes_190e_tachometer.h"
#include "mercedes_190e_speedometer/mercedes_190e_speedometer.h"
#include "sparkline/sparkline.h"
#include "mercedes_190e_telltales/battery_telltale.h"

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
    void onSpeedChanged(int value);
    void onRpmChanged(int value);
    void onBatteryTelltaleToggled(bool checked);

private:
    Ui::WidgetsMainWindow *ui;
    TachometerWidget *mTachometer;
    SpeedometerWidgetMPH *mSpeedometer;
    SparklineItem *mSparkline;
    BatteryTelltaleWidget *mBatteryTelltale;
    QSlider *mRpmSlider;
    QSlider *mSpeedSlider;
    QSlider *mSparklineSlider;
    QWidget *mCentralWidget;
    QHBoxLayout *mMainLayout;
    QTimer *mTimerRPM;
    QTimer *mTimerMPH;
};
#endif // WidgetsMainWindow_H