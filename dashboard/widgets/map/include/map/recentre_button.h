// SPDX-License-Identifier: GPL-3.0-or-later
//
// "Put the camera back where it belongs."
//
// Shown only once the user has dragged the map away from what the layout or the
// vehicle says the centre is. Without it a pan is a one-way door: Follow
// Vehicle is suspended for as long as the widget lives, and nothing on screen
// says why the map has stopped tracking.
//
// A REAL QAbstractButton and not a rectangle painted into MapWidget::paintEvent,
// for three reasons that are each small and together decisive: hover and press
// states come for free, the editor already makes child widgets mouse-transparent
// in edit mode (Canvas::setEditorMode) so it cannot swallow a selection drag,
// and it appears in ui_snapshot as an addressable widget rather than as a
// coordinate the agent interface has to be told about.
//
// It paints itself rather than carrying an icon: there is no icon pipeline in
// this tree, a QIcon would need one per state per theme, and the glyph is four
// ticks and two circles.
#ifndef MAP_RECENTRE_BUTTON_H
#define MAP_RECENTRE_BUTTON_H

#include <QAbstractButton>
#include <QColor>

namespace map_widget
{

class RecentreButton : public QAbstractButton
{
  public:
    // Takes the LABEL colours from the style rather than colours of its own.
    // Those are the pair already chosen to stay readable over an arbitrary
    // map -- the halo is what makes the text legible over both water and a
    // motorway -- and a button that picked its own would be legible over
    // whatever the style was when it was written.
    RecentreButton(const QColor& glyph, const QColor& disc, QWidget* parent = nullptr);

    // Logical pixels, square. Chosen big enough to hit with a finger on a
    // dashboard, which is a larger target than a mouse needs.
    static constexpr int kSize = 34;
    // Between the button and the corner of the map.
    static constexpr int kMargin = 10;

    QSize sizeHint() const override { return QSize(kSize, kSize); }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QColor mGlyph;
    QColor mDisc;
};

} // namespace map_widget

#endif // MAP_RECENTRE_BUTTON_H
