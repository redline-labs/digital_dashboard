// SPDX-License-Identifier: GPL-3.0-or-later
//
// The map, drawn by the GPU, headless.
//
// Deliberately NOT a QRhiWidget and NOT a QOpenGLWidget, for ONE reason: under
// QT_QPA_PLATFORM=offscreen neither comes up. QOpenGLContext::create() returns
// false, and QRhiWidget reports "QRhi is not supported on this platform" and
// draws nothing -- which would make the map invisible to ui_screenshot and to
// every `gui` test. That is the constraint the whole design is built around.
//
// Two things about that are easy to get wrong, and were:
//
//   * It is NOT that QRhiWidget captures as black. MEASURED on Qt 6.11/cocoa:
//     its content DOES come back through a parent's QWidget::grab(), and a
//     child widget over it composites correctly on top. Qt composites
//     render-to-texture widgets itself. Capture is not the problem; headless
//     is.
//   * It is NOT a surface-binding problem either. QRhiWidget is gated on the
//     platform integration advertising RHI-based rendering, and bails before
//     it ever picks a backend -- setApi(Api::Null) fails offscreen just as
//     setApi(Api::OpenGL) does. Driving QRhi against a texture we allocate
//     ourselves asks the platform for nothing, which is why it works.
//
// Both verified on macOS. The Linux offscreen plugin can be built with GLX/EGL
// support, so a Linux target may not be gated the same way -- if that turns out
// to be true, the trade-off below is worth re-opening on that hardware.
//
// Driving QRhi directly against an offscreen texture has no such dependency:
// Metal, Vulkan and D3D do not need a window to render into a texture. The
// frame is read back into a QImage and blitted by the widget's QPainter.
//
// The GPU stage is dominated by the synchronous round trip, not by the drawing
// and not, on unified memory, by the readback. Measured on an M-series Metal
// backend at 660x640: 0.88 ms total, of which the readback is only ~0.15 ms --
// deleting it entirely still leaves 0.73 ms of waiting. It is linear in pixels
// on top of that, about 0.5 ms per megapixel of the TARGET, so the same widget
// at a device pixel ratio of 2 is ~1.3 ms.
//
// DO NOT carry those proportions to other hardware. On unified memory a
// readback is a copy in shared RAM; on a discrete or low-power integrated GPU
// it crosses a bus. docs/map.md records 2.44 ms fixed + 3.51 ms/Mpx on an Intel
// UHD 630 through Mesa -- seven times the per-pixel cost and a fixed stall
// three times larger. On that class of hardware the readback IS the frame. Multisampling is not the cost -- forcing sampleCount to 1 moves a
// 5120x2880 frame by under a millisecond -- which is why the sample count is
// taken as high as the hardware offers and the SIZE is the thing to think
// about. Measure with map_bench --width/--height/--dpr.
//
// One draw call per (layer, tile), layer-major. Tile-major would let one tile's
// landcover bury its neighbour's motorway, which shows up as roads vanishing
// along tile seams.
#ifndef MAP_GPU_RENDERER_H
#define MAP_GPU_RENDERER_H

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QtGui/qtguiglobal.h>

// The Vulkan backend is the only one that needs an instance object, and the
// type only exists where Qt found both the feature and the SDK header. Same
// guard qrhi_platform.h puts on QRhiVulkanInitParams: QT_CONFIG(vulkan) alone
// is true on macOS, where there is no vulkan.h and nothing is declared.
#if QT_CONFIG(vulkan) && __has_include(<vulkan/vulkan.h>)
#define MAP_HAS_VULKAN 1
#include <QVulkanInstance>
#else
#define MAP_HAS_VULKAN 0
#endif

#include "map/projection.h"
#include "map/style.h"
#include "map/tessellator.h"
#include "map/text_quad.h"

class QOffscreenSurface;
class QRhi;
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiSampler;

namespace map_widget
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

