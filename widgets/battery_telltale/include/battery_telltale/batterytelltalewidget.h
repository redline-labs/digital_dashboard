#ifndef BATTERYTELLTALEWIDGET_H
#define BATTERYTELLTALEWIDGET_H

#include <QWidget>
#include <QPainter>
#include <QSvgRenderer>
#include <QTimer>

class BatteryTelltaleWidget : public QWidget
{
    Q_OBJECT
    Q_PROPERTY(bool asserted READ isAsserted WRITE setAsserted NOTIFY assertedChanged)

public:
    explicit BatteryTelltaleWidget(QWidget *parent = nullptr);
    ~BatteryTelltaleWidget();

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