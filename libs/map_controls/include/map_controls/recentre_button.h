// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Put the camera back where it belongs."
//
// Shown only once the user has dragged the map away from what the layout or
// the vehicle says the centre is. Without it a pan is a one-way door: follow
// mode is suspended for as long as the widget lives, and nothing on screen
// says why the map has stopped tracking.
#ifndef MAP_CONTROLS_RECENTRE_BUTTON_H
#define MAP_CONTROLS_RECENTRE_BUTTON_H

#include "map_controls/map_button.h"

namespace map_controls
{

class RecentreButton : public MapButton
{
    Q_OBJECT

  public:
    RecentreButton(const QColor& glyph, const QColor& disc, QWidget* parent = nullptr);

  protected:
    void paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph) override;
};

} // namespace map_controls

#endif // MAP_CONTROLS_RECENTRE_BUTTON_H
