// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/gpu_renderer.h"

#include <rhi/qrhi.h>
// The InitParams structs live here rather than in qrhi.h, and each is behind the
// feature test for its own backend.
#include <rhi/qrhi_platform.h>

#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QColor>
#include <QElapsedTimer>
#include <QMatrix4x4>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "mapVert_qsb.h"
#include "mapFrag_qsb.h"
#include "mapHighlightVert_qsb.h"
#include "mapHighlightFrag_qsb.h"
#include "glyphVert_qsb.h"
#include "glyphFrag_qsb.h"

#include <QPainter>

namespace map_widget
{
namespace
{

// mat4 + two floats + vec2 pad + the highlight vec4. Comfortably inside the
// 256-byte stride the hardware's uniform alignment rounds up to, and inside
// the 64 KB uniform buffer limit the stricter backends impose.
constexpr quint32 kUniformBlockSize = 96;

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

void GpuRenderer::setText(std::vector<TextQuad> quads, const QImage& atlasPage,
                          bool atlasDirty)
{
    mTextQuads = std::move(quads);
    if (atlasDirty)
    {
        mAtlasPage = atlasPage;
        mAtlasDirty = true;
    }
}

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

    mUniformStride = std::max<quint32>(quint32(mRhi->ubufAlignment()), kUniformBlockSize);
    // Fixed for the life of the backend, and read once: it decides which way
    // up the ortho box goes, which is per tile per frame.
    mYUpInFramebuffer = mRhi->isYUpInFramebuffer();

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
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
        mUniformBuffer.get(), kUniformBlockSize) });
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
    mHighlightPipeline.reset();
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

    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

    QRhiVertexInputLayout layout;
    layout.setBindings({ { sizeof(MapVertex) } });
    layout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, offsetof(MapVertex, x) },
        { 0, 1, QRhiVertexInputAttribute::Float2, offsetof(MapVertex, nx) },
        { 0, 2, QRhiVertexInputAttribute::Float, offsetof(MapVertex, halfPx) },
        { 0, 3, QRhiVertexInputAttribute::Float4, offsetof(MapVertex, r) },
    });

    // Both pipelines are the same in everything but their shader stages: same
    // blend, same vertex buffer, same shader resource bindings, and no culling
    // -- MVT ring winding says exterior-or-hole, not front-or-back, and earcut
    // emits whatever order the ear clipping produced.
    const auto makePipeline = [&](QShader vert, QShader frag) {
        std::unique_ptr<QRhiGraphicsPipeline> pipeline(mRhi->newGraphicsPipeline());
        pipeline->setTargetBlends({ blend });
        pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        pipeline->setCullMode(QRhiGraphicsPipeline::None);
        pipeline->setSampleCount(mSampleCount);
        pipeline->setShaderStages({ { QRhiShaderStage::Vertex, std::move(vert) },
                                    { QRhiShaderStage::Fragment, std::move(frag) } });
        pipeline->setVertexInputLayout(layout);
        pipeline->setShaderResourceBindings(mSrb.get());
        pipeline->setRenderPassDescriptor(mPass.get());
        if (!pipeline->create())
        {
            pipeline.reset();
        }
        return pipeline;
    };

    mPipeline = makePipeline(loadShader(map_shaders::mapVert, map_shaders::mapVertSize),
                             loadShader(map_shaders::mapFrag, map_shaders::mapFragSize));
    if (!mPipeline)
    {
        SPDLOG_ERROR("[map] GPU pipeline could not be created");
        return false;
    }

    // The highlight pipeline draws the map's own road geometry again, on top:
    // its vertex stage widens the line by the uniform's extraHalfPx and its
    // fragment stage recolours from the uniform. Same bindings, so it reads
    // the same per-tile uniform slot as the base pass.
    mHighlightPipeline =
        makePipeline(loadShader(map_shaders::mapHighlightVert, map_shaders::mapHighlightVertSize),
                     loadShader(map_shaders::mapHighlightFrag, map_shaders::mapHighlightFragSize));
    if (!mHighlightPipeline)
    {
        SPDLOG_ERROR("[map] GPU highlight pipeline could not be created");
        return false;
    }

    // ---- PROTOTYPE glyph pipeline ---------------------------------------
    if (!mGlyphSampler)
    {
        mGlyphSampler.reset(mRhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear,
                                             QRhiSampler::None, QRhiSampler::ClampToEdge,
                                             QRhiSampler::ClampToEdge));
        if (!mGlyphSampler->create())
        {
            SPDLOG_ERROR("[map] glyph sampler could not be created");
            return false;
        }
    }
    if (!mGlyphTexture)
    {
        mGlyphTexture.reset(mRhi->newTexture(QRhiTexture::RGBA8, QSize(1024, 1024), 1));
        if (!mGlyphTexture->create())
        {
            SPDLOG_ERROR("[map] glyph atlas texture could not be created");
            return false;
        }
    }
    if (!mGlyphUniform)
    {
        mGlyphUniform.reset(
            mRhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 16));
        if (!mGlyphUniform->create())
        {
            return false;
        }
    }
    if (!mGlyphSrb)
    {
        mGlyphSrb.reset(mRhi->newShaderResourceBindings());
        mGlyphSrb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage, mGlyphUniform.get()),
            QRhiShaderResourceBinding::sampledTexture(
                1, QRhiShaderResourceBinding::FragmentStage, mGlyphTexture.get(),
                mGlyphSampler.get()),
        });
        if (!mGlyphSrb->create())
        {
            return false;
        }
    }

    QRhiVertexInputLayout glyphLayout;
    glyphLayout.setBindings({ { 4 * sizeof(float) } });
    glyphLayout.setAttributes({
        { 0, 0, QRhiVertexInputAttribute::Float2, 0 },
        { 0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float) },
    });

    mGlyphPipeline.reset(mRhi->newGraphicsPipeline());
    {
        // The atlas holds premultiplied pixels, so the blend is One /
        // OneMinusSrcAlpha rather than SrcAlpha -- multiplying by alpha twice
        // is what makes atlas text look thin and grey.
        QRhiGraphicsPipeline::TargetBlend glyphBlend;
        glyphBlend.enable = true;
        glyphBlend.srcColor = QRhiGraphicsPipeline::One;
        glyphBlend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        glyphBlend.srcAlpha = QRhiGraphicsPipeline::One;
        glyphBlend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
        mGlyphPipeline->setTargetBlends({ glyphBlend });
        mGlyphPipeline->setTopology(QRhiGraphicsPipeline::Triangles);
        mGlyphPipeline->setCullMode(QRhiGraphicsPipeline::None);
        mGlyphPipeline->setSampleCount(mSampleCount);
        mGlyphPipeline->setShaderStages(
            { { QRhiShaderStage::Vertex,
                loadShader(map_shaders::glyphVert, map_shaders::glyphVertSize) },
              { QRhiShaderStage::Fragment,
                loadShader(map_shaders::glyphFrag, map_shaders::glyphFragSize) } });
        mGlyphPipeline->setVertexInputLayout(glyphLayout);
        mGlyphPipeline->setShaderResourceBindings(mGlyphSrb.get());
        mGlyphPipeline->setRenderPassDescriptor(mPass.get());
        if (!mGlyphPipeline->create())
        {
            SPDLOG_ERROR("[map] glyph pipeline could not be created");
            return false;
        }
    }

    mSize = size;
    return true;
}

