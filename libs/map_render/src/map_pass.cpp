// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_render/map_pass.h"

#include <rhi/qrhi.h>

#include <QColor>
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

namespace map_render
{
namespace
{

// mat4 (64) + vec2 viewport (8) + three floats (12) + the pad std140 needs to
// put a vec4 back on a 16-byte boundary (4) + the highlight vec4 (16).
// Comfortably inside the 256-byte stride the hardware's uniform alignment
// rounds up to, and inside the 64 KB uniform buffer limit the stricter
// backends impose.
constexpr quint32 kUniformBlockSize = 112;

QShader loadShader(const unsigned char* bytes, std::size_t size)
{
    return QShader::fromSerialized(
        QByteArray(reinterpret_cast<const char*>(bytes), static_cast<qsizetype>(size)));
}

} // namespace

MapPass::MapPass() = default;
MapPass::~MapPass() = default;

void MapPass::setText(std::vector<TextQuad> quads, const QImage& atlasPage, bool atlasDirty)
{
    mTextQuads = std::move(quads);
    if (atlasDirty)
    {
        mAtlasPage = atlasPage;
        mAtlasDirty = true;
    }
}

void MapPass::releaseResources()
{
    // Reverse of creation order: a pipeline references the shader resource
    // bindings and the render pass descriptor, and the bindings reference the
    // uniform buffer.
    mGlyphPipeline.reset();
    mGlyphSrb.reset();
    mGlyphVertexBuffer.reset();
    mGlyphUniform.reset();
    mGlyphTexture.reset();
    mGlyphSampler.reset();
    mHighlightPipeline.reset();
    mPipeline.reset();
    mSrb.reset();
    mUniformBuffer.reset();
    mVertexBuffer.reset();
    mIndexBuffer.reset();

    // The buffers are gone, so nothing is resident any more. Forgetting this
    // would have the next frame draw from block offsets into a buffer that no
    // longer exists and never upload the geometry it thinks is already there.
    mResident.clear();
    mUploadedIds.clear();
    mUploadedSerials.clear();
    mBatchRegions.clear();
    mPendingUploads.clear();
    mVertexArena.reset(0);
    mIndexArena.reset(0);
    mAtlasDirty = !mAtlasPage.isNull();

    mRhi = nullptr;
    mPass = nullptr;
}

bool MapPass::createResources(QRhi* rhi, QRhiRenderPassDescriptor* pass, int sampleCount)
{
    releaseResources();
    if (!rhi || !pass)
    {
        return false;
    }
    mRhi = rhi;
    mPass = pass;
    mSampleCount = sampleCount;
    mStats.sampleCount = sampleCount;

    mUniformStride = std::max<quint32>(quint32(mRhi->ubufAlignment()), kUniformBlockSize);
    // Fixed for the life of the backend, and read once: it decides which way
    // up the ortho box goes, which is per tile per frame.
    mYUpInFramebuffer = mRhi->isYUpInFramebuffer();

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
        pipeline->setRenderPassDescriptor(mPass);
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
        // One mat4: screen pixels -> clip.
        mGlyphUniform.reset(
            mRhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 64));
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
        mGlyphPipeline->setRenderPassDescriptor(mPass);
        if (!mGlyphPipeline->create())
        {
            SPDLOG_ERROR("[map] glyph pipeline could not be created");
            return false;
        }
    }

    return true;
}

std::span<const GpuBatch> MapPass::clampToBudget(std::span<const GpuBatch> requested)
{
    if (requested.size() <= kMaxTilesPerFrame)
    {
        return requested;
    }
    // Logged once rather than per frame: at 60 Hz the warning would be the
    // problem.
    if (!mWarnedTileLimit)
    {
        mWarnedTileLimit = true;
        SPDLOG_WARN("[map] {} tiles in one frame; drawing the nearest {}", requested.size(),
                    kMaxTilesPerFrame);
    }
    return requested.subspan(0, kMaxTilesPerFrame);
}

bool MapPass::batchesChanged(std::span<const GpuBatch> batches) const
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

