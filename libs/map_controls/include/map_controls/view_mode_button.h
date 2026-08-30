// SPDX-License-Identifier: GPL-3.0-or-later
//
// Top-down or perspective: the button that tilts the map back and forth.
//
// The glyph shows the view a press WOULD GIVE, the way the big map apps'
// 2D/3D buttons do: a trapezoid while the map is flat ("press for
// perspective"), a flat square while it is tilted ("press to look straight
// down"). Showing the current state instead reads as a broken toggle -- the
// picture never matches what pressing it does.
#ifndef MAP_CONTROLS_VIEW_MODE_BUTTON_H
#define MAP_CONTROLS_VIEW_MODE_BUTTON_H

#include "map_controls/map_button.h"

namespace map_controls
{

class ViewModeButton : public MapButton
{
    Q_OBJECT

  public:
    ViewModeButton(const QColor& glyph, const QColor& disc, QWidget* parent = nullptr);

    // Whether the map is CURRENTLY in perspective. Flips the glyph and the
    // tooltip; the click handling belongs to the host.
    void setPerspective(bool perspective);
    bool perspective() const { return mPerspective; }

  protected:
    void paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph) override;

  private:
    bool mPerspective { false };
};

} // namespace map_controls

#endif // MAP_CONTROLS_VIEW_MODE_BUTTON_H
