// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_surface/map_surface.h"

#include <QGuiApplication>
#include <QResizeEvent>
#include <private/qguiapplication_p.h>
#include <qpa/qplatformintegration.h>

#include <spdlog/spdlog.h>

#include <utility>

#include "map_host.h"
#include "map_overlay.h"
#include "offscreen_host.h"
#include "rhi_widget_host.h"

namespace map_surface
{

bool rhiWidgetsAreAvailable()
{
    // The same question QRhiWidget asks itself before it will initialise, asked
    // early enough that a surface can pick a host rather than find out from
    // a widget that silently draws nothing.
    static const bool available = [] {
        auto* platform = QGuiApplicationPrivate::platformIntegration();
        return platform != nullptr &&
               platform->hasCapability(QPlatformIntegration::RhiBasedRendering);
    }();
    return available;
}

MapSurface::MapSurface(QWidget* parent) : MapSurface(Host::automatic, parent) {}

MapSurface::MapSurface(Host host, QWidget* parent) : QWidget(parent)
{
    // The embedder keeps its gestures. Everything in here is scenery, and Qt's hit
    // test does not descend into a transparent-for-mouse subtree -- which is
    // also why the map_controls buttons must stay children of the EMBEDDER. See the
    // header.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    build(host);
}

MapSurface::~MapSurface() = default;

void MapSurface::build(Host host)
{
    if (host == Host::automatic && rhiWidgetsAreAvailable())
    {
        auto rhi = std::make_unique<RhiWidgetHost>(this);
        // isUsable() is only meaningful after Qt has tried to initialise, which
        // has not happened yet -- so this is optimistic, and the surface falls
        // back on the first frame if it turns out wrong. See setFrame().
        mHost = std::move(rhi);
    }
    else
    {
        mHost = std::make_unique<OffscreenHost>(this);
    }

    QWidget* widget = mHost->widget();
    widget->setGeometry(rect());
    // A CHILD of the host's widget, not a sibling: that is the arrangement measured
    // to composite over a render-to-texture widget. See map_overlay.h.
    mOverlay = new MapOverlay(widget);
    mOverlay->setGeometry(widget->rect());
    mOverlay->raise();
}

void MapSurface::setOverlayPainter(OverlayPainter painter)
{
    if (mOverlay)
    {
        mOverlay->setPainter(std::move(painter));
    }
}

void MapSurface::setFrame(const map_render::Projection& projection, std::vector<GpuBatch> batches,
                          const MapStyle_t& style, const QColor& background,
                          const Highlight& highlight)
{
    if (!mHost)
    {
        return;
    }

    // The one place the choice can still be wrong: QRhiWidget only reports
    // failure once Qt has tried to bring it up, which is after the first paint.
    // Swapping here rather than leaving a widget that draws nothing is the
    // whole reason an embedder never has to ask which host it got.
    if (mHost->usesRhi() && !mHost->isUsable())
    {
        SPDLOG_WARN("[map] the RHI map surface did not come up; falling back to the offscreen "
                    "renderer");
        OverlayPainter painter;
        if (mOverlay)
        {
            painter = mOverlay->takePainter();
        }
        mOverlay = nullptr;
        mHost.reset();
        build(Host::offscreen);
        setOverlayPainter(std::move(painter));
        mHost->widget()->setGeometry(rect());
        if (mOverlay)
        {
            mOverlay->setGeometry(mHost->widget()->rect());
        }
    }

    MapContent content;
    content.projection = std::make_unique<map_render::Projection>(projection);
    content.batches = std::move(batches);
    content.style = style;
    content.background = background;
    content.highlight = highlight;
    mHost->submit(std::move(content));

    // Only where the host will not do it for us. See
    // MapHost::repaintsOverlay(): hiding this difference is the whole job of
    // this class, and issuing the repaint unconditionally would cost the
    // offscreen host a second overlay paint every frame.
    if (mOverlay && !mHost->repaintsOverlay())
    {
        mOverlay->update();
    }
}

void MapSurface::setText(std::vector<map_render::TextQuad> quads, const QImage& atlasPage,
                         bool atlasDirty)
{
    if (mHost)
    {
        mHost->setText(std::move(quads), atlasPage, atlasDirty);
    }
}

void MapSurface::refreshOverlay()
{
    if (mOverlay)
    {
        mOverlay->update();
    }
}

double MapSurface::lastMapMs() const
{
    return mHost ? mHost->lastMapMs() : 0.0;
}

std::uint64_t MapSurface::framesDrawn() const
{
    return mHost ? mHost->framesDrawn() : 0;
}

double MapSurface::lastOverlayMs() const
{
    return mOverlay ? mOverlay->lastPaintMs() : 0.0;
}

std::uint64_t MapSurface::overlayFramesPainted() const
{
    return mOverlay ? mOverlay->framesPainted() : 0;
}

bool MapSurface::isUsable() const
{
    return mHost && mHost->isUsable();
}

bool MapSurface::usesRhi() const
{
    return mHost && mHost->usesRhi();
}

QString MapSurface::backendName() const
{
    return mHost ? mHost->backendName() : QString();
}

MapSurface::Stats MapSurface::stats() const
{
    return mHost ? mHost->stats() : Stats {};
}

void MapSurface::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    if (mHost)
    {
        mHost->widget()->setGeometry(rect());
    }
    if (mOverlay)
    {
        mOverlay->setGeometry(QRect(QPoint(0, 0), size()));
    }
}

} // namespace map_surface
