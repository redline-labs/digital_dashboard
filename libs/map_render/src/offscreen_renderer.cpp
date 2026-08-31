// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_render/offscreen_renderer.h"

#include <rhi/qrhi.h>
#if MAP_HAS_VULKAN
#include <rhi/qrhi_platform.h>
#endif

#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QColor>
#include <QElapsedTimer>
#include <QMatrix4x4>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <utility>

namespace map_render
{
namespace
{

// Refuse absurd viewports rather than trying to allocate for them.
constexpr int kMaxDimension = 8192;

// The ratio this frame will actually be rendered at: the screen's, unless the
// viewport is wide enough that the device-pixel texture would cross
// kMaxDimension.
//
// Clamping rather than failing matters because the alternative is a blank map:
// ensureTarget() refuses an oversized texture, render() returns a null image,
// and the widget fills its background with no diagnostic to say why. A map
// that comes back soft is a far better answer than one that does not come back.
double renderRatio(const Projection& projection)
{
    double ratio = projection.devicePixelRatio();
    if (projection.viewportWidth() > 0.0)
    {
        ratio = std::min(ratio, double(kMaxDimension) / projection.viewportWidth());
    }
    if (projection.viewportHeight() > 0.0)
    {
        ratio = std::min(ratio, double(kMaxDimension) / projection.viewportHeight());
    }
    return ratio;
}

} // namespace

OffscreenRenderer::OffscreenRenderer() = default;
OffscreenRenderer::~OffscreenRenderer() = default;

std::unique_ptr<OffscreenRenderer> OffscreenRenderer::create()
{
    std::unique_ptr<OffscreenRenderer> renderer(new OffscreenRenderer);
    if (!renderer->initialise())
    {
        return nullptr;
    }
    return renderer;
}

QString OffscreenRenderer::backendName() const
{
    return mRhi ? QString::fromUtf8(mRhi->backendName()) : QString();
}

bool OffscreenRenderer::initialise()
{
    // Tried in the order each platform actually prefers. OpenGL is last
    // everywhere: it is the backend that always exists, not the one to reach
    // for first. It does work under a headless QPA plugin, provided it is given
    // the fallback surface below -- and it has to, because the offscreen plugin
    // never implements createPlatformVulkanInstance, so Vulkan cannot come up
    // there no matter what the driver supports.
#if QT_CONFIG(metal)
    {
        QRhiMetalInitParams params;
        mRhi.reset(QRhi::create(QRhi::Metal, &params));
    }
#endif

#if MAP_HAS_VULKAN
    if (!mRhi)
    {
        // A QVulkanInstance is not optional -- QRhi::create() rejects Vulkan
        // params with a null one. Kept alive for as long as the QRhi, which is
        // why it is a member and not a local.
        mVulkan = std::make_unique<QVulkanInstance>();
        if (mVulkan->create())
        {
            QRhiVulkanInitParams params;
            params.inst = mVulkan.get();
            mRhi.reset(QRhi::create(QRhi::Vulkan, &params));
        }
        if (!mRhi)
        {
            mVulkan.reset();
        }
    }
#endif

    if (!mRhi)
    {
        // 3.3 core because that is what the shaders are baked for: CMakeLists
        // runs qsb at --glsl 330, so a context below it has nothing to compile.
        // Qt's default surface format is GL 2.0, which QRhi honours and then
        // reports as GLSL 120 -- and the mismatch does not surface here, where
        // the backend comes up happily, but later at pipeline creation as "no
        // GLSL shader code found". Asking for the version the bake targets puts
        // the failure, if there is one, at the point that can explain it.
        QRhiGles2InitParams params;
        params.format.setVersion(3, 3);
        params.format.setProfile(QSurfaceFormat::CoreProfile);

        // The fallback surface is NOT optional, and not merely a nicety for
        // headless. QRhi drives GL through a context that has to be current on
        // something, and with no window in play there is nothing to make it
        // current on. Given no surface to fall back to, QRhi does not decline
        // the backend -- it dereferences the null and takes the process with it.
        // It has to carry the same format as the context, or the two disagree.
        mFallbackSurface.reset(QRhiGles2InitParams::newFallbackSurface(params.format));
        params.fallbackSurface = mFallbackSurface.get();
        mRhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
        if (!mRhi)
        {
            mFallbackSurface.reset();
        }
    }

    if (!mRhi)
    {
        SPDLOG_ERROR("[map] no GPU backend available; the map cannot be drawn");
        return false;
    }

    // 4x if the hardware has it. Multisampling is not what a frame costs -- see
    // the header: at 5120x2880 the difference between 4x and none is under a
    // millisecond, against six for the pixels themselves -- so the only reason
    // not to antialias is not being able to.
    //
    // supportedSampleCounts() is the wrong question on its own: it answers
    // "can this backend multisample", where ensureTarget() needs "can it
    // multisample into a TEXTURE", which QRhi tracks separately. The GL backend
    // here reports 1 2 4 8 16 and lets newTexture(..., 4, ...) succeed, then
    // fails at framebuffer completeness -- an incomplete attachment two calls
    // away from the list that promised it. Ask the feature that governs it.
    const QList<int> counts = mRhi->isFeatureSupported(QRhi::MultisampleTexture)
                                  ? mRhi->supportedSampleCounts()
                                  : QList<int> { 1 };
    mSampleCount = counts.contains(4) ? 4 : (counts.contains(2) ? 2 : 1);

    SPDLOG_INFO("[map] GPU renderer on {} ({}x MSAA)", mRhi->backendName(), mSampleCount);
    return true;
}

bool OffscreenRenderer::ensureTarget(const QSize& size)
{
    if (mTarget && mSize == size)
    {
        return true;
    }
    if (size.width() <= 0 || size.height() <= 0 || size.width() > kMaxDimension ||
        size.height() > kMaxDimension)
    {
        return false;
    }

    // Order matters on teardown: the pass's pipelines reference the render pass
    // descriptor, which references the target.
    mPass.releaseResources();
    mTarget.reset();
    mRenderPass.reset();
    mResolve.reset();
    mMsaaColour.reset();

    mResolve.reset(mRhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                    QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!mResolve->create())
    {
        return false;
    }

    QRhiColorAttachment attachment;
    if (mSampleCount > 1)
    {
        mMsaaColour.reset(
            mRhi->newTexture(QRhiTexture::RGBA8, size, mSampleCount, QRhiTexture::RenderTarget));
        if (!mMsaaColour->create())
        {
            return false;
        }
        // Draw multisampled, then resolve into the single-sample texture that
        // gets read back. Reading back the multisample texture directly is not
        // a thing.
        attachment.setTexture(mMsaaColour.get());
        attachment.setResolveTexture(mResolve.get());
    }
    else
    {
        attachment.setTexture(mResolve.get());
    }

    QRhiTextureRenderTargetDescription description({ attachment });
    mTarget.reset(mRhi->newTextureRenderTarget(description));
    mRenderPass.reset(mTarget->newCompatibleRenderPassDescriptor());
    mTarget->setRenderPassDescriptor(mRenderPass.get());
    if (!mTarget->create())
    {
        return false;
    }

    // Everything that survives a resize -- the pipelines, the uniform buffer,
    // the glyph atlas -- is MapPass's, and it is rebuilt here because a new
    // target means a new render pass descriptor and a pipeline is built against
    // one. The geometry already on the GPU goes with it; releaseResources()
    // clears the residency table for exactly that reason.
    if (!mPass.createResources(mRhi.get(), mRenderPass.get(), mSampleCount))
    {
        SPDLOG_ERROR("[map] GPU pass resources could not be created");
        return false;
    }

    mSize = size;
    return true;
}

const QImage& OffscreenRenderer::render(const Projection& projection,
                                  const std::vector<GpuBatch>& requested, const MapStyle_t& style,
                                  const QColor& background, const Highlight& highlight)
{
    static const QImage kNull;

    QElapsedTimer timer;
    timer.start();

    // DEVICE pixels, which is the whole point of the ratio: the projection is
    // logical because the label and marker passes drawn over this frame go
    // through QPainter, and this is the one pass that can render at what the
    // screen actually has. Rendering it logical left the geometry upscaled
    // underneath text drawn sharp, which reads as a blurry map rather than as
    // a bug.
    const double ratio = renderRatio(projection);
    const QSize size(int(std::lround(projection.viewportWidth() * ratio)),
                     int(std::lround(projection.viewportHeight() * ratio)));
    if (!mRhi || !ensureTarget(size))
    {
        return kNull;
    }
    mPass.stats().devicePixelRatio = ratio;

    // A view, not a copy: the old copy-and-rebind idiom only worked because
    // kMaxTilesPerFrame is nonzero, and cost a vector of shared_ptrs per
    // clamped frame besides.
    const std::span<const GpuBatch> batches = mPass.clampToBudget(requested);

    // Same camera, same tiles, same style, same viewport -- so the same
    // pixels. Handing back the frame already in hand skips the draw AND the
    // readback, which together are the whole cost.
    //
    // The scalar half of the key is built on the stack; the vector half is
    // COMPARED IN PLACE against what the memo already holds rather than built
    // first -- the hit path is every idle repaint, and paying three vector
    // allocations per frame to conclude "nothing changed" was the memo's
    // whole cost.
    FrameKey key;
    key.size = size;
    key.center = projection.camera().center;
    key.zoom = projection.camera().zoom;
    key.bearing = projection.camera().bearing;
    key.pitch = projection.camera().pitch;
    // Times the ratio, because a vertex's halfPx is a LOGICAL pixel count and
    // this frame is drawn in device ones -- without it roads come out a ratio
    // thinner on a HiDPI screen while everything else keeps its width. It also
    // means the ratio is part of the key, along with `size` above.
    key.widthScale =
        widthScaleForZoom(projection.camera().zoom) * float(style.road_width_scale) * float(ratio);
    key.background = background.rgba();
    // The highlight is part of the picture, so it is part of the key -- without
    // it, moving onto the next road leaves the previous one lit until something
    // else happens to invalidate the frame.
    key.highlightColour = highlight.colour.rgba();
    key.highlightWidth = highlight.extraHalfPx;
    for (std::size_t li = 0; li < kMapLayerCount; ++li)
    {
        key.layerMinZooms[li] = layerMinZoom(static_cast<MapLayer>(li), style);
    }

    const auto batchesMatchKey = [&]() {
        if (mFrameKey.ids.size() != batches.size())
        {
            return false;
        }
        for (std::size_t i = 0; i < batches.size(); ++i)
        {
            if (!(mFrameKey.ids[i] == batches[i].id) ||
                mFrameKey.serials[i] !=
                    (batches[i].geometry ? batches[i].geometry->serial : 0) ||
                mFrameKey.alphas[i] != quantizeAlpha(batches[i].alpha))
            {
                return false;
            }
        }
        return mFrameKey.highlightIds == highlight.osmWayIds;
    };

    // Text, compared in place for the same reason the batch vectors are: the
    // hit path is every idle repaint, and building a copy to conclude "nothing
    // changed" is the one cost the memo cannot afford.
    const auto textMatchesKey = [&]() { return mFrameKey.text == mPass.text(); };

    if (mHaveFrame && !mFrame.isNull() && key.scalarsEqual(mFrameKey) && batchesMatchKey() &&
        textMatchesKey())
    {
        ++mPass.stats().reused;
        mPass.stats().lastFrameMs = double(timer.nsecsElapsed()) / 1.0e6;
        return mFrame;
    }

    // A miss: refill the stored key's vectors in place, keeping their
    // capacity across frames.
    key.highlightIds = std::move(mFrameKey.highlightIds);
    key.highlightIds.assign(highlight.osmWayIds.begin(), highlight.osmWayIds.end());
    key.text = mPass.text();
    key.ids = std::move(mFrameKey.ids);
    key.serials = std::move(mFrameKey.serials);
    key.alphas = std::move(mFrameKey.alphas);
    key.ids.clear();
    key.serials.clear();
    key.alphas.clear();
    key.ids.reserve(batches.size());
    key.serials.reserve(batches.size());
    key.alphas.reserve(batches.size());
    for (const GpuBatch& batch : batches)
    {
        key.ids.push_back(batch.id);
        key.serials.push_back(batch.geometry ? batch.geometry->serial : 0);
        key.alphas.push_back(quantizeAlpha(batch.alpha));
    }

    MapPass::Frame frame;
    frame.projection = &projection;
    frame.batches = batches;
    frame.style = &style;
    frame.highlight = &highlight;
    frame.sizePx = size;
    frame.ratio = ratio;

    QRhiCommandBuffer* cb = nullptr;
    if (mRhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
    {
        return kNull;
    }

    QRhiResourceUpdateBatch* updates = mRhi->nextResourceUpdateBatch();
    if (!mPass.prepare(frame, updates))
    {
        updates->release();
        mRhi->endOffscreenFrame();
        return kNull;
    }

    cb->beginPass(mTarget.get(), background, { 1.0f, 0 }, updates);
    mPass.record(frame, cb);
    cb->endPass();

    QRhiReadbackResult readback;
    QRhiResourceUpdateBatch* readUpdates = mRhi->nextResourceUpdateBatch();
    readUpdates->readBackTexture(QRhiReadbackDescription(mResolve.get()), &readback);
    cb->resourceUpdate(readUpdates);

    mRhi->endOffscreenFrame();

    if (readback.data.isEmpty())
    {
        return kNull;
    }

    // MOVED, not copied. The bytes are already the frame; copying them buys a
    // whole-image memcpy every paint to produce something identical. Holding
    // them in a member is what lets the QImage below be a view -- and render()
    // already promises the image is only valid until the next call, which is
    // exactly how long the member lives.
    mReadbackData = std::move(readback.data);
    // Rebuilt each frame rather than kept: the buffer moves in from a fresh
    // QByteArray every time, so a QImage held across frames would be pointing
    // at whatever the previous one owned.
    mFrame = QImage(reinterpret_cast<const uchar*>(mReadbackData.constData()),
                    readback.pixelSize.width(), readback.pixelSize.height(),
                    QImage::Format_RGBA8888);
    // What makes the blit a straight one: with this set, QPainter draws the
    // frame across the logical rectangle it was rendered for instead of
    // treating its device pixels as logical ones and covering four times the
    // widget. It is also the clamp's escape hatch -- a frame that had to come
    // back at less than the screen's ratio still lands on the same rectangle,
    // upscaled.
    mFrame.setDevicePixelRatio(ratio);

    mFrameKey = std::move(key);
    mHaveFrame = true;

    mPass.stats().lastFrameMs = double(timer.nsecsElapsed()) / 1.0e6;
    return mFrame;
}

} // namespace map_render
