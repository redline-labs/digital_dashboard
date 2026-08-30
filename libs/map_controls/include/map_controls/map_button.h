// SPDX-License-Identifier: GPL-3.0-or-later
//
// The floating buttons both map surfaces overlay on their maps.
//
// One base class carrying everything that makes a control look like it belongs
// on TOP of a map rather than beside it: a translucent disc in the style's
// label-halo colour (the pair already chosen to stay readable over water and
// motorway alike), a soft painted shadow that separates it from whatever the
// map draws underneath, and hover/press expressed through opacity. Subclasses
// paint only their glyph.
//
// A REAL QAbstractButton and not a rectangle painted into the host's
// paintEvent, for three reasons that are each small and together decisive:
// hover and press states come for free, the editor already makes child widgets
// mouse-transparent in edit mode so it cannot swallow a selection drag, and it
// appears in ui_snapshot as an addressable widget rather than as a coordinate
// the agent interface has to be told about.
//
// It paints itself rather than carrying an icon: there is no icon pipeline in
// this tree, and a QIcon would need one per state per theme.
#ifndef MAP_CONTROLS_MAP_BUTTON_H
#define MAP_CONTROLS_MAP_BUTTON_H

#include <QAbstractButton>
#include <QColor>

#include <initializer_list>

namespace map_controls
{

class MapButton : public QAbstractButton
{
    Q_OBJECT

  public:
    // Takes the LABEL colours from the style rather than colours of its own --
    // a button that picked its own would be legible over whatever the style
    // was when it was written.
    MapButton(const QColor& glyph, const QColor& disc, QWidget* parent = nullptr);

    // Logical pixels, square. Chosen big enough to hit with a finger on a
    // dashboard, which is a larger target than a mouse needs.
    static constexpr int kSize = 34;
    // Between a button and the corner of the map.
    static constexpr int kMargin = 10;
    // Between two buttons in a stack.
    static constexpr int kSpacing = 8;

    QSize sizeHint() const override { return QSize(kSize, kSize); }

  protected:
    void paintEvent(QPaintEvent* event) override;

    // The subclass's mark, drawn over the disc. `box` is the button's own
    // rect, already antialiased and centred; the colour carries the state
    // alpha, so a glyph that uses it as given hovers and presses like every
    // other.
    virtual void paintGlyph(QPainter& painter, const QRectF& box, const QColor& glyph) = 0;

    // How present the whole button is. 1 is a button asking to be pressed;
    // the compass drops toward kDeEmphasized when it has nothing to say.
    // Applied to disc, shadow and glyph together, so a quiet button recedes
    // rather than becoming a dark hole with a bright mark in it.
    void setEmphasis(double emphasis);
    static constexpr double kDeEmphasized = 0.4;

    const QColor& glyphColour() const { return mGlyph; }

  private:
    QColor mGlyph;
    QColor mDisc;
    double mEmphasis { 1.0 };
};

// Stack `buttons` into `corner` of a host widget of `hostSize`, nearest the
// corner first, one MapButton::kSpacing apart. Hidden buttons take no slot, so
// a transient button leaves no hole when it goes -- pass every button and call
// again when one shows or hides. Today's two users both stack vertically;
// horizontal can join the parameters when a layout wants it.
void layOutStack(std::initializer_list<QAbstractButton*> buttons, const QSize& hostSize,
                 Qt::Corner corner);

} // namespace map_controls

#endif // MAP_CONTROLS_MAP_BUTTON_H
