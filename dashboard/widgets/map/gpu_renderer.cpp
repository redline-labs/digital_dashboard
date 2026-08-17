// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/gpu_renderer.h"

#include <rhi/qrhi.h>
// The InitParams structs live here rather than in qrhi.h, and each is behind the
// feature test for its own backend.
#include <rhi/qrhi_platform.h>

#include <QColor>
#include <QElapsedTimer>
#include <QMatrix4x4>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "mapVert_qsb.h"
#include "mapFrag_qsb.h"

namespace map_widget
{
namespace
{

// mat4 + two floats + padding, rounded to a std140-friendly size. The stride
// between tiles is the hardware's uniform alignment, which is larger.
constexpr quint32 kUniformBlockSize = 80;

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

QShader loadShader(const unsigned char* bytes, std::size_t size)
{
    return QShader::fromSerialized(
        QByteArray(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(size)));
}

} // namespace

std::unique_ptr<GpuRenderer> GpuRenderer::create()
{
    std::unique_ptr<GpuRenderer> renderer(new GpuRenderer);
    if (!renderer->initialise())
    {
        return nullptr;
    }
    return renderer;
}

GpuRenderer::GpuRenderer() = default;
GpuRenderer::~GpuRenderer() = default;

bool GpuRenderer::initialise()
{
    // Tried in the order each platform actually prefers. OpenGL is last
    // everywhere: under a headless QPA plugin it needs an offscreen surface
    // from the platform, and `QOpenGLContext::create()` was measured returning
    // false there -- which is the dependency this whole class exists to avoid.
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
        QRhiGles2InitParams params;
        mRhi.reset(QRhi::create(QRhi::OpenGLES2, &params));
    }

    if (!mRhi)
    {
        SPDLOG_ERROR("[map] no GPU backend available; the map cannot be drawn");
        return false;
    }

    mUniformStride = std::max<quint32>(quint32(mRhi->ubufAlignment()), kUniformBlockSize);
    // Fixed for the life of the backend, and read once: it decides which way
    // up the ortho box goes, which is per tile per frame.
    mYUpInFramebuffer = mRhi->isYUpInFramebuffer();

    // 4x if the hardware has it. Multisampling is not what a frame costs -- see
    // the header: at 5120x2880 the difference between 4x and none is under a
    // millisecond, against six for the pixels themselves -- so the only reason
    // not to antialias is not being able to.
    const QList<int> counts = mRhi->supportedSampleCounts();
    mSampleCount = counts.contains(4) ? 4 : (counts.contains(2) ? 2 : 1);
    mStats.sampleCount = mSampleCount;

    // Allocated once, here, and never replaced -- see the header. Both
    // objects outlive every pipeline built against them, which is the whole
    // point of doing it before the first ensureTarget().
    mUniformBuffer.reset(mRhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                         quint32(mUniformStride * kMaxTilesPerFrame)));
    if (!mUniformBuffer->create())
    {
        SPDLOG_ERROR("[map] GPU uniform buffer could not be created");
        return false;
    }

    mSrb.reset(mRhi->newShaderResourceBindings());
    // Dynamic offset: one uniform block per TILE, selected per draw call. The
    // alternative -- a matrix per vertex, or one buffer per tile -- is either
    // more bandwidth or more objects for the same result.
    mSrb->setBindings({ QRhiShaderResourceBinding::uniformBufferWithDynamicOffset(
        0, QRhiShaderResourceBinding::VertexStage, mUniformBuffer.get(), kUniformBlockSize) });
    if (!mSrb->create())
    {
        SPDLOG_ERROR("[map] GPU shader resource bindings could not be created");
        return false;
    }

    SPDLOG_INFO("[map] GPU renderer on {} ({}x MSAA)", mRhi->backendName(), mSampleCount);
    return true;
}

QString GpuRenderer::backendName() const
{
    return mRhi ? QString::fromUtf8(mRhi->backendName()) : QString();
}

