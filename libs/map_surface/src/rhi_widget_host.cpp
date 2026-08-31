// SPDX-License-Identifier: GPL-3.0-or-later
#include "rhi_widget_host.h"

#include <rhi/qrhi.h>

#include <QElapsedTimer>

#include <spdlog/spdlog.h>

#include <utility>

namespace map_surface
{

RhiWidgetHost::RhiWidgetHost(QWidget* parent) : QRhiWidget(parent)
{
    // 4x if the target can take it, matching what OffscreenRenderer asks for.
    // The two hosts drawing the same map at different sample counts would make a
    // screenshot differ from the screen, which is the one difference between
    // them nobody would think to look for.
    setSampleCount(4);
    setAutoRenderTarget(true);
    // Hit tests fall through to the surface and from there to the embedder,
    // whose gestures are untouched by which host it got.
    setAttribute(Qt::WA_TransparentForMouseEvents);
}

RhiWidgetHost::~RhiWidgetHost() = default;

void RhiWidgetHost::submit(MapContent content)
{
    mContent = std::move(content);
    update();
}

void RhiWidgetHost::setText(std::vector<map_render::TextQuad> quads, const QImage& atlasPage,
                         bool atlasDirty)
{
    mPass.setText(std::move(quads), atlasPage, atlasDirty);
}

QString RhiWidgetHost::backendName() const
{
    return rhi() ? QString::fromUtf8(rhi()->backendName()) : QString();
}

void RhiWidgetHost::initialize(QRhiCommandBuffer* /*cb*/)
{
    QRhi* device = rhi();
    QRhiRenderTarget* target = renderTarget();
    if (!device || !target)
    {
        mInitialiseFailed = true;
        return;
    }

    // Rebuilt only when the device or the target's FORMAT changed. Qt calls
    // this on every resize as well, and rebuilding there would also throw away
    // every tile already on the GPU -- a full re-upload for a window drag.
    if (device == mBuiltFor && target->renderPassDescriptor() == mBuiltAgainst)
    {
        return;
    }

    if (!mPass.createResources(device, target->renderPassDescriptor(), sampleCount()))
    {
        SPDLOG_ERROR("[map] the map surface's GPU resources could not be created");
        mInitialiseFailed = true;
        mBuiltFor = nullptr;
        mBuiltAgainst = nullptr;
        return;
    }
    mBuiltFor = device;
    mBuiltAgainst = target->renderPassDescriptor();
    mInitialiseFailed = false;
    SPDLOG_INFO("[map] map surface on {} ({}x MSAA)", device->backendName(), sampleCount());
}

void RhiWidgetHost::releaseResources()
{
    mPass.releaseResources();
    mBuiltFor = nullptr;
    mBuiltAgainst = nullptr;
}

void RhiWidgetHost::render(QRhiCommandBuffer* cb)
{
    QRhi* device = rhi();
    QRhiRenderTarget* target = renderTarget();
    if (!device || !target || !mPass.ready())
    {
        return;
    }

    QElapsedTimer timer;
    timer.start();

    const QSize sizePx = target->pixelSize();
    if (sizePx.isEmpty())
    {
        return;
    }

    // Nothing handed to us yet: clear to the background rather than leaving
    // whatever the target held. A surface with no frame should look like an
    // empty map, not like uninitialised memory.
    if (!mContent.projection)
    {
        cb->beginPass(target, mContent.background, { 1.0F, 0 });
        cb->endPass();
        return;
    }

    map_render::MapPass::Frame frame;
    frame.projection = mContent.projection.get();
    frame.batches = mPass.clampToBudget(mContent.batches);
    frame.style = &mContent.style;
    frame.highlight = &mContent.highlight;
    frame.sizePx = sizePx;
    // The ratio the TARGET actually is, not the widget's. QRhiWidget sizes its
    // texture itself and may clamp it, and deriving the ratio from the
    // projection would then disagree with the pixels being drawn into.
    frame.ratio = mContent.projection->viewportWidth() > 0.0
                      ? double(sizePx.width()) / mContent.projection->viewportWidth()
                      : 1.0;
    mPass.stats().devicePixelRatio = frame.ratio;

    QRhiResourceUpdateBatch* updates = device->nextResourceUpdateBatch();
    if (!mPass.prepare(frame, updates))
    {
        updates->release();
        return;
    }

    cb->beginPass(target, mContent.background, { 1.0F, 0 }, updates);
    mPass.record(frame, cb);
    cb->endPass();

    // Recording only. There is no endOffscreenFrame() to wait on and no
    // readback to copy, so this is the GUI thread's whole share of the map.
    mLastRecordMs = double(timer.nsecsElapsed()) / 1.0e6;
    mPass.stats().lastFrameMs = mLastRecordMs;
    ++mFramesRecorded;
}

} // namespace map_surface
