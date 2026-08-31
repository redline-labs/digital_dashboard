// SPDX-License-Identifier: GPL-3.0-or-later
//
// The map's GPU pass: every resource and every draw call, and NOTHING about
// where the pixels end up.
//
// It was split out of OffscreenRenderer for one reason. That class drives QRhi
// against a texture it allocates itself, reads the result back into a QImage
// and lets the widget blit it -- which is the only thing that works under
// QT_QPA_PLATFORM=offscreen, and so the only thing that works for
// ui_screenshot and for every `gui` test. On a real display it is also the
// expensive thing: MEASURED at 660x640 and a device pixel ratio of 2, the pass
// records in 0.04 ms and the readback and blit cost 1.03 ms; at 2560x1440 it
// is 0.21 ms against 8.77 ms. Nearly the whole cost of the map is the round
// trip, not the drawing.
//
// So there are two hosts, and they share this:
//
//   * OffscreenRenderer -- offscreen texture, synchronous readback, QImage
//     out. The only one that comes up headless, and what agent control, the
//     bench and the tests all use.
//   * RhiWidgetHost (libs/map_surface) -- a QRhiWidget, drawing straight into
//     the widget's own swapchain with no readback and no blit at all. Faster
//     by the numbers above, and impossible to run headless. MapSurface picks
//     between the two.
//
// Everything that decides what a pixel looks like is HERE, so the two hosts
// cannot drift into drawing different maps. What each host owns is only its
// target and its frame lifecycle -- which is why the fast path, the one CI
// cannot exercise, is thin enough to read in one sitting.
#ifndef MAP_PASS_H
#define MAP_PASS_H

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include <QColor>
#include <QImage>
#include <QMatrix4x4>
#include <QSize>

#include "map_render/buffer_arena.h"
#include "map_render/projection.h"
#include "map_render/style.h"
#include "map_render/tessellator.h"
#include "map_render/text_quad.h"

class QRhi;
class QRhiBuffer;
class QRhiCommandBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiResourceUpdateBatch;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;

namespace map_render
{

// One tile's worth of geometry, with the id that says where to put it.
struct GpuBatch
{
    TileId id;
    std::shared_ptr<const TileGeometry> geometry;
    // This tile's crossfade, 1 once settled. Rides the per-tile uniform (the
    // shader's fadeAlpha), so a fade costs a uniform write and never touches
    // the vertex buffer -- uploads must NOT climb during a fade.
    float alpha { 1.0F };
};

// One quantisation for the fade, used by BOTH the uniform write and the frame
// key. If they rounded differently, a frame could be served from the memo
// while its pixels lag the key by a step -- or re-rendered for a change no
// pixel can show.
inline std::uint8_t quantizeAlpha(float alpha)
{
    const float clamped = alpha < 0.0F ? 0.0F : (alpha > 1.0F ? 1.0F : alpha);
    return static_cast<std::uint8_t>(clamped * 255.0F + 0.5F);
}

class MapPass
{
  public:
    // How many tiles one frame may draw. The uniform buffer holds one block per
    // tile and is allocated ONCE at this size, because the graphics pipeline
    // holds a pointer to the shader resource bindings and the bindings hold a
    // pointer to the buffer -- so growing the buffer later means destroying an
    // object the pipeline still refers to, and the symptom of that is a frame
    // that draws nothing at all while every draw call reports success.
    //
    // A 3840x2160 viewport at 512 px tiles needs 40, and 70 with the prefetch
    // ring, so this is roughly triple the worst real case. At 256 bytes of
    // stride it is 48 KB, which also keeps it under the 64 KB uniform buffer
    // limit that the stricter backends impose.
    //
    // Public because the widget budgets against it: a frame that spends this on
    // stand-in tiles would have its real ones truncated away, and truncation
    // takes the tail.
    static constexpr std::size_t kMaxTilesPerFrame = 192;

    // Roads to draw again, on top, in one colour.
    //
    // OSM way ids, because that is the identifier every one of these arrives
    // as: `map/nearest` answers with `osmWayId`, `map/route` returns segment
    // ids that pack it, and `map_build` stamps it on every tile feature for
    // exactly this join. Sorted, so matching against a tile's own sorted road
    // list is a walk rather than a search per id.
    struct Highlight
    {
        std::vector<std::uint64_t> osmWayIds;
        QColor colour;
        // Added to the road's own half-width, so the highlight reads as a
        // casing around the road rather than replacing it. Zero draws it at
        // exactly the road's width, which mostly disappears.
        float extraHalfPx { 0.0F };

        bool empty() const { return osmWayIds.empty(); }
        bool operator==(const Highlight&) const = default;
    };