bool GpuRenderer::ensureTarget(const QSize& size)
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

    // Order matters on teardown: the pipeline references the render pass
    // descriptor, which references the target.
    mPipeline.reset();
    mTarget.reset();
    mPass.reset();
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
    mPass.reset(mTarget->newCompatibleRenderPassDescriptor());
    mTarget->setRenderPassDescriptor(mPass.get());
    if (!mTarget->create())
    {
        return false;
    }

    mPipeline.reset(mRhi->newGraphicsPipeline());
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    mPipeline->setTargetBlends({ blend });
    mPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    // No culling: MVT ring winding says exterior-or-hole, not front-or-back,
    // and earcut emits whatever order the ear clipping produced.
    mPipeline->setCullMode(QRhiGraphicsPipeline::None);
    mPipeline->setSampleCount(mSampleCount);
    mPipeline->setShaderStages(
        { { QRhiShaderStage::Vertex, loadShader(map_shaders::mapVert, map_shaders::mapVertSize) },
          { QRhiShaderStage::Fragment,
            loadShader(map_shaders::mapFrag, map_shaders::mapFragSize) } });

    QRhiVertexInputLayout layout;
    layout.setBindings({ { sizeof(MapVertex) } });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, offsetof(MapVertex, x) },
        { 0, 1, QRhiVertexInputAttribute::Float2, offsetof(MapVertex, nx) },
        { 0, 2, QRhiVertexInputAttribute::Float, offsetof(MapVertex, halfPx) },
        { 0, 3, QRhiVertexInputAttribute::Float4, offsetof(MapVertex, r) },
    });
    mPipeline->setVertexInputLayout(layout);
    mPipeline->setShaderResourceBindings(mSrb.get());
    mPipeline->setRenderPassDescriptor(mPass.get());
    if (!mPipeline->create())
    {
        SPDLOG_ERROR("[map] GPU pipeline could not be created");
        return false;
    }

    mSize = size;
    return true;
}

bool GpuRenderer::batchesChanged(const std::vector<GpuBatch>& batches) const
{
    if (batches.size() != mUploadedIds.size())
    {
        return true;
    }
    for (std::size_t i = 0; i < batches.size(); ++i)
    {
        if (!(batches[i].id == mUploadedIds[i]))
        {
            return true;
        }
        // Serial, not address and not contents. A tile can be replaced by a
        // re-tessellation while keeping its id, and the replacement is very
        // likely to be handed the dead one's address by the allocator -- so
        // comparing pointers reports "unchanged" and the map silently keeps
        // drawing the vertices it uploaded before. See TileGeometry::serial.
        const std::uint64_t serial = batches[i].geometry ? batches[i].geometry->serial : 0;
        if (serial != mUploadedSerials[i])
        {
            return true;
        }
    }
    return false;
}

bool GpuRenderer::prepareUpload(const std::vector<GpuBatch>& batches,
                                std::vector<MapVertex>& flat)
{
    mTileBaseVertex.clear();
    mUploadedIds.clear();
    mUploadedSerials.clear();

    std::uint32_t total = 0;
    for (const GpuBatch& batch : batches)
    {
        mTileBaseVertex.push_back(total);
        mUploadedIds.push_back(batch.id);
        mUploadedSerials.push_back(batch.geometry ? batch.geometry->serial : 0);
        // Null is a normal state -- a tile that has been asked for but has not
        // arrived. It still takes a slot, so the base-vertex and uniform indices
        // stay aligned with `batches`.
        total += batch.geometry
                     ? static_cast<std::uint32_t>(batch.geometry->vertices.size())
                     : 0U;
    }
    mUploadedVertexCount = total;

    if (total == 0)
    {
        return true;
    }

    const quint32 needBytes = quint32(total * sizeof(MapVertex));
    if (!mVertexBuffer || needBytes > mVertexCapacity)
    {
        // Grow with headroom so a pan that adds one tile does not reallocate a
        // ten megabyte buffer every time.
        const quint32 capacity = needBytes + (needBytes / 2);
        mVertexBuffer.reset(
            mRhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, capacity));
        if (!mVertexBuffer->create())
        {
            return false;
        }
        mVertexCapacity = capacity;
    }

    flat.clear();
    flat.reserve(total);
    for (const GpuBatch& batch : batches)
    {
        if (!batch.geometry)
        {
            continue;
        }
        flat.insert(flat.end(), batch.geometry->vertices.begin(), batch.geometry->vertices.end());
    }

    return true;
}