// Oldest-first reclaim, and only under pressure. Returns false when the arenas
// cannot serve the request even with every tile this frame does not need thrown
// out -- which is fragmentation, and the caller's answer to that is to compact.
bool MapPass::allocateRegion(Region& region)
{
    for (;;)
    {
        region.vertexBase = mVertexArena.allocate(region.vertexCount);
        region.indexBase = mIndexArena.allocate(region.indexCount);
        if (region.vertexBase != BufferArena::kNoBlock &&
            region.indexBase != BufferArena::kNoBlock)
        {
            return true;
        }
        // Hand back whichever half succeeded before evicting, so the freed
        // space is available to the retry and nothing leaks on the way out.
        if (region.vertexBase != BufferArena::kNoBlock)
        {
            mVertexArena.release(region.vertexBase, region.vertexCount);
        }
        if (region.indexBase != BufferArena::kNoBlock)
        {
            mIndexArena.release(region.indexBase, region.indexCount);
        }

        auto oldest = mResident.end();
        for (auto it = mResident.begin(); it != mResident.end(); ++it)
        {
            // Never a tile this frame already placed: evicting one would free
            // a block the draw loop is about to read from.
            if (it->second.lastPlan == mPlanCounter)
            {
                continue;
            }
            if (oldest == mResident.end() || it->second.lastPlan < oldest->second.lastPlan)
            {
                oldest = it;
            }
        }
        if (oldest == mResident.end())
        {
            return false;
        }
        mVertexArena.release(oldest->second.region.vertexBase, oldest->second.region.vertexCount);
        mIndexArena.release(oldest->second.region.indexBase, oldest->second.region.indexCount);
        mResident.erase(oldest);
    }
}

bool MapPass::planResidency(std::span<const GpuBatch> batches)
{
    mPendingUploads.clear();
    mBatchRegions.assign(batches.size(), Region {});
    mUploadedIds.clear();
    mUploadedSerials.clear();
    mUploadedIds.reserve(batches.size());
    mUploadedSerials.reserve(batches.size());

    // What the whole frame needs, so the buffers can be sized before anything
    // is placed. A tile appearing twice -- the same archive tile drawn as both
    // a stand-in and a real tile, say -- is counted twice here and shares one
    // block below, so this over-estimates. Over-estimating is the safe
    // direction: it can only ask for a buffer larger than the frame uses.
    std::uint32_t needVertices = 0;
    std::uint32_t needIndices = 0;
    for (const GpuBatch& batch : batches)
    {
        // Null is a normal state -- a tile that has been asked for but has not
        // arrived. It gets no block and draws nothing, but it still takes a
        // slot in `batches` so the uniform indices stay aligned.
        if (!batch.geometry)
        {
            continue;
        }
        needVertices += static_cast<std::uint32_t>(batch.geometry->vertices.size());
        needIndices += static_cast<std::uint32_t>(batch.geometry->indices.size());
    }

    // Outgrowing a buffer means a new one, and a new one holds nothing -- so
    // every tile has to be placed and sent again. Grown with headroom so a pan
    // that adds one tile does not reallocate ten megabytes every time.
    bool rebuild = false;
    if (!mVertexBuffer || needVertices > mVertexArena.capacity())
    {
        const std::uint32_t capacity = needVertices + (needVertices / 2);
        mVertexBuffer.reset(mRhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::VertexBuffer,
                                            quint32(capacity * sizeof(MapVertex))));
        if (!mVertexBuffer->create())
        {
            return false;
        }
        mVertexArena.reset(capacity);
        rebuild = true;
    }
    if (!mIndexBuffer || needIndices > mIndexArena.capacity())
    {
        const std::uint32_t capacity = needIndices + (needIndices / 2);
        mIndexBuffer.reset(mRhi->newBuffer(QRhiBuffer::Static, QRhiBuffer::IndexBuffer,
                                           quint32(capacity * sizeof(std::uint32_t))));
        if (!mIndexBuffer->create())
        {
            return false;
        }
        mIndexArena.reset(capacity);
        rebuild = true;
    }
    if (rebuild)
    {
        // Both arenas are reset together even when only one buffer was
        // replaced: a Region names a place in each, and half a table is worse
        // than none.
        mResident.clear();
        mVertexArena.reset(mVertexArena.capacity());
        mIndexArena.reset(mIndexArena.capacity());
    }

    ++mPlanCounter;

    // A tile that leaves the visible set is NOT evicted. Measured: the drawn
    // set oscillates between four and six tiles as the camera crosses a tile
    // boundary, so a tile that leaves is very likely back within a frame or
    // two, and evicting on departure paid a full re-upload for each round trip.
    // Space is reclaimed lazily instead -- oldest first, and only when an
    // allocation actually cannot be served, which bounds the table to what the
    // buffer holds without needing a size limit of its own.

    // Place everything that is not already down. `compact` is the answer to
    // fragmentation: the arenas have the room but not in one run, so throw the
    // layout away and rebuild it contiguously. That is exactly the cost every
    // upload used to pay, which is what bounds the worst case to today's.
    bool compact = false;
    for (std::size_t i = 0; i < batches.size(); ++i)
    {
        const GpuBatch& batch = batches[i];
        mUploadedIds.push_back(batch.id);
        mUploadedSerials.push_back(batch.geometry ? batch.geometry->serial : 0);
        if (!batch.geometry || batch.geometry->vertices.empty() ||
            batch.geometry->indices.empty())
        {
            continue;
        }
        if (compact)
        {
            continue;
        }

        const auto found = mResident.find(batch.geometry->serial);
        if (found != mResident.end())
        {
            found->second.lastPlan = mPlanCounter;
            mBatchRegions[i] = found->second.region;
            continue;
        }

        Region region;
        region.vertexCount = static_cast<std::uint32_t>(batch.geometry->vertices.size());
        region.indexCount = static_cast<std::uint32_t>(batch.geometry->indices.size());
        if (!allocateRegion(region))
        {
            compact = true;
            continue;
        }
        mResident.emplace(batch.geometry->serial, Resident { region, mPlanCounter });
        mBatchRegions[i] = region;
        mPendingUploads.push_back(PendingUpload { batch.geometry->vertices.data(),
                                                  batch.geometry->indices.data(), region });
    }

    if (compact)
    {
        ++mStats.compactions;
        mResident.clear();
        mVertexArena.reset(mVertexArena.capacity());
        mIndexArena.reset(mIndexArena.capacity());
        mPendingUploads.clear();
        for (std::size_t i = 0; i < batches.size(); ++i)
        {
            const GpuBatch& batch = batches[i];
            mBatchRegions[i] = Region {};
            if (!batch.geometry || batch.geometry->vertices.empty() ||
                batch.geometry->indices.empty())
            {
                continue;
            }
            const auto found = mResident.find(batch.geometry->serial);
            if (found != mResident.end())
            {
                mBatchRegions[i] = found->second.region;
                continue;
            }
            Region region;
            region.vertexCount = static_cast<std::uint32_t>(batch.geometry->vertices.size());
            region.indexCount = static_cast<std::uint32_t>(batch.geometry->indices.size());
            region.vertexBase = mVertexArena.allocate(region.vertexCount);
            region.indexBase = mIndexArena.allocate(region.indexCount);
            // Cannot fail: the buffers were sized above against the same
            // (over-estimated) total, and an arena reset to its capacity has
            // one run that long.
            if (region.vertexBase == BufferArena::kNoBlock ||
                region.indexBase == BufferArena::kNoBlock)
            {
                SPDLOG_ERROR("[map] tile geometry did not fit a freshly compacted buffer");
                return false;
            }
            mResident.emplace(batch.geometry->serial, Resident { region, mPlanCounter });
            mBatchRegions[i] = region;
            mPendingUploads.push_back(PendingUpload { batch.geometry->vertices.data(),
                                                      batch.geometry->indices.data(), region });
        }
    }

    mStats.vertices = mVertexArena.used();
    mStats.indices = mIndexArena.used();
    return true;
}

