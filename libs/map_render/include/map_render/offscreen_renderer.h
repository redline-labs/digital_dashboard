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
//   * It is NOT a surface-binding problem either, and it is not about the
//     backend: QRhiWidget is gated on the platform integration advertising
//     RhiBasedRendering and bails before it ever picks one, so
//     setApi(Api::Null) fails offscreen exactly as setApi(Api::OpenGL) does.
//     Driving QRhi against a texture we allocate ourselves asks the platform
//     for nothing, which is why it works.
//
// The gate is in the OFFSCREEN PLUGIN, not in QRhiWidget, and it is absolute:
// QOffscreenIntegration::hasCapability returns `case RhiBasedRendering: return
// false`, and the Linux X11/GLX variant overrides only OpenGL and
// ThreadedOpenGL, letting RhiBasedRendering fall through to that same false.
// So no platform, Linux included, gets a QRhiWidget under
// QT_QPA_PLATFORM=offscreen.
//
// What DOES work headless is not the offscreen plugin at all: a virtual display
// -- Xvfb with the xcb plugin, or eglfs on an embedded target -- gives a real
// GL stack with no monitor attached, and QRhiWidget comes up normally there.
// That is the standard way Qt GUI tests get a GPU in CI. It is a live option
// for a Linux dashboard, where production and CI could share it; it is not one
// for macOS development, which is why this renderer still exists.
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
//
// The drawing itself is NOT here: it is MapPass, which knows nothing about
// where its pixels end up and is shared verbatim with libs/map_surface's
// QRhiWidget host. What this class owns is the offscreen texture, the frame
// memo and the readback -- the part that makes a map visible headless, and the
// part that costs. MEASURED at a device pixel ratio of 2: recording the pass is
// 0.04 ms at 660x640 and 0.21 ms at 2560x1440; the readback and the blit that
// follows are 1.03 ms and 8.77 ms.
#ifndef MAP_OFFSCREEN_RENDERER_H
#define MAP_OFFSCREEN_RENDERER_H

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

#include <QByteArray>
#include <QImage>
#include <QMatrix4x4>
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

#include "map_render/map_pass.h"
#include "map_render/projection.h"
#include "map_render/style.h"
#include "map_render/tessellator.h"
#include "map_render/text_quad.h"

class QOffscreenSurface;
class QRhi;
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
class QRhiSampler;

namespace map_render
{

// The offscreen host for MapPass. GpuBatch, the highlight and the tile budget
// all live in map_pass.h now, aliased here so callers did not have to change.
class OffscreenRenderer
{
  public:
    using Highlight = MapPass::Highlight;
    using Stats = MapPass::Stats;
    static constexpr std::size_t kMaxTilesPerFrame = MapPass::kMaxTilesPerFrame;

    // Null when no backend could be created. That is a hard failure -- there is
    // no CPU fallback -- but it must not be a crash: the widget reports it.
    static std::unique_ptr<OffscreenRenderer> create();

    ~OffscreenRenderer();

    OffscreenRenderer(const OffscreenRenderer&) = delete;
    OffscreenRenderer& operator=(const OffscreenRenderer&) = delete;

    QString backendName() const;

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

    void setText(std::vector<TextQuad> quads, const QImage& atlasPage, bool atlasDirty)
    {
        mPass.setText(std::move(quads), atlasPage, atlasDirty);
    }

    Stats stats() const { return mPass.stats(); }

  private:
    // Declared, not defaulted here: every member is a unique_ptr to a
    // forward-declared type, and a defaulted constructor has to be able to
    // destroy them. Defined in the .cpp, where the types are complete.
    OffscreenRenderer();

    bool initialise();
    bool ensureTarget(const QSize& size);

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
    std::unique_ptr<QRhiRenderPassDescriptor> mRenderPass;

    // Declared AFTER the objects it borrows, so it is destroyed before them:
    // its pipelines hold a pointer to mRenderPass, and its buffers belong to
    // mRhi.
    MapPass mPass;

    QSize mSize;
    int mSampleCount { 1 };

    // What produced mFrame. A repaint with all of this unchanged would redraw
    // the identical image and read it back again, which is the whole frame
    // cost for nothing -- and a widget repaints for reasons that have nothing
    // to do with the map, including a sibling widget being updated.
    //
    // Only the offscreen host has one: a QRhiWidget draws when it schedules an
    // update, so there is no equivalent of an unrelated repaint to defend
    // against there.
    struct FrameKey
    {
        QSize size;
        Coordinate center;
        double zoom { 0.0 };
        double bearing { 0.0 };
        double pitch { 0.0 };
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
                   bearing == other.bearing && pitch == other.pitch &&
                   widthScale == other.widthScale &&
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
};

} // namespace map_render

#endif // MAP_OFFSCREEN_RENDERER_H
