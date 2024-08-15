#ifndef CARPLAY_WIDGET_H_
#define CARPLAY_WIDGET_H_

#include "touch_action.h"

#include <QLabel>
#include <QMouseEvent>

class CarPlayWidget : public QLabel
{
    Q_OBJECT

  public:
    CarPlayWidget();
    ~CarPlayWidget();

    void setSize(uint32_t width_px, uint32_t height_px);


  public slots:
    void phone_connected(bool is_connected);

  signals:
    void touchEvent(TouchAction action, uint32_t x, uint32_t y);

  private:
    void mousePressEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
};


#endif  // CARPLAY_WIDGET_H_