QMatrix4x4 MapPass::screenToClip(const QSize& size) const
{
    // Two independent per-backend facts, and both have to be applied.
    //
    // clipSpaceCorrMatrix() carries the backend's depth convention and, on
    // Vulkan alone, a Y negation -- Vulkan is the only backend whose NDC Y runs
    // down. isYUpInFramebuffer() is a different question: which end of the
    // image row zero is, true on OpenGL and false everywhere else. A pass that
    // consults only the second agrees with Metal and OpenGL and comes out
    // mirrored on Vulkan, which is why both live here and nowhere else.
    //
    // The framebuffer flip is folded into the ortho box rather than applied to
    // the image afterwards: a QImage::flipped() copies the whole frame, while
    // swapping the box costs nothing and produces the same pixels. Winding is
    // not a concern -- the pipeline does not cull, because MVT ring order means
    // exterior-or-hole rather than front-or-back.
    QMatrix4x4 m = mRhi->clipSpaceCorrMatrix();
    if (mYUpInFramebuffer)
    {
        m.ortho(0.0F, float(size.width()), 0.0F, float(size.height()), -1.0F, 1.0F);
    }
    else
    {
        m.ortho(0.0F, float(size.width()), float(size.height()), 0.0F, -1.0F, 1.0F);
    }
    return m;
}