    struct Stats
    {
        // Frames served from a host's memo because nothing that affects the
        // image had changed. Climbing while the vehicle moves would mean the
        // key is missing something.
        std::uint64_t reused { 0 };
        int drawCalls { 0 };
        int tiles { 0 };
        std::uint32_t vertices { 0 };
        // Indices resident. Three per triangle, and no longer the same number
        // as `vertices` -- which is the point of indexing.
        std::uint32_t indices { 0 };
        // Wall clock for the host's last frame. On the offscreen host that
        // includes the readback, which waits for the GPU, so it is a real
        // number rather than a submission time.
        double lastFrameMs { 0.0 };
        // How many frames uploaded any geometry at all. Should climb only when
        // the visible tile set or the style changes; if it tracks the frame
        // count, something is invalidating the cache every frame.
        std::uint64_t uploads { 0 };
        // Vertices actually sent across on those frames. THIS is the number
        // that says whether uploads are incremental: with one arena block per
        // tile it tracks the tiles that arrived, and if it tracks `vertices`
        // times `uploads` instead then something is forcing a whole-buffer
        // rebuild.
        std::uint64_t uploadedVertices { 0 };
        // Whole-buffer rebuilds, from fragmentation or from outgrowing the
        // buffer. Each one costs what every upload used to, so a run where this
        // tracks `uploads` has bought nothing.
        std::uint64_t compactions { 0 };
        int sampleCount { 1 };
        // What the last frame was actually rendered at. Normally the screen's
        // ratio; lower when the viewport was wide enough that a device-pixel
        // texture would have exceeded what the backend will allocate, in which
        // case the frame is upscaled to fit the widget. Worth reporting,
        // because the only other evidence of that is a map that looks slightly
        // soft.
        double devicePixelRatio { 1.0 };
    };

    // One frame's worth of what to draw. A view, not an owner -- everything in
    // it lives for the host's call and no longer.
    struct Frame
    {
        const Projection* projection { nullptr };
        std::span<const GpuBatch> batches;
        const MapStyle_t* style { nullptr };
        const Highlight* highlight { nullptr };
        // Device pixels the target actually is, and the ratio that produced it.
        // The pass works entirely in device pixels; the projection is logical,
        // because the marker and track drawn over the map go through QPainter.
        QSize sizePx;
        double ratio { 1.0 };
    };

    MapPass();
    ~MapPass();

    MapPass(const MapPass&) = delete;
    MapPass& operator=(const MapPass&) = delete;

    // Everything that depends on the device and on the target's FORMAT --
    // pipelines, the uniform buffer, the glyph atlas -- but nothing that
    // depends on its SIZE. A host calls this once per (rhi, render pass
    // descriptor, sample count); changing the target's size does not need it,
    // changing its format does.
    bool createResources(QRhi* rhi, QRhiRenderPassDescriptor* pass, int sampleCount);
    // Frees everything createResources() made, so a host can hand back a
    // device. Safe to call when nothing was created.
    void releaseResources();
    bool ready() const { return mPipeline != nullptr; }

    // The frame's text, as quads into `atlasPage`.
    //
    // Drawn last and inside the SAME pass as the tiles, so it lands over every
    // one of them without a second pass having to clear or reload the target.
    // The page is re-uploaded only when `atlasDirty` -- a glyph set is an
    // alphabet, so that settles within the first frames of a drive and then
    // never fires again.
    void setText(std::vector<TextQuad> quads, const QImage& atlasPage, bool atlasDirty);
    const std::vector<TextQuad>& text() const { return mTextQuads; }

    // Queues this frame's uploads and uniform writes into `updates`. Must be
    // called between the host's begin-frame and its begin-pass, because the
    // batch is handed to beginPass().
    bool prepare(const Frame& frame, QRhiResourceUpdateBatch* updates);
    // Records the draw calls into a pass the host has already begun.
    void record(const Frame& frame, QRhiCommandBuffer* cb);

    // True when `batches` differ from what is already on the GPU. Hosts use it
    // to decide whether a frame needs planning at all.
    bool batchesChanged(std::span<const GpuBatch> batches) const;

    // The first kMaxTilesPerFrame of `requested`, warning once if it had to cut.
    //
    // Truncated rather than grown, because the uniform buffer is fixed -- see
    // kMaxTilesPerFrame. Projection::visibleTiles() orders centre-outward, so
    // what is dropped is what is furthest from where the driver is looking.
    // Shared by both hosts so neither can quietly draw a different number of
    // tiles than the other.
    std::span<const GpuBatch> clampToBudget(std::span<const GpuBatch> requested);

    // Device pixels (Y down from the top-left of the image we want back) ->
    // clip, for `size`. EVERY pass must start from this, because two things
    // vary per backend and neither one alone settles which way up a pass comes
    // out: whether the framebuffer's Y runs up, and whether the backend's
    // clipSpaceCorrMatrix() negates Y. Vulkan is the one that does both, so a
    // pass that consults only isYUpInFramebuffer() agrees with Metal and
    // OpenGL and comes out mirrored on Vulkan alone.
    QMatrix4x4 screenToClip(const QSize& size) const;