bool GpuRenderer::batchesChanged(std::span<const GpuBatch> batches) const
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

bool GpuRenderer::prepareUpload(std::span<const GpuBatch> batches,
                                std::vector<MapVertex>& flat,
                                std::vector<std::uint32_t>& flatIndices)
{
    mTileBaseVertex.clear();
    mTileBaseIndex.clear();
    mUploadedIds.clear();
    mUploadedSerials.clear();

    std::uint32_t total = 0;
    std::uint32_t totalIndices = 0;
    for (const GpuBatch& batch : batches)
    {
        mTileBaseVertex.push_back(total);
        mTileBaseIndex.push_back(totalIndices);
        mUploadedIds.push_back(batch.id);
        mUploadedSerials.push_back(batch.geometry ? batch.geometry->serial : 0);
        // Null is a normal state -- a tile that has been asked for but has not
        // arrived. It still takes a slot, so the base-vertex and uniform indices
        // stay aligned with `batches`.
        total += batch.geometry
                     ? static_cast<std::uint32_t>(batch.geometry->vertices.size())
                     : 0U;
        totalIndices += batch.geometry
                            ? static_cast<std::uint32_t>(batch.geometry->indices.size())
                            : 0U;
    }
    mUploadedVertexCount = total;
    mUploadedIndexCount = totalIndices;

    if (total == 0 || totalIndices == 0)
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

    const quint32 needIndexBytes = quint32(totalIndices * sizeof(std::uint32_t));
    if (!mIndexBuffer || needIndexBytes > mIndexCapacity)
    {
        const quint32 capacity = needIndexBytes + (needIndexBytes / 2);
        mIndexBuffer.reset(
            mRhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::IndexBuffer, capacity));
        if (!mIndexBuffer->create())
        {
            return false;
        }
        mIndexCapacity = capacity;
    }

    flat.clear();
    flat.reserve(total);
    flatIndices.clear();
    flatIndices.reserve(totalIndices);
    for (const GpuBatch& batch : batches)
    {
        if (!batch.geometry)
        {
            continue;
        }
        flat.insert(flat.end(), batch.geometry->vertices.begin(), batch.geometry->vertices.end());
        // Copied VERBATIM: they are tile-local, and drawIndexed() is handed the
        // tile's base vertex to add. Rewriting them here would make a tile's
        // indices depend on where in the batch it landed, which is exactly what
        // batchesChanged() relies on NOT being true.
        flatIndices.insert(flatIndices.end(), batch.geometry->indices.begin(),
                           batch.geometry->indices.end());
    }

    return true;
}

