// SPDX-License-Identifier: GPL-3.0-or-later
#include "offscreen_host.h"

#include <QElapsedTimer>
#include <QPainter>
#include <QPaintEvent>

#include <utility>

namespace map_surface
{

OffscreenHost::OffscreenHost(QWidget* parent)
    : QWidget(parent), mRenderer(map_render::OffscreenRenderer::create())
{
    // NOT WA_OpaquePaintEvent, even though this does fill its own rectangle.
    // A child that declares itself opaque and covers its parent has Qt skip
    // the PARENT's paint entirely -- and the embedder's paintEvent is the frame
    // driver, so declaring it cost the map every frame it was ever asked for.
    // Hit tests fall through to the embedder, as with the overlay.
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

OffscreenHost::~OffscreenHost() = default;

void OffscreenHost::submit(MapContent content)
{
    mContent = std::move(content);
    update();
}

void OffscreenHost::setText(std::vector<map_render::TextQuad> quads, const QImage& atlasPage,
                               bool atlasDirty)
{
    if (mRenderer)
    {
        mRenderer->setText(std::move(quads), atlasPage, atlasDirty);
    }
}

QString OffscreenHost::backendName() const
{
    return mRenderer ? mRenderer->backendName() : QString();
}

map_render::MapPass::Stats OffscreenHost::stats() const
{
    return mRenderer ? mRenderer->stats() : map_render::MapPass::Stats {};
}

void OffscreenHost::paintEvent(QPaintEvent*)
{
    if (!mRenderer)
    {
        return;
    }
    QElapsedTimer timer;
    timer.start();

    QPainter painter(this);
    if (!mContent.projection)
    {
        // Nothing handed to us yet. Fill rather than leave whatever the backing
        // store held -- WA_OpaquePaintEvent means Qt will not have.
        painter.fillRect(rect(), mContent.background);
        return;
    }

    // The overlay is a CHILD of this widget and has no background of its own,
    // so Qt repaints this rectangle underneath it whenever the marker moves --
    // which means a frame where only the marker moved arrives here as a full
    // map repaint. The frame memo is what makes that nearly free, and it
    // carries far more weight in this arrangement than it did when the host
    // painted the map itself. MEASURED: half of all paints in the bench are
    // memo hits.
    const std::uint64_t reusedBefore = mRenderer->stats().reused;
    const QImage& image = mRenderer->render(*mContent.projection, mContent.batches,
                                            mContent.style, mContent.background,
                                            mContent.highlight);
    const bool redrew = mRenderer->stats().reused == reusedBefore;
    if (image.isNull())
    {
        painter.fillRect(rect(), mContent.background);
        return;
    }
    // The image carries its own device pixel ratio, so this lands across the
    // logical rectangle it was rendered for rather than at device scale.
    painter.drawImage(QPointF(0.0, 0.0), image);

    // Everything the QRhiWidget host does NOT do is inside this: the synchronous
    // wait for the GPU, the readback, and this blit.
    // A memo hit did not draw a map, so it is not a frame -- counting it would
    // fold a nearly free repaint into the median as if the map had been
    // rendered.
    if (redrew)
    {
        mLastPaintMs = double(timer.nsecsElapsed()) / 1.0e6;
        ++mFramesPainted;
    }
}

} // namespace map_surface