    int sampleCount() const { return mSampleCount; }
    Stats& stats() { return mStats; }
    const Stats& stats() const { return mStats; }

  private:
    // Where one tile's geometry sits in the shared buffers. Indices stay
    // TILE-LOCAL; drawIndexed() is given the tile's base vertex separately, so
    // a tile's geometry never has to be rewritten to be placed -- which is what
    // makes leaving it where it is possible at all.
    struct Region
    {
        std::uint32_t vertexBase { 0 };
        std::uint32_t vertexCount { 0 };
        std::uint32_t indexBase { 0 };
        std::uint32_t indexCount { 0 };
    };

    // Works out where every batch's geometry lives in the shared buffers,
    // leaving tiles that are already resident exactly where they are, and
    // queues an upload for each one that is not. Grows or compacts the buffers
    // when it has to.
    bool planResidency(std::span<const GpuBatch> batches);
    // Places one tile, evicting tiles no longer on screen if it has to. False
    // means even that was not enough -- the buffer is fragmented and the caller
    // has to compact.
    bool allocateRegion(Region& region);

    // Borrowed from the host for as long as the resources live.
    QRhi* mRhi { nullptr };
    QRhiRenderPassDescriptor* mPass { nullptr };
    int mSampleCount { 1 };
    // OpenGL reads back bottom-up, every other backend top-down. Baked into
    // the projection instead of flipping the image afterwards. Read it through
    // screenToClip() rather than directly -- on its own it is only half the
    // answer.
    bool mYUpInFramebuffer { false };
    // Bytes between one tile's uniform block and the next. The hardware's
    // minimum alignment, not sizeof(the struct).
    quint32 mUniformStride { 256 };

    std::unique_ptr<QRhiGraphicsPipeline> mPipeline;
    // Same everything but the fragment stage: draws the map's own geometry in
    // one colour, for the route and the road the vehicle is on.
    std::unique_ptr<QRhiGraphicsPipeline> mHighlightPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> mSrb;
    std::unique_ptr<QRhiBuffer> mVertexBuffer;
    std::unique_ptr<QRhiBuffer> mIndexBuffer;
    std::unique_ptr<QRhiBuffer> mUniformBuffer;

    // The text pass. One pipeline, one atlas page, one dynamic vertex buffer
    // rewritten every frame -- the quads move with the camera, so there is
    // nothing to cache between frames the way tile geometry is cached.
    std::unique_ptr<QRhiGraphicsPipeline> mGlyphPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> mGlyphSrb;
    std::unique_ptr<QRhiBuffer> mGlyphVertexBuffer;
    std::unique_ptr<QRhiBuffer> mGlyphUniform;
    std::unique_ptr<QRhiTexture> mGlyphTexture;
    std::unique_ptr<QRhiSampler> mGlyphSampler;

    // What is currently on the GPU, so a frame that changed only the camera
    // does not even have to look at the residency table.
    std::vector<TileId> mUploadedIds;
    std::vector<std::uint64_t> mUploadedSerials;

    struct Resident
    {
        Region region;
        // Which planResidency() call last wanted this tile. A tile that has
        // left the visible set keeps its block until something else needs the
        // space, and this is what decides who goes first.
        std::uint64_t lastPlan { 0 };
    };
    // Keyed by TileGeometry::serial, NOT by TileId. Two sources -- a basemap
    // and an overlay archive -- can both hand up the same z/x/y with entirely
    // different triangles in one frame, and a TileId key would silently draw
    // one of them twice. The serial is unique per tessellation for exactly the
    // reason the frame key already uses it.
    std::unordered_map<std::uint64_t, Resident> mResident;
    std::uint64_t mPlanCounter { 0 };
    // Resolved per batch index, so the draw loop never touches the map.
    std::vector<Region> mBatchRegions;
    // Queued by planResidency(), drained into the frame's resource update
    // batch. The pointers are into the batches' own geometry, which the caller
    // owns for the whole frame, and QRhi copies out of them when the batch is
    // built.
    struct PendingUpload
    {
        const void* vertexData { nullptr };
        const void* indexData { nullptr };
        Region region;
    };
    std::vector<PendingUpload> mPendingUploads;

    BufferArena mVertexArena;
    BufferArena mIndexArena;

    // The tile-limit warning is once per pass, not once per frame.
    bool mWarnedTileLimit { false };

    // Scratch reused across frames -- cleared or overwritten each frame,
    // capacity kept, so a steady repaint allocates nothing here.
    std::vector<float> mGlyphScratch;
    std::vector<TextQuad> mTextQuads;
    QImage mAtlasPage;
    bool mAtlasDirty { false };
    quint32 mGlyphVertexCount { 0 };
    std::vector<char> mUniformScratch;

    Stats mStats;
};

} // namespace map_render

#endif // MAP_PASS_H