const QImage& GpuRenderer::render(const Projection& projection,
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
    mStats.devicePixelRatio = ratio;

    // Truncated rather than grown, because the uniform buffer is fixed -- see
    // kMaxTilesPerFrame. Projection::visibleTiles() orders centre-outward, so
    // what is dropped is what is furthest from where the driver is looking.
    // Logged once rather than per frame: at 60 Hz the warning would be the
    // problem.
    if (requested.size() > kMaxTilesPerFrame && !mWarnedTileLimit)
    {
        mWarnedTileLimit = true;
        SPDLOG_WARN("[map] {} tiles in one frame; drawing the nearest {}", requested.size(),
                    kMaxTilesPerFrame);
    }
    // A view, not a copy: the old copy-and-rebind idiom only worked because
    // kMaxTilesPerFrame is nonzero, and cost a vector of shared_ptrs per
    // clamped frame besides.
    const std::span<const GpuBatch> batches(
        requested.data(), std::min(requested.size(), std::size_t(kMaxTilesPerFrame)));

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
    const auto textMatchesKey = [&]() { return mFrameKey.text == mTextQuads; };

    if (mHaveFrame && !mFrame.isNull() && key.scalarsEqual(mFrameKey) && batchesMatchKey() &&
        textMatchesKey())
    {
        ++mStats.reused;
        mStats.lastFrameMs = double(timer.nsecsElapsed()) / 1.0e6;
        return mFrame;
    }

    // A miss: refill the stored key's vectors in place, keeping their
    // capacity across frames.
    key.highlightIds = std::move(mFrameKey.highlightIds);
    key.highlightIds.assign(highlight.osmWayIds.begin(), highlight.osmWayIds.end());
    key.text = mTextQuads;
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

    // Prepared, not submitted. The vertices ride in the same resource update
    // batch as the uniforms below -- uploading them used to open and close an
    // offscreen frame of its own, so a frame that brought in a new tile cost
    // two submissions and two GPU waits instead of one.
    std::vector<MapVertex>& flat = mFlatScratch;
    std::vector<std::uint32_t>& flatIndices = mFlatIndexScratch;
    flat.clear();
    flatIndices.clear();
    const bool uploading = batchesChanged(batches);
    if (uploading && !prepareUpload(batches, flat, flatIndices))
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
    std::vector<char>& uniforms = mUniformScratch;
    uniforms.assign(std::size_t(mUniformStride) * std::max<std::size_t>(batches.size(), 1), 0);
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
        // Floats 18 and 19: this tile's crossfade and the highlight's extra
        // half-width. Then the highlight colour at float 20 -- std140 aligns a
        // vec4 to 16 bytes, which is exactly where the two floats leave it.
        const float fadeAlpha = float(quantizeAlpha(batches[i].alpha)) / 255.0F;
        std::memcpy(slot + (18 * sizeof(float)), &fadeAlpha, sizeof(float));
        std::memcpy(slot + (19 * sizeof(float)), &highlight.extraHalfPx, sizeof(float));
        const std::array<float, 4> highlightRgba {
            float(highlight.colour.redF()), float(highlight.colour.greenF()),
            float(highlight.colour.blueF()), float(highlight.colour.alphaF())
        };
        std::memcpy(slot + (20 * sizeof(float)), highlightRgba.data(), 4 * sizeof(float));
    }

    QRhiCommandBuffer* cb = nullptr;
    if (mRhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess)
    {
        return kNull;
    }

    QRhiResourceUpdateBatch* updates = mRhi->nextResourceUpdateBatch();
    if (uploading && !flat.empty() && !flatIndices.empty())
    {
        updates->uploadStaticBuffer(mVertexBuffer.get(), 0,
                                    quint32(flat.size() * sizeof(MapVertex)), flat.data());
        updates->uploadStaticBuffer(mIndexBuffer.get(), 0,
                                    quint32(flatIndices.size() * sizeof(std::uint32_t)),
                                    flatIndices.data());
        ++mStats.uploads;
    }
    if (!batches.empty())
    {
        updates->updateDynamicBuffer(mUniformBuffer.get(), 0,
                                     quint32(mUniformStride * batches.size()), uniforms.data());
    }

    // ---- text, prepared alongside the tiles ------------------------------
    quint32 glyphVertexCount = 0;
    if (!mTextQuads.empty() && mGlyphPipeline)
    {
        mGlyphScratch.clear();
        mGlyphScratch.reserve(mTextQuads.size() * 6 * 4);
        for (const TextQuad& q : mTextQuads)
        {
            const QPointF uvs[4] = { q.uv.topLeft(), q.uv.topRight(), q.uv.bottomRight(),
                                     q.uv.bottomLeft() };
            const int order[6] = { 0, 1, 2, 0, 2, 3 };
            for (const int i : order)
            {
                mGlyphScratch.push_back(float(q.corners[i].x()));
                mGlyphScratch.push_back(float(q.corners[i].y()));
                mGlyphScratch.push_back(float(uvs[i].x()));
                mGlyphScratch.push_back(float(uvs[i].y()));
            }
        }
        glyphVertexCount = quint32(mTextQuads.size() * 6);

        const quint32 bytes = quint32(mGlyphScratch.size() * sizeof(float));
        if (!mGlyphVertexBuffer || mGlyphVertexBuffer->size() < bytes)
        {
            mGlyphVertexBuffer.reset(mRhi->newBuffer(QRhiBuffer::Dynamic,
                                                     QRhiBuffer::VertexBuffer,
                                                     std::max<quint32>(bytes, 256u * 1024u)));
            mGlyphVertexBuffer->create();
        }
        if (mAtlasDirty && !mAtlasPage.isNull())
        {
            // The whole page, once, whenever it grew. A glyph set is an
            // alphabet, so this settles in the first frames of a drive and
            // then never fires again.
            updates->uploadTexture(mGlyphTexture.get(), mAtlasPage);
            mAtlasDirty = false;
        }
        updates->updateDynamicBuffer(mGlyphVertexBuffer.get(), 0, bytes, mGlyphScratch.data());
        // Screen pixels -> clip. The Y term matches the ortho box the map pass
        // builds, so text lands the same way up as the geometry under it.
        const float uniform[4] = { float(size.width()), float(size.height()),
                                   mYUpInFramebuffer ? 1.0F : -1.0F, 0.0F };
        updates->updateDynamicBuffer(mGlyphUniform.get(), 0, 16, uniform);
    }

    cb->beginPass(mTarget.get(), background, { 1.0f, 0 }, updates);

    int draws = 0;
    if (!batches.empty() && mUploadedVertexCount > 0 && mUploadedIndexCount > 0)
    {
        cb->setGraphicsPipeline(mPipeline.get());
        cb->setViewport({ 0.0f, 0.0f, float(size.width()), float(size.height()) });
        const QRhiCommandBuffer::VertexInput vertexInput(mVertexBuffer.get(), 0);
        // 32-bit indices. A single tile stays far under 65 536 vertices, but the
        // buffer is SHARED across every tile in the frame and the index is into
        // that -- so 16-bit would cap a frame at 65 536 vertices in total, which
        // is one dense z14 tile.
        cb->setVertexInput(0, 1, &vertexInput, mIndexBuffer.get(), 0,
                           QRhiCommandBuffer::IndexUInt32);

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
                const std::uint32_t count = geometry.layerIndexCount(layer);
                if (count == 0)
                {
                    continue;
                }
                const QRhiCommandBuffer::DynamicOffset offset(
                    0, quint32(mUniformStride * ti));
                cb->setShaderResources(mSrb.get(), 1, &offset);
                // The tile's base vertex is the LAST argument, not folded into
                // the indices: that is what lets a tile's geometry be uploaded
                // unchanged wherever it lands in the shared buffer.
                cb->drawIndexed(count, 1, mTileBaseIndex[ti] + geometry.layerIndexStart[li], 
                                qint32(mTileBaseVertex[ti]));
                ++draws;
            }
        }

        // The highlight pass, AFTER every layer: the route and the road the
        // vehicle is on have to sit over the map, not inside its draw order.
        //
        // The same vertex buffer, the same per-tile uniform and the same line
        // expansion -- only the fragment stage differs, so this is the map's
        // own geometry recoloured rather than a second polyline that can drift
        // off the road it is meant to be on.
        if (!highlight.empty())
        {
            cb->setGraphicsPipeline(mHighlightPipeline.get());
            for (std::size_t ti = 0; ti < batches.size(); ++ti)
            {
                if (!batches[ti].geometry || batches[ti].geometry->roads.empty())
                {
                    continue;
                }
                const TileGeometry& geometry = *batches[ti].geometry;
                const QRhiCommandBuffer::DynamicOffset offset(0, quint32(mUniformStride * ti));
                bool bound = false;
                for (const std::uint64_t osmWayId : highlight.osmWayIds)
                {
                    geometry.forEachRoadRange(osmWayId, [&](const FeatureRange& range) {
                        if (!bound)
                        {
                            cb->setShaderResources(mSrb.get(), 1, &offset);
                            bound = true;
                        }
                        cb->drawIndexed(range.indexCount, 1,
                                        mTileBaseIndex[ti] + range.indexStart,
                                        qint32(mTileBaseVertex[ti]));
                        ++draws;
                    });
                }
            }
        }
    }
    // Text last, inside the same pass: a second pass would have to clear or
    // load the target, and drawing here is both cheaper and correctly ordered
    // over every tile.
    if (glyphVertexCount > 0)
    {
        cb->setGraphicsPipeline(mGlyphPipeline.get());
        cb->setViewport({ 0.0f, 0.0f, float(size.width()), float(size.height()) });
        cb->setShaderResources(mGlyphSrb.get());
        const QRhiCommandBuffer::VertexInput glyphInput(mGlyphVertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &glyphInput);
        cb->draw(glyphVertexCount);
        ++draws;
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
    mStats.indices = mUploadedIndexCount;
    mStats.lastFrameMs = double(timer.nsecsElapsed()) / 1.0e6;
    return mFrame;
}

} // namespace map_widget
