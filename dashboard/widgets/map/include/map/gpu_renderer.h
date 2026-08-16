// SPDX-License-Identifier: GPL-3.0-or-later
//
// The map, drawn by the GPU, headless.
//
// Deliberately NOT a QRhiWidget and NOT a QOpenGLWidget. Both bind to a
// surface owned by the platform plugin, and under QT_QPA_PLATFORM=offscreen
// there is no such surface: `QOpenGLContext::create()` returns false and
// QRhiWidget reports "QRhi is not supported on this platform" and hands back a
// null map. That would make the map invisible to ui_screenshot and to every
// `gui` test, and it is the constraint the whole design is built around.
//
// Driving QRhi directly against an offscreen texture has no such dependency:
// Metal, Vulkan and D3D do not need a window to render into a texture. The
// frame is read back into a QImage and blitted by the widget's QPainter. The
// readback was measured at ~0.03 ms, which is noise against a ~0.2 ms frame --
// on unified-memory hardware there is no copy across a bus to pay for.
//
// One draw call per (layer, tile), layer-major. Tile-major would let one tile's
// landcover bury its neighbour's motorway, which shows up as roads vanishing
// along tile seams.
#ifndef MAP_GPU_RENDERER_H
#define MAP_GPU_RENDERER_H

#include <cstdint>
#include <memory>
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

class QRhi;
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;

namespace map_widget
{

// One tile's worth of geometry, with the id that says where to put it.
struct GpuBatch
{
    TileId id;
    std::shared_ptr<const TileGeometry> geometry;
};

class GpuRenderer
{
  public:
    // Null when no backend could be created. That is a hard failure -- there is
    // no CPU fallback -- but it must not be a crash: the widget reports it.
    static std::unique_ptr<GpuRenderer> create();

    ~GpuRenderer();

    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;

    QString backendName() const;

    // Draw one frame. The returned image is owned by the renderer and is valid
    // until the next call -- the widget blits it and does not keep it.
    //
    // Returns a null QImage if the frame could not be rendered.
    const QImage& render(const Projection& projection, const std::vector<GpuBatch>& batches,
                         const MapStyle_t& style, const QColor& background);

    struct Stats
    {
        // Frames served from the memo because nothing that affects the image
        // had changed. Climbing while the vehicle moves would mean the key is
        // missing something.
        std::uint64_t reused { 0 };
        int drawCalls { 0 };
        int tiles { 0 };
        std::uint32_t vertices { 0 };
        // Wall clock for the last render() including readback. endOffscreenFrame
        // waits for the GPU, so this is a real number rather than a submission
        // time.
        double lastFrameMs { 0.0 };
        // How many times the vertex buffer has been rebuilt. Should climb only
        // when the visible tile set or the style changes; if it tracks the
        // frame count, something is invalidating the cache every frame.
        std::uint64_t uploads { 0 };
        int sampleCount { 1 };
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
    bool batchesChanged(const std::vector<GpuBatch>& batches) const;
    // Works out the per-tile offsets, grows the vertex buffer if it must, and
    // flattens the geometry into `flat`. Does NOT submit: the caller puts the
    // upload in the same resource update batch as the frame's uniforms, so a
    // frame that brings in a new tile is still one submission.
    bool prepareUpload(const std::vector<GpuBatch>& batches, std::vector<MapVertex>& flat);

#if MAP_HAS_VULKAN
    // Declared BEFORE mRhi so it outlives it: members are destroyed in reverse
    // declaration order, and the Vulkan backend holds this instance. Null on
    // every platform that did not take the Vulkan path.
    std::unique_ptr<QVulkanInstance> mVulkan;
#endif
    std::unique_ptr<QRhi> mRhi;
    std::unique_ptr<QRhiTexture> mMsaaColour;
    std::unique_ptr<QRhiTexture> mResolve;
    std::unique_ptr<QRhiTextureRenderTarget> mTarget;
    std::unique_ptr<QRhiRenderPassDescriptor> mPass;
    std::unique_ptr<QRhiGraphicsPipeline> mPipeline;
    std::unique_ptr<QRhiShaderResourceBindings> mSrb;
    std::unique_ptr<QRhiBuffer> mVertexBuffer;
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
    std::uint32_t mUploadedVertexCount { 0 };
    quint32 mVertexCapacity { 0 };

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
        // Batch identity, so a tile arriving invalidates the frame. Serials,
        // not addresses -- see TileGeometry::serial.
        std::vector<TileId> ids;
        std::vector<std::uint64_t> serials;

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
};

} // namespace map_widget

#endif // MAP_GPU_RENDERER_H
