// SPDX-License-Identifier: GPL-3.0-or-later
//
// A transparent layer over the map, for the embedder's own QPainter drawing.
//
// It exists because a QRhiWidget HAS NO PAINT ENGINE: QPainter on one logs
// "QWidget::paintEngine: Should no longer be called" and draws nothing, so the
// marker and the trail cannot be painted after the map the way they were when
// the map arrived as a QImage. A child widget over a render-to-texture widget
// composites correctly -- measured on Qt 6.11/cocoa, and it is what
// libs/agent_control/capture.cpp records as well -- so the embedder's painting
// moves here unchanged.
//
// The SAME layer is used with the offscreen host, even though that one could
// paint in its own paintEvent. One structure for both is the whole point: an
// embedder that draws its marker one way headless and another way on screen has two
// code paths to keep in agreement, and only one of them is ever tested.
#ifndef MAP_SURFACE_OVERLAY_H
#define MAP_SURFACE_OVERLAY_H

#include <cstdint>

#include <QWidget>

#include "map_surface/map_surface.h"

namespace map_surface
{

class MapOverlay : public QWidget
{
    Q_OBJECT

  public:
    explicit MapOverlay(QWidget* parent);

    void setPainter(MapSurface::OverlayPainter painter);
    // Handed over when the surface has to swap hosts under us -- the layer is
    // a child of the host's widget, so it dies with it, and the embedder's
    // painter would be lost with it.
    MapSurface::OverlayPainter takePainter();

    // The WHOLE paintEvent, not just the embedder's callback: the dispatch, the
    // QPainter on a widget, the render hints and the embedder's drawing. Timed here
    // rather than inside the callback because the question "what does the
    // transparent layer cost" is mostly about the parts the callback cannot
    // see.
    double lastPaintMs() const { return mLastPaintMs; }
    std::uint64_t framesPainted() const { return mFramesPainted; }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    MapSurface::OverlayPainter mPainter;
    double mLastPaintMs { 0.0 };
    std::uint64_t mFramesPainted { 0 };
};

} // namespace map_surface

#endif // MAP_SURFACE_OVERLAY_H