class GpuRenderer
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

    // Null when no backend could be created. That is a hard failure -- there is
    // no CPU fallback -- but it must not be a crash: the widget reports it.
    static std::unique_ptr<GpuRenderer> create();

    ~GpuRenderer();

    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;

    QString backendName() const;

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

    // Draw one frame. The returned image is owned by the renderer and is valid
    // until the next call -- the widget blits it and does not keep it.
    //
    // Returns a null QImage if the frame could not be rendered.
    const QImage& render(const Projection& projection, const std::vector<GpuBatch>& batches,
                         const MapStyle_t& style, const QColor& background,
                         const Highlight& highlight);

    // Nothing highlighted, which is every caller that has no route and no
    // matched road -- the bench, the tests, and the widget before the first fix
    // arrives.
    const QImage& render(const Projection& projection, const std::vector<GpuBatch>& batches,
                         const MapStyle_t& style, const QColor& background)
    {
        return render(projection, batches, style, background, Highlight {});
    }

    // The frame's text, as quads into `atlasPage`.
    //
    // Drawn last and inside the SAME pass as the tiles, so it lands over every
    // one of them without a second pass having to clear or reload the target.
    // The page is re-uploaded only when `atlasDirty` -- a glyph set is an
    // alphabet, so that settles within the first frames of a drive and then
    // never fires again.
    //
    // Text is part of the frame's identity: two frames with the same camera
    // and the same tiles but different labels are different pictures, so the
    // quads are compared by the memo like everything else.
    void setText(std::vector<TextQuad> quads, const QImage& atlasPage, bool atlasDirty);

    struct Stats
    {
        // Frames served from the memo because nothing that affects the image
        // had changed. Climbing while the vehicle moves would mean the key is
        // missing something.
        std::uint64_t reused { 0 };
        int drawCalls { 0 };
        int tiles { 0 };
        std::uint32_t vertices { 0 };
        // Indices uploaded for the frame. Three per triangle, and no longer the
        // same number as `vertices` -- which is the point of indexing.
        std::uint32_t indices { 0 };
        // Wall clock for the last render() including readback. endOffscreenFrame
        // waits for the GPU, so this is a real number rather than a submission
        // time.
        double lastFrameMs { 0.0 };
        // How many times the vertex buffer has been rebuilt. Should climb only
        // when the visible tile set or the style changes; if it tracks the
        // frame count, something is invalidating the cache every frame.
        std::uint64_t uploads { 0 };
        int sampleCount { 1 };
        // What the last frame was actually rendered at. Normally the screen's
        // ratio; lower when the viewport was wide enough that a device-pixel
        // texture would have exceeded what the backend will allocate, in which
        // case the frame is upscaled to fit the widget. Worth reporting,
        // because the only other evidence of that is a map that looks slightly
        // soft.
        double devicePixelRatio { 1.0 };
    };

    Stats stats() const { return mStats; }

  private:
    // Declared, not defaulted here: every member is a unique_ptr to a
    // forward-declared type, and a defaulted constructor has to be able to
    // destroy them. Defined in the .cpp, where the types are complete.
    GpuRenderer();

    bool initialise();
    bool ensureTarget(const QSize& size);
    // True when `batches` differ from what is already on the GPU.
    bool batchesChanged(std::span<const GpuBatch> batches) const;
    // Works out the per-tile offsets, grows the vertex buffer if it must, and
    // flattens the geometry into `flat`. Does NOT submit: the caller puts the
    // upload in the same resource update batch as the frame's uniforms, so a
    // frame that brings in a new tile is still one submission.
    bool prepareUpload(std::span<const GpuBatch> batches, std::vector<MapVertex>& flat,
                       std::vector<std::uint32_t>& flatIndices);

#if MAP_HAS_VULKAN
    // Declared BEFORE mRhi so it outlives it: members are destroyed in reverse
    // declaration order, and the Vulkan backend holds this instance. Null on
    // every platform that did not take the Vulkan path.
    std::unique_ptr<QVulkanInstance> mVulkan;
