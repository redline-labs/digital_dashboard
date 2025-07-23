#ifndef BATTERYTELLTALEWIDGET_H
#define BATTERYTELLTALEWIDGET_H

#include <mercedes_190e_telltales/config.h>

#include <QWidget>
#include <QPainter>
#include <QTimer>

// Forward declaration.
class QSvgRenderer;


class Mercedes190EBatteryTelltale : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(bool asserted READ isAsserted WRITE setAsserted NOTIFY assertedChanged)

public:
    explicit Mercedes190EBatteryTelltale(const battery_telltale_config_t& cfg, QWidget *parent = nullptr);
    ~Mercedes190EBatteryTelltale();

    bool isAsserted() const { return mAsserted; }
    void setAsserted(bool asserted);

    QSize sizeHint() const override;

signals:
    void assertedChanged(bool asserted);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void updateColors();

    battery_telltale_config_t _cfg;

    QSvgRenderer *mSvgRenderer;
    bool mAsserted;
    QColor mBackgroundColor;
    QColor mIconColor;
    
    // Colors
    static const QColor ASSERTED_BACKGROUND;
    static const QColor NORMAL_BACKGROUND;
    static const QColor ASSERTED_ICON;
    static const QColor NORMAL_ICON;
};

#endif // BATTERYTELLTALEWIDGET_H 