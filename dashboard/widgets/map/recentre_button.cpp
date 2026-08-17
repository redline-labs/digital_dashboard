// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/recentre_button.h"

#include <QPainter>
#include <QPen>

namespace map_widget
{

RecentreButton::RecentreButton(const QColor& glyph, const QColor& disc, QWidget* parent) :
    QAbstractButton(parent), mGlyph(glyph), mDisc(disc)
{
    setCursor(Qt::ArrowCursor);
    // No focus, because the map is not a form. Taking focus here would move it
    // off whatever the rest of the dashboard had it on, and a button that
    // appears mid-gesture stealing focus is worse than one that cannot be
    // tabbed to.
    setFocusPolicy(Qt::NoFocus);
    setToolTip(QStringLiteral("Recentre the map"));
    resize(kSize, kSize);
}

void RecentreButton::paintEvent(QPaintEvent* event)
{
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QPointF centre = box.center();

    // The disc is the label HALO colour at most of its opacity -- the same
    // trick the labels use to stay readable over anything, rather than a fixed
    // dark rectangle that would vanish over a dark style.
    QColor disc = mDisc;
    disc.setAlphaF(isDown() ? 0.95F : (underMouse() ? 0.88F : 0.78F));
    painter.setPen(Qt::NoPen);
    painter.setBrush(disc);
    painter.drawEllipse(box);

    // A locate glyph: a ring, a centre dot, and four ticks on the axes. It
    // reads as "put me back in the middle" without a word of text, which
    // matters because there is no room for one and no translation pipeline.
    QColor glyph = mGlyph;
    glyph.setAlphaF(underMouse() || isDown() ? 1.0F : 0.85F);

    QPen pen(glyph);
    pen.setWidthF(1.6);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);

    const double ring = box.width() * 0.26;
    painter.drawEllipse(centre, ring, ring);

    const double tickFrom = ring * 1.35;
    const double tickTo = ring * 1.95;
    painter.drawLine(QPointF(centre.x(), centre.y() - tickFrom),
                     QPointF(centre.x(), centre.y() - tickTo));
    painter.drawLine(QPointF(centre.x(), centre.y() + tickFrom),
                     QPointF(centre.x(), centre.y() + tickTo));
    painter.drawLine(QPointF(centre.x() - tickFrom, centre.y()),
                     QPointF(centre.x() - tickTo, centre.y()));
    painter.drawLine(QPointF(centre.x() + tickFrom, centre.y()),
                     QPointF(centre.x() + tickTo, centre.y()));

    painter.setPen(Qt::NoPen);
    painter.setBrush(glyph);
    painter.drawEllipse(centre, ring * 0.34, ring * 0.34);
}

} // namespace map_widget