const QImage& GpuRenderer::render(const Projection& projection,
                                  const std::vector<GpuBatch>& requested, const MapStyle_t& style,
                                  const QColor& background)
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
    mStats.devicePixelRatio = ratio;

    // Truncated rather than grown, because the uniform buffer is fixed -- see
    // kMaxTilesPerFrame. Projection::visibleTiles() orders centre-outward, so
    // what is dropped is what is furthest from where the driver is looking.
    // Logged once rather than per frame: at 60 Hz the warning would be the
    // problem.
    std::vector<GpuBatch> clamped;
    if (requested.size() > kMaxTilesPerFrame)
    {
        if (!mWarnedTileLimit)
        {
            mWarnedTileLimit = true;
            SPDLOG_WARN("[map] {} tiles in one frame; drawing the nearest {}", requested.size(),
                        kMaxTilesPerFrame);
        }
        clamped.assign(requested.begin(), requested.begin() + kMaxTilesPerFrame);
    }
    const std::vector<GpuBatch>& batches = clamped.empty() ? requested : clamped;

    // Same camera, same tiles, same style, same viewport -- so the same
    // pixels. Handing back the frame already in hand skips the draw AND the
    // readback, which together are the whole cost.
    FrameKey key;
    key.size = size;
    key.center = projection.camera().center;
    key.zoom = projection.camera().zoom;
    key.bearing = projection.camera().bearing;
    // Times the ratio, because a vertex's halfPx is a LOGICAL pixel count and
    // this frame is drawn in device ones -- without it roads come out a ratio
    // thinner on a HiDPI screen while everything else keeps its width. It also
    // means the ratio is part of the key, along with `size` above.
    key.widthScale =
        widthScaleForZoom(projection.camera().zoom) * float(style.road_width_scale) * float(ratio);
    key.background = background.rgba();
    key.ids.reserve(batches.size());
    key.serials.reserve(batches.size());
    for (const GpuBatch& batch : batches)
    {
        key.ids.push_back(batch.id);
        key.serials.push_back(batch.geometry ? batch.geometry->serial : 0);
    }

    if (mHaveFrame && key == mFrameKey && !mFrame.isNull())
    {
        ++mStats.reused;
        mStats.lastFrameMs = double(timer.nsecsElapsed()) / 1.0e6;
        return mFrame;
    }

    // Prepared, not submitted. The vertices ride in the same resource update
    // batch as the uniforms below -- uploading them used to open and close an
    // offscreen frame of its own, so a frame that brought in a new tile cost
    // two submissions and two GPU waits instead of one.
    std::vector<MapVertex> flat;
    const bool uploading = batchesChanged(batches);
    if (uploading && !prepareUpload(batches, flat))
    {
        return kNull;
    }

    // Per-tile uniforms. A camera move rewrites these 80 bytes per tile and
    // nothing else -- the vertex buffer is untouched, which is the entire
    // reason a pan costs a fraction of a millisecond.
    // Zoom taper times the user's multiplier. Applied here rather than baked
    // into the vertices so that widening every road stays a uniform write and
    // does not re-tessellate the city.
    const float widthScale = key.widthScale;
    std::vector<char> uniforms(std::size_t(mUniformStride) * std::max<std::size_t>(batches.size(), 1),
                               0);
    for (std::size_t i = 0; i < batches.size(); ++i)
    {
        const TileId& id = batches[i].id;
        // Scaled into device pixels, like `size` above: the projection hands
        // back logical ones and the ortho box below is the device-sized frame.
        const ScreenPoint origin = projection.tileOrigin(id);
        const float tileSize = float(projection.tileScreenSize(id.z) * ratio);

        // Screen pixels -> clip, then place the tile: its rotated on-screen
        // origin, the map's rotation, and local [0,1] -> pixels. tileOrigin()
        // already carries the rotation and the date-line wrap, so the rotation
        // here only orients the tile's own axes.
        QMatrix4x4 mvp = mRhi->clipSpaceCorrMatrix();
        // The framebuffer flip, done HERE rather than on the image that comes
        // back. OpenGL reads back bottom-up and everything else top-down; a
        // QImage::flipped() afterwards copies the whole frame to fix it, while
        // swapping the ortho box costs nothing and produces the same pixels.
        // Winding is not a concern -- the pipeline does not cull, because MVT
        // ring order means exterior-or-hole rather than front-or-back.
        if (mYUpInFramebuffer)
        {
            mvp.ortho(0.0f, float(size.width()), 0.0f, float(size.height()), -1.0f, 1.0f);
        }
        else
        {
            mvp.ortho(0.0f, float(size.width()), float(size.height()), 0.0f, -1.0f, 1.0f);
        }
        mvp.translate(float(origin.x * ratio), float(origin.y * ratio));
        if (projection.camera().bearing != 0.0)
        {
            mvp.rotate(float(-projection.camera().bearing), 0.0f, 0.0f, 1.0f);
        }
        mvp.scale(tileSize, tileSize, 1.0f);

        char* slot = uniforms.data() + (std::size_t(mUniformStride) * i);
        std::memcpy(slot, mvp.constData(), 16 * sizeof(float));
        const float px = tileSize;
        std::memcpy(slot + (16 * sizeof(float)), &px, sizeof(float));
        std::memcpy(slot + (17 * sizeof(float)), &widthScale, sizeof(float));
    }

    QRhiCommandBuffer* cb = nullptr;
    if (mRhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
    {
        return kNull;
    }

    QRhiResourceUpdateBatch* updates = mRhi->nextResourceUpdateBatch();
    if (uploading && !flat.empty())
    {
        updates->uploadStaticBuffer(mVertexBuffer.get(), 0,
                                    quint32(flat.size() * sizeof(MapVertex)), flat.data());
        ++mStats.uploads;
    }
    if (!batches.empty())
    {
        updates->updateDynamicBuffer(mUniformBuffer.get(), 0,
                                     quint32(mUniformStride * batches.size()), uniforms.data());
    }

    cb->beginPass(mTarget.get(), background, { 1.0f, 0 }, updates);

    int draws = 0;
    if (!batches.empty() && mUploadedVertexCount > 0)
    {
        cb->setGraphicsPipeline(mPipeline.get());
        cb->setViewport({ 0.0f, 0.0f, float(size.width()), float(size.height()) });
        const QRhiCommandBuffer::VertexInput vertexInput(mVertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &vertexInput);

        // Layer-major across tiles. See the header.
        for (std::size_t li = 0; li < kMapLayerCount; ++li)
        {
            const auto layer = static_cast<MapLayer>(li);
            if (projection.camera().zoom < layerMinZoom(layer, style))
            {
                continue;
            }

            for (std::size_t ti = 0; ti < batches.size(); ++ti)
            {
                // A batch may carry no geometry: the widget puts a tile in the
                // visible set as soon as it is wanted, which is before its
                // reply has arrived.
                if (!batches[ti].geometry)
                {
                    continue;
                }
                const TileGeometry& geometry = *batches[ti].geometry;
                const std::uint32_t count = geometry.layerVertexCount(layer);
                if (count == 0)
                {
                    continue;
                }
                const QRhiCommandBuffer::DynamicOffset offset(
                    0, quint32(mUniformStride * ti));
                cb->setShaderResources(mSrb.get(), 1, &offset);
                cb->draw(count, 1, mTileBaseVertex[ti] + geometry.layerStart[li]);
                ++draws;
            }
        }
    }
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

    mStats.drawCalls = draws;
    mStats.tiles = int(batches.size());
    mStats.vertices = mUploadedVertexCount;
    mStats.lastFrameMs = double(timer.nsecsElapsed()) / 1.0e6;
    return mFrame;
}

} // namespace map_widget
