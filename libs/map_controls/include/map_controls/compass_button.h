// SPDX-License-Identifier: GPL-3.0-or-later
//
// The compass: a needle that always points at TRUE NORTH ON SCREEN, the
// button that cycles the orientation mode -- and the map's ROTATION CONTROL.
//
// The needle tracks the live camera bearing -- feed it from wherever the
// camera is assembled -- so in heading-up mode it swings with the vehicle,
// which is what tells the driver the map is turning. When the map is
// effectively north-up and the needle points straight up it de-emphasises
// rather than hiding: the button is the only door into heading-up and must
// stay pressable.
//
// Grabbing the needle and dragging spins the map: the bearing follows the
// cursor's angle about the button centre, so the needle stays under the
// finger. Confining rotation to the button is what keeps it from ever
// fighting the pan gesture, and it works the same for a mouse and a touch.
// A press that never moves past the drag threshold is still a plain click.
#ifndef MAP_CONTROLS_COMPASS_BUTTON_H
#define MAP_CONTROLS_COMPASS_BUTTON_H

#include "map_controls/map_button.h"

namespace map_controls
{

class CompassButton : public MapButton
{
    Q_OBJECT

  public:
    CompassButton(const QColor& glyph, const QColor& disc, QWidget* parent = nullptr);

    // The camera's current bearing in degrees clockwise from north. The
    // needle draws at MINUS this, which is where north actually is on screen.
    void setBearing(double degrees);
    double bearing() const { return mBearing; }

  signals:
    // The needle is being dragged: the map should turn to this bearing, NOW
    // -- emitted per move, in degrees clockwise from north, normalised to
    // [0, 360). No signal fires on release; the last value stands.
    void bearingDragged(double degrees);

  protected:
    void paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

  private:
    double mBearing { 0.0 };
    QPointF mPressPos;
    bool mDragging { false };
};

} // namespace map_controls

#endif // MAP_CONTROLS_COMPASS_BUTTON_H