bool MapPass::prepare(const Frame& frame, QRhiResourceUpdateBatch* updates)
{
    if (!mPipeline || !updates)
    {
        return false;
    }
    const Projection& projection = *frame.projection;
    const MapStyle_t& style = *frame.style;
    const Highlight& highlight = *frame.highlight;
    const std::span<const GpuBatch> batches = frame.batches;
    const double ratio = frame.ratio;
    const QSize size = frame.sizePx;

    // batchesChanged() is kept in front of the residency pass purely as the
    // fast path: the overwhelmingly common frame moves the camera over an
    // unchanged tile set, and that frame should not walk a hash table at all.
    if (batchesChanged(batches))
    {
        if (!planResidency(batches))
        {
            return false;
        }
    }
    else
    {
        mPendingUploads.clear();
    }

    // Per-tile uniforms. A camera move rewrites these 80 bytes per tile and
    // nothing else -- the vertex buffer is untouched, which is the entire
    // reason a pan costs a fraction of a millisecond.
    // Zoom taper times the user's multiplier. Applied here rather than baked
    // into the vertices so that widening every road stays a uniform write and
    // does not re-tessellate the city.
    const float widthScale = widthScaleForZoom(projection.camera().zoom) *
                             float(style.road_width_scale) * float(ratio);

    // The tilt, one matrix for the whole frame, in DEVICE pixels -- the same
    // space the flat placement below lands in. Rows above the pivot recede by
    // the perspective divide; at pitch 0 this is the identity and the frame is
    // bit-identical to the untilted path. Built by hand because the projective
    // row is w = f - y*sin(p) about the pivot, which no QMatrix4x4 helper
    // composes directly.
    QMatrix4x4 tilt;
    if (projection.pitched())
    {
        const float f = float(projection.focalPixels() * ratio);
        const float sinP = float(projection.pitchSin());
        const float cosP = float(projection.pitchCos());
        const float cx = float((projection.viewportWidth() / 2.0) * ratio);
        const float cy = float((projection.viewportHeight() / 2.0) * ratio);
        QMatrix4x4 about; // takes (u, v) about the pivot to (u*f, v*cos*f, ., f - v*sin)
        about.setRow(0, QVector4D(f, 0.0F, 0.0F, 0.0F));
        about.setRow(1, QVector4D(0.0F, cosP * f, 0.0F, 0.0F));
        about.setRow(2, QVector4D(0.0F, 0.0F, 1.0F, 0.0F));
        about.setRow(3, QVector4D(0.0F, -sinP, 0.0F, f));
        QMatrix4x4 toPivot;
        toPivot.translate(-cx, -cy);
        QMatrix4x4 fromPivot;
        fromPivot.translate(cx, cy);
        // The perspective divide leaves x/w, so the pivot restore must itself
        // scale with w -- fold it in BEFORE the divide by multiplying the
        // matrices in pivot order.
        tilt = fromPivot * about * toPivot;
    }

    std::vector<char>& uniforms = mUniformScratch;
    uniforms.assign(std::size_t(mUniformStride) * std::max<std::size_t>(batches.size(), 1), 0);
    for (std::size_t i = 0; i < batches.size(); ++i)
    {
        const TileId& id = batches[i].id;
        // Scaled into device pixels, like `size` above: the projection hands
        // back logical ones and the ortho box below is the device-sized frame.
        const ScreenPoint origin = projection.tileOrigin(id);
        const float tileSize = float(projection.tileScreenSize(id.z) * ratio);

        // Screen pixels -> clip, then the frame's tilt, then place the tile:
        // its rotated on-screen origin, the map's rotation, and local [0,1] ->
        // pixels. tileOrigin() is FLAT on purpose -- the tilt here is the only
        // application of the pitch, so nothing tilts twice.
        QMatrix4x4 mvp = screenToClip(size);
        if (projection.pitched())
        {
            mvp = mvp * tilt;
        }
        mvp.translate(float(origin.x * ratio), float(origin.y * ratio));
        if (projection.camera().bearing != 0.0)
        {
            mvp.rotate(float(-projection.camera().bearing), 0.0f, 0.0f, 1.0f);
        }
        mvp.scale(tileSize, tileSize, 1.0f);

        char* slot = uniforms.data() + (std::size_t(mUniformStride) * i);
        std::memcpy(slot, mvp.constData(), 16 * sizeof(float));

        // Floats 16 and 17: the frame in DEVICE pixels. The vertex stage
        // widens lines AFTER the matrix and needs it to turn an offset in
        // pixels into one in NDC -- see expandToScreenWidth() in map.vert.
        // The same for every tile; it rides the per-tile block because there
        // is only one block.
        const std::array<float, 2> viewportPx { float(size.width()), float(size.height()) };
        std::memcpy(slot + (16 * sizeof(float)), viewportPx.data(), 2 * sizeof(float));
        std::memcpy(slot + (18 * sizeof(float)), &widthScale, sizeof(float));
        // Floats 19 and 20: this tile's crossfade and the highlight's extra
        // half-width. Then the highlight colour at float 24 -- std140 aligns a
        // vec4 to 16 bytes, and floats 21..23 are the pad that gets it there.
        const float fadeAlpha = float(quantizeAlpha(batches[i].alpha)) / 255.0F;
        std::memcpy(slot + (19 * sizeof(float)), &fadeAlpha, sizeof(float));
        std::memcpy(slot + (20 * sizeof(float)), &highlight.extraHalfPx, sizeof(float));
        const std::array<float, 4> highlightRgba {
            float(highlight.colour.redF()), float(highlight.colour.greenF()),
            float(highlight.colour.blueF()), float(highlight.colour.alphaF())
        };
        std::memcpy(slot + (24 * sizeof(float)), highlightRgba.data(), 4 * sizeof(float));
    }

    // One pair of regions per tile that is NOT already on the GPU. QRhi copies
    // out of these pointers as the batch is built, so pointing straight at each
    // tile's own vectors is safe and saves flattening the whole frame into a
    // scratch array first -- which used to be the larger half of an upload.
    for (const PendingUpload& pending : mPendingUploads)
    {
        updates->uploadStaticBuffer(
            mVertexBuffer.get(), quint32(pending.region.vertexBase * sizeof(MapVertex)),
            quint32(pending.region.vertexCount * sizeof(MapVertex)), pending.vertexData);
        updates->uploadStaticBuffer(
            mIndexBuffer.get(), quint32(pending.region.indexBase * sizeof(std::uint32_t)),
            quint32(pending.region.indexCount * sizeof(std::uint32_t)), pending.indexData);
        mStats.uploadedVertices += pending.region.vertexCount;
    }
    if (!mPendingUploads.empty())
    {
        ++mStats.uploads;
    }
    if (!batches.empty())
    {
        updates->updateDynamicBuffer(mUniformBuffer.get(), 0,
                                     quint32(mUniformStride * batches.size()), uniforms.data());
    }

    // ---- text, prepared alongside the tiles ------------------------------
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
        mGlyphVertexCount = quint32(mTextQuads.size() * 6);

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
        // The SAME matrix the map pass starts from, so text cannot land a
        // different way up from the geometry under it. The quads are already in
        // device pixels, so there is nothing further to place.
        const QMatrix4x4 textClip = screenToClip(size);
        updates->updateDynamicBuffer(mGlyphUniform.get(), 0, 64, textClip.constData());
    }

    mStats.tiles = int(batches.size());
    return true;
}

