// SPDX-License-Identifier: GPL-3.0-or-later
//
// One step of zoom, in or out. A pair of these sits in the corner stack; the
// glyph is a plus or a minus and the button auto-repeats, so holding it zooms
// continuously the way holding a wheel notch would if wheels worked that way.
// The zoom itself belongs to the host -- these emit clicks and nothing else,
// so the widget's and the panel's different zoom semantics stay their own.
#ifndef MAP_CONTROLS_ZOOM_BUTTON_H
#define MAP_CONTROLS_ZOOM_BUTTON_H

#include "map_controls/map_button.h"

namespace map_controls
{

class ZoomButton : public MapButton
{
    Q_OBJECT

  public:
    enum class Direction
    {
        In,
        Out
    };

    ZoomButton(Direction direction, const QColor& glyph, const QColor& disc,
               QWidget* parent = nullptr);

  protected:
    void paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph) override;

  private:
    Direction mDirection;
};

} // namespace map_controls

#endif // MAP_CONTROLS_ZOOM_BUTTON_H
