#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QTimer>
#include <QHBoxLayout>
#include <QSlider>

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

#include "tachometerwidget.h"
#include "speedometerwidgetmph.h"
#include "sparklineitem.h"
#include "batterytelltalewidget.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void updateTachometer();
    void updateSpeedometer();
    void updateSparkline(int value);
    void onSpeedChanged(int value);
    void onRpmChanged(int value);
    void onBatteryTelltaleToggled(bool checked);

private:
    Ui::MainWindow *ui;
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
#endif // MAINWINDOW_H