void MapPass::record(const Frame& frame, QRhiCommandBuffer* cb)
{
    if (!mPipeline || !cb)
    {
        return;
    }
    const Projection& projection = *frame.projection;
    const MapStyle_t& style = *frame.style;
    const Highlight& highlight = *frame.highlight;
    const std::span<const GpuBatch> batches = frame.batches;
    const QSize size = frame.sizePx;

    int draws = 0;
    if (!batches.empty() && mVertexArena.used() > 0 && mIndexArena.used() > 0)
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
                cb->drawIndexed(count, 1,
                                mBatchRegions[ti].indexBase + geometry.layerIndexStart[li],
                                qint32(mBatchRegions[ti].vertexBase));
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
                                        mBatchRegions[ti].indexBase + range.indexStart,
                                        qint32(mBatchRegions[ti].vertexBase));
                        ++draws;
                    });
                }
            }
        }
    }
    // Text last, inside the same pass: a second pass would have to clear or
    // load the target, and drawing here is both cheaper and correctly ordered
    // over every tile.
    if (mGlyphVertexCount > 0)
    {
        cb->setGraphicsPipeline(mGlyphPipeline.get());
        cb->setViewport({ 0.0f, 0.0f, float(size.width()), float(size.height()) });
        cb->setShaderResources(mGlyphSrb.get());
        const QRhiCommandBuffer::VertexInput glyphInput(mGlyphVertexBuffer.get(), 0);
        cb->setVertexInput(0, 1, &glyphInput);
        cb->draw(mGlyphVertexCount);
        ++draws;
    }

    mStats.drawCalls = draws;
}

} // namespace map_render
