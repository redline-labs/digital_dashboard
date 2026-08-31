// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_overlay.h"

#include <QElapsedTimer>
#include <QPainter>

#include <utility>

namespace map_surface
{

MapOverlay::MapOverlay(QWidget* parent) : QWidget(parent)
{
    // No background of its own: the map underneath has to show through, and
    // WA_NoSystemBackground is what stops Qt filling the rectangle first.
    setAttribute(Qt::WA_NoSystemBackground);
    // Hit tests fall through to the surface, and from there to the embedder --
    // which is what keeps pan, zoom and the drag-to-rotate gesture working
    // exactly as they did when the embedder painted the map itself.
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

void MapOverlay::setPainter(MapSurface::OverlayPainter painter)
{
    mPainter = std::move(painter);
    update();
}

MapSurface::OverlayPainter MapOverlay::takePainter()
{
    return std::move(mPainter);
}

void MapOverlay::paintEvent(QPaintEvent*)
{
    QElapsedTimer timer;
    timer.start();

    if (mPainter)
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        mPainter(painter);
    }

    mLastPaintMs = double(timer.nsecsElapsed()) / 1.0e6;
    ++mFramesPainted;
}

} // namespace map_surface
