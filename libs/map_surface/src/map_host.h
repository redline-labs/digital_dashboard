// SPDX-License-Identifier: GPL-3.0-or-later
//
// The seam between the two ways of getting a map onto a widget.
//
// Deliberately tiny, and deliberately NOT a QObject: what varies between the
// QRhiWidget host and the offscreen one is the frame lifecycle and nothing
// else, so the interface is "here is a widget, here is new content, draw it
// when you can". Everything about WHAT is drawn is map_render's MapPass, which
// both hosts share verbatim -- see map_pass.h for why that split is where it
// is.
#ifndef MAP_SURFACE_HOST_H
#define MAP_SURFACE_HOST_H

#include <cstdint>
#include <memory>
#include <vector>

#include <QColor>
#include <QImage>
#include <QString>

#include "map_render/map_pass.h"
#include "map_render/projection.h"
#include "map_render/style.h"

class QWidget;

namespace map_surface
{

// One frame's worth of what to draw, owned by whoever was given it last.
//
// Projection is held by pointer because it has no default constructor -- it is
// built from a camera and a viewport and there is no meaningful empty one --
// and null is the honest way to say "nothing has been handed to us yet".
struct MapContent
{
    std::unique_ptr<map_render::Projection> projection;
    std::vector<map_render::GpuBatch> batches;
    MapStyle_t style;
    QColor background { Qt::black };
    map_render::MapPass::Highlight highlight;
};

class MapHost
{
  public:
    virtual ~MapHost() = default;

    // The widget to put in the layout. Owned by the host.
    virtual QWidget* widget() = 0;

    // New content. The host takes it and schedules whatever redraw it needs.
    virtual void submit(MapContent content) = 0;
    virtual void setText(std::vector<map_render::TextQuad> quads, const QImage& atlasPage,
                         bool atlasDirty) = 0;

    // GUI-thread milliseconds the last map frame cost THIS host, and how
    // many it has drawn.
    //
    // The same quantity for both, which is the only way they can be compared:
    // for the offscreen host it is the whole paintEvent -- render, wait for
    // the GPU, copy the frame back, blit it -- and for the RHI one it is
    // recording the pass, because there is nothing else. It excludes the embedder's
    // own overlay in both cases.
    virtual double lastMapMs() const = 0;
    virtual std::uint64_t framesDrawn() const = 0;

    // Whether redrawing the map already repaints the transparent layer over it.
    //
    // The two do NOT agree, and it is the one behavioural difference an embedder
    // would otherwise notice. Repainting a plain QWidget repaints the
    // transparent child on top of it; a QRhiWidget's render() never touches its
    // children, so its marker would freeze while the map moved underneath. The
    // surface asks this and issues the extra repaint only where it is needed --
    // asking unconditionally cost the offscreen host TWO overlay paints per
    // frame, which is measurable.
    virtual bool repaintsOverlay() const = 0;

    virtual bool isUsable() const = 0;
    virtual bool usesRhi() const = 0;
    virtual QString backendName() const = 0;
    virtual map_render::MapPass::Stats stats() const = 0;
};

} // namespace map_surface

#endif // MAP_SURFACE_HOST_H
