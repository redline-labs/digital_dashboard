// SPDX-License-Identifier: GPL-3.0-or-later

#include "map_controls/map_button.h"

#include <QPainter>
#include <QPen>

#include <algorithm>

namespace map_controls
{

MapButton::MapButton(const QColor& glyph, const QColor& disc, QWidget* parent) :
    QAbstractButton(parent), mGlyph(glyph), mDisc(disc)
{
    setCursor(Qt::ArrowCursor);
    // No focus, because the map is not a form. Taking focus here would move it
    // off whatever the rest of the dashboard had it on, and a button that
    // appears mid-gesture stealing focus is worse than one that cannot be
    // tabbed to.
    setFocusPolicy(Qt::NoFocus);
    resize(kSize, kSize);
}

void MapButton::setEmphasis(double emphasis)
{
    const double clamped = std::clamp(emphasis, kDeEmphasized, 1.0);
    if (clamped == mEmphasis)
    {
        return;
    }
    mEmphasis = clamped;
    update();
}

void MapButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setOpacity(mEmphasis);

    const QRectF box = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);

    // The shadow: three concentric near-black ellipses, each a little larger
    // and fainter, offset a pixel down. Painted rather than a
    // QGraphicsDropShadowEffect, which re-renders through an offscreen buffer
    // and misbehaves under the offscreen platform the agent interface runs.
    painter.setPen(Qt::NoPen);
    for (int ring = 3; ring >= 1; --ring)
    {
        QColor shade(0, 0, 0);
        shade.setAlphaF(0.05F * float(4 - ring));
        painter.setBrush(shade);
        const double grow = 0.5 * double(ring);
        painter.drawEllipse(box.adjusted(-grow, -grow + 1.0, grow, grow + 1.0));
    }

    // The disc is the label HALO colour at most of its opacity -- the same
    // trick the labels use to stay readable over anything, rather than a fixed
    // dark rectangle that would vanish over a dark style.
    QColor disc = mDisc;
    disc.setAlphaF(isDown() ? 0.95F : (underMouse() ? 0.88F : 0.78F));
    painter.setBrush(disc);
    painter.drawEllipse(box);

    QColor glyph = mGlyph;
    glyph.setAlphaF(underMouse() || isDown() ? 1.0F : 0.85F);
    paintGlyph(painter, box, glyph);
}

void layOutStack(std::initializer_list<QAbstractButton*> buttons, const QSize& hostSize,
                 Qt::Corner corner)
{
    const int size = MapButton::kSize;
    const int margin = MapButton::kMargin;
    const int step = size + MapButton::kSpacing;

    const bool right =
        corner == Qt::TopRightCorner || corner == Qt::BottomRightCorner;
    const bool bottom =
        corner == Qt::BottomLeftCorner || corner == Qt::BottomRightCorner;
    const int x = right ? hostSize.width() - size - margin : margin;

    int slot = 0;
    for (QAbstractButton* button : buttons)
    {
        if (button == nullptr || button->isHidden())
        {
            continue;
        }
        const int y = bottom ? hostSize.height() - size - margin - (slot * step)
                             : margin + (slot * step);
        // A stack that has outgrown its host hides the tail rather than
        // painting buttons off the edge or over whatever lives in the
        // opposite corner. The far end is the least essential by
        // construction -- callers order the list nearest-corner-first.
        if (y < margin || y + size + margin > hostSize.height())
        {
            button->hide();
            continue;
        }
        button->setGeometry(x, y, size, size);
        ++slot;
    }
}

} // namespace map_controls
