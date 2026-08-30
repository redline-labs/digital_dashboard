// SPDX-License-Identifier: GPL-3.0-or-later
//
// The compass: a needle that always points at TRUE NORTH ON SCREEN, and the
// button that cycles the orientation mode.
//
// The needle tracks the live camera bearing -- feed it from wherever the
// camera is assembled -- so in heading-up mode it swings with the vehicle,
// which is what tells the driver the map is turning. When the map is
// effectively north-up and the needle points straight up it de-emphasises
// rather than hiding, the way the big map apps' compasses fade: unlike them
// there is no rotate gesture here, so the button is the only door into
// heading-up and must stay pressable.
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

  protected:
    void paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph) override;

  private:
    double mBearing { 0.0 };
};

} // namespace map_controls

#endif // MAP_CONTROLS_COMPASS_BUTTON_H