#endif
    // Declared BEFORE mRhi for the same reason, and null unless the OpenGL path
    // was taken: that backend keeps this surface to make its context current on
    // whenever there is no window to bind to.
    std::unique_ptr<QOffscreenSurface> mFallbackSurface;
    std::unique_ptr<QRhi> mRhi;
    std::unique_ptr<QRhiTexture> mMsaaColour;
    std::unique_ptr<QRhiTexture> mResolve;
    std::unique_ptr<QRhiTextureRenderTarget> mTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> mPass;
    std::unique_ptr<QRhiGraphicsPipeline> mPipeline;
    // Same everything but the fragment stage: draws the map's own geometry in
    // one colour, for the route and the road the vehicle is on.
    std::unique_ptr<QRhiGraphicsPipeline> mHighlightPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> mSrb;
    std::unique_ptr<QRhiBuffer> mVertexBuffer;
    std::unique_ptr<QRhiBuffer> mIndexBuffer;
    std::unique_ptr<QRhiBuffer> mUniformBuffer;

    QSize mSize;
    int mSampleCount { 1 };
    // OpenGL reads back bottom-up, every other backend top-down. Baked into
    // the projection instead of flipping the image afterwards.
    bool mYUpInFramebuffer { false };
    // Bytes between one tile's uniform block and the next. The hardware's
    // minimum alignment, not sizeof(the struct).
    quint32 mUniformStride { 256 };

    // What is currently on the GPU, so a frame that changed only the camera
    // uploads nothing.
    std::vector<TileId> mUploadedIds;
    std::vector<std::uint64_t> mUploadedSerials;
    std::vector<std::uint32_t> mTileBaseVertex;
    // Where each tile's indices start in the shared index buffer. The indices
    // themselves stay tile-local; drawIndexed() is given the tile's base vertex
    // separately, so a tile's geometry never has to be rewritten to be placed.
    std::vector<std::uint32_t> mTileBaseIndex;
    std::uint32_t mUploadedVertexCount { 0 };
    std::uint32_t mUploadedIndexCount { 0 };
    quint32 mVertexCapacity { 0 };
    quint32 mIndexCapacity { 0 };

    // The tile-limit warning is once per renderer, not once per frame.
    bool mWarnedTileLimit { false };

    // What produced mFrame. A repaint with all of this unchanged would redraw
    // the identical image and read it back again, which is the whole frame
    // cost for nothing -- and a widget repaints for reasons that have nothing
    // to do with the map, including a sibling widget being updated.
    struct FrameKey
    {
        QSize size;
        Coordinate center;
        double zoom { 0.0 };
        double bearing { 0.0 };
        float widthScale { 0.0F };
        QRgb background { 0 };
        // The per-layer zoom floors, which the draw loop culls whole layers on
        // -- so two frames with the same camera and the same tiles are still
        // different pictures if these moved.
        //
        // Kept as the RESOLVED floors rather than as MapDetail_t, because that
        // is exactly what render() consults; a style field that stops feeding
        // layerMinZoom() would otherwise leave a stale entry here forever.
        //
        // It cannot go stale today -- a config change destroys and rebuilds the
        // widget, so the style is fixed for a renderer's life. It is in the key
        // because that is a fact about the dashboard, not about this class, and
        // the failure it would cause is a style edit that silently does nothing.
        std::array<double, kMapLayerCount> layerMinZooms {};
        QRgb highlightColour { 0 };
        float highlightWidth { 0.0F };
        std::vector<std::uint64_t> highlightIds;

        // Everything except the vectors, which the memo compares in place --
        // see render(). Kept next to the fields so a new scalar cannot be
        // added without deciding whether it belongs here.
        bool scalarsEqual(const FrameKey& other) const
        {
            return size == other.size && center == other.center && zoom == other.zoom &&
                   bearing == other.bearing && widthScale == other.widthScale &&
                   background == other.background && highlightColour == other.highlightColour &&
                   highlightWidth == other.highlightWidth &&
                   layerMinZooms == other.layerMinZooms;
        }
        // Batch identity, so a tile arriving invalidates the frame. Serials,
        // not addresses -- see TileGeometry::serial.
        std::vector<TileId> ids;
        std::vector<std::uint64_t> serials;
        // The frame's text. Labels move with the camera and appear and vanish
        // with collision, so two frames can agree on every tile and still be
        // different pictures. Compared rather than hashed: a viewport holds a
        // few hundred quads, and an exact compare is both cheaper than it
        // looks and impossible to get subtly wrong.
        std::vector<TextQuad> text;
        // Quantised fades -- see quantizeAlpha(). A fading tile makes every
        // frame a fresh picture; when the last fade settles at 255 the key
        // stabilises and the memo takes over again.
        std::vector<std::uint8_t> alphas;

        bool operator==(const FrameKey&) const = default;
    };
    FrameKey mFrameKey;
    bool mHaveFrame { false };

    // Holds the pixels mFrame points AT -- see render(). MOVED out of the
    // readback result rather than copied, so this costs a pointer swap. Both
    // are only valid until the next render(), which is the lifetime the
    // accessor already promises.
    QByteArray mReadbackData;
    QImage mFrame;
    Stats mStats;

    // Scratch reused across frames -- cleared or overwritten each render,
    // capacity kept, so a steady repaint allocates nothing here.
    // The text pass. One pipeline, one atlas page, one dynamic vertex buffer
    // rewritten every frame -- the quads move with the camera, so there is nothing
    // to cache between frames the way tile geometry is cached.
    std::unique_ptr<QRhiGraphicsPipeline> mGlyphPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> mGlyphSrb;
    std::unique_ptr<QRhiBuffer> mGlyphVertexBuffer;
    std::unique_ptr<QRhiBuffer> mGlyphUniform;
    std::unique_ptr<QRhiTexture> mGlyphTexture;
    std::unique_ptr<QRhiSampler> mGlyphSampler;
    std::vector<float> mGlyphScratch;
    std::vector<TextQuad> mTextQuads;
    QImage mAtlasPage;
    bool mAtlasDirty { false };

    std::vector<MapVertex> mFlatScratch;
    std::vector<std::uint32_t> mFlatIndexScratch;
    std::vector<char> mUniformScratch;
};

} // namespace map_widget

#endif // MAP_GPU_RENDERER_H
