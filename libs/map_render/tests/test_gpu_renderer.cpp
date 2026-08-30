// SPDX-License-Identifier: GPL-3.0-or-later
//
// The GPU renderer, headless.
//
// The premise of the whole design is that QRhi renders into a texture with no
// window anywhere -- so this test runs under QT_QPA_PLATFORM=offscreen and
// asserts that known geometry lands on known PIXELS. If that ever stops
// holding, every `gui` test and every ui_screenshot of a map silently returns
// a blank frame instead of failing, so it is worth pinning down explicitly.
//
// Colour comparisons are tolerant. MSAA resolve, sRGB handling and rasterisation
// rules all differ slightly across backends, and asserting on an exact byte
// would make this test a report on which GPU the CI box has.

#include "map_render/gpu_renderer.h"
#include "map_render/tessellator.h"

#include "mvt/tile.h"

#include <QColor>
#include <QGuiApplication>
#include <QImage>

#include <spdlog/spdlog.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using map_render::Camera;
using map_render::Coordinate;
using map_render::GpuBatch;
using map_render::GpuRenderer;
using map_render::MapLayer;
using map_render::MapVertex;
using map_render::Projection;
using map_render::TileGeometry;
using map_render::TileId;

constexpr int kWidth = 400;
constexpr int kHeight = 300;

// Irvine, so the camera sits over the archive's coverage and the tile ids in
// this file match the ones libs/mvt and the widget use.
constexpr double kIrvineLat = 33.6865966;
constexpr double kIrvineLon = -117.8557874;

bool near(const QColor& got, const QColor& want, int tolerance = 24)
{
    return std::abs(got.red() - want.red()) <= tolerance &&
           std::abs(got.green() - want.green()) <= tolerance &&
           std::abs(got.blue() - want.blue()) <= tolerance;
}

std::string describe(const QColor& colour)
{
    return "rgb(" + std::to_string(colour.red()) + "," + std::to_string(colour.green()) + "," +
           std::to_string(colour.blue()) + ")";
}

// A tile whose water layer covers the whole tile. The simplest geometry whose
// position on screen can be predicted by hand.
std::shared_ptr<const TileGeometry> fullTileQuad(MapLayer layer, float r, float g, float b)
{
    auto geometry = std::make_shared<TileGeometry>();

    const auto vertex = [&](float x, float y) {
        return MapVertex { x, y, 0.0f, 0.0f, 0.0f, r, g, b, 1.0f };
    };

    // Tile-local [0,1], four corners drawn as two triangles by index.
    geometry->vertices = { vertex(0.0f, 0.0f), vertex(1.0f, 0.0f), vertex(1.0f, 1.0f),
                           vertex(0.0f, 1.0f) };
    geometry->indices = { 0, 1, 2, 0, 2, 3 };

    // Everything before `layer` starts at 0 and everything from it on ends at
    // the full count.
    for (std::size_t i = 0; i <= map_render::kMapLayerCount; ++i)
    {
        geometry->layerStart[i] = (i <= std::size_t(layer)) ? 0U : 4U;
        geometry->layerIndexStart[i] = (i <= std::size_t(layer)) ? 0U : 6U;
    }
    return geometry;
}

// A tile whose only geometry is one horizontal line across its middle, at
// `halfPx` half-width. The vertex layout is pos, normal, half-width, colour --
// the normal and the half-width are what the shader expands, and expanding it
// there rather than on the CPU is why a width is a SCREEN pixel count rather
// than something baked into the geometry. See map.vert.
std::shared_ptr<const TileGeometry> fullTileStripe(float halfPx)
{
    auto geometry = std::make_shared<TileGeometry>();

    const auto vertex = [&](float x, float normalY) {
        return MapVertex { x, 0.5f, 0.0f, normalY, halfPx, 0.0f, 1.0f, 0.0f, 1.0f };
    };

    // Two vertices per end, one either side of the centreline -- the shape the
    // tessellator now produces.
    geometry->vertices = { vertex(0.0f, -1.0f), vertex(0.0f, 1.0f), vertex(1.0f, -1.0f),
                           vertex(1.0f, 1.0f) };
    geometry->indices = { 0, 1, 2, 2, 1, 3 };

    for (std::size_t i = 0; i <= map_render::kMapLayerCount; ++i)
    {
        geometry->layerStart[i] = (i <= std::size_t(MapLayer::Motorway)) ? 0U : 4U;
        geometry->layerIndexStart[i] = (i <= std::size_t(MapLayer::Motorway)) ? 0U : 6U;
    }
    return geometry;
}

// How much of the frame the green stripe covers. Counted rather than sampled,
// because the question is how WIDE the line came out and a sample says only
// whether it was hit.
int greenPixels(const QImage& frame)
{
    int count = 0;
    for (int y = 0; y < frame.height(); ++y)
    {
        for (int x = 0; x < frame.width(); ++x)
        {
            const QColor pixel = frame.pixelColor(x, y);
            if (pixel.green() > 128 && pixel.red() < 128 && pixel.blue() < 128)
            {
                ++count;
            }
        }
    }
    return count;
}

// The tile the camera is centred on, so its quad covers the middle of the frame.
//
// The sort is not decoration: visibleTiles() is row-major and stable -- see
// Projection::visibleTiles -- so front() is the north-west corner of the
// viewport, not the middle. Asking for centre-outward is what makes this
// helper's name true.
TileId centreTile(const Projection& projection)
{
    auto tiles = projection.visibleTiles(14, 0);
    if (tiles.empty())
    {
        return TileId { 14, 0, 0 };
    }
    projection.sortCentreOutward(tiles);
    return tiles.front();
}

// ============================================================================

void test_a_backend_comes_up_with_no_window()
{
    const auto renderer = GpuRenderer::create();
    check(renderer != nullptr,
          "QRhi initialises under QT_QPA_PLATFORM=offscreen -- if this fails, every map is blank");
    if (renderer)
    {
        SPDLOG_INFO("backend: {}", renderer->backendName().toStdString());
        check(!renderer->backendName().isEmpty(), "and names itself");
    }
}

void test_an_empty_frame_is_the_background_colour()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const QColor background(0x16, 0x18, 0x1d);

    const QImage& frame = renderer->render(projection, {}, style, background);

    check(!frame.isNull(), "a frame comes back");
    if (frame.isNull())
    {
        return;
    }
    check(frame.width() == kWidth && frame.height() == kHeight,
          "at the size that was asked for, got " + std::to_string(frame.width()) + "x" +
              std::to_string(frame.height()));

    const QColor centre = frame.pixelColor(kWidth / 2, kHeight / 2);
    check(near(centre, background),
          "with nothing to draw the frame clears to the background, got " + describe(centre));
}

void test_geometry_reaches_the_pixels()
{
    // The whole pipeline in one assertion: a tessellated quad, uploaded, drawn
    // through the baked shader, resolved out of MSAA and read back.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const QColor background(0x16, 0x18, 0x1d);

    // Bright green, nothing like the background or any style colour.
    std::vector<GpuBatch> batches { GpuBatch {
        centreTile(projection), fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) } };

    const QImage& frame = renderer->render(projection, batches, style, background);
    check(!frame.isNull(), "the frame renders");
    if (frame.isNull())
    {
        return;
    }

    const QColor centre = frame.pixelColor(kWidth / 2, kHeight / 2);
    check(near(centre, QColor(0, 255, 0)),
          "the tile's own colour is at the centre of the viewport, got " + describe(centre));

    const auto stats = renderer->stats();
    check(stats.tiles == 1, "one tile was drawn");
    check(stats.drawCalls >= 1, "with at least one draw call");
    // Four corners, six indices: the quad is indexed now, not expanded.
    check(stats.vertices == 4, "and four vertices, got " + std::to_string(stats.vertices));
    check(stats.indices == 6, "drawn by six indices, got " + std::to_string(stats.indices));
    SPDLOG_INFO("frame {:.2f} ms, {} draw call(s), {}x MSAA", stats.lastFrameMs, stats.drawCalls,
                stats.sampleCount);
}

void test_layer_order_beats_tile_order()
{
    // THE seam bug. Drawing tile-by-tile lets a later tile's landcover paint over
    // an earlier tile's motorway, so roads disappear along tile boundaries -- and
    // it looks like a tile-loading fault rather than a draw-order one. The fix is
    // to iterate layer-major across every tile, which is what this pins.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const QColor background(0x16, 0x18, 0x1d);
    const TileId centre = centreTile(projection);

    // Two batches on the SAME tile: a motorway listed first, then landcover.
    // Tile-major would draw them in list order and the landcover would win; the
    // motorway is the later layer and must survive.
    std::vector<GpuBatch> batches {
        GpuBatch { centre, fullTileQuad(MapLayer::Motorway, 1.0f, 0.0f, 0.0f) },
        GpuBatch { centre, fullTileQuad(MapLayer::Landcover, 0.0f, 0.0f, 1.0f) },
    };

    const QImage& frame = renderer->render(projection, batches, style, background);
    if (frame.isNull())
    {
        return;
    }

    const QColor pixel = frame.pixelColor(kWidth / 2, kHeight / 2);
    check(near(pixel, QColor(255, 0, 0)),
          "the motorway is on top of the landcover regardless of batch order, got " +
              describe(pixel));
}

void test_panning_does_not_reupload()
{
    // The performance claim, as a test. Tessellation is the expensive step and
    // is cached per tile; a frame that changed only the camera must touch the
    // uniform buffer and nothing else. If uploads track the frame count, the
    // cache is being invalidated every frame and the GPU path has bought nothing.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Projection first(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 }, kWidth,
                           kHeight);

    std::vector<GpuBatch> batches { GpuBatch {
        centreTile(first), fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) } };

    renderer->render(first, batches, style, background);
    const std::uint64_t afterFirst = renderer->stats().uploads;
    check(afterFirst >= 1, "the first frame uploads");

    for (int i = 1; i <= 8; ++i)
    {
        const Projection moved(
            Camera { Coordinate { kIrvineLat + (0.0001 * i), kIrvineLon }, 14.0 + (i * 0.01),
                     double(i) * 5.0 },
            kWidth, kHeight);
        renderer->render(moved, batches, style, background);
    }

    check(renderer->stats().uploads == afterFirst,
          "and panning, zooming and rotating upload nothing more, got " +
              std::to_string(renderer->stats().uploads) + " vs " + std::to_string(afterFirst));
}

void test_new_geometry_for_the_same_tile_does_reupload()
{
    // The other half: a style change re-tessellates in place, so the tile ids are
    // identical while the geometry is not. Comparing ids alone would keep drawing
    // the old triangles and the style change would appear to do nothing.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);

    std::uint64_t afterGreen = 0;
    {
        // Scoped, and this scope IS the test. The old geometry has to be freed
        // before the replacement is allocated, because that is what lets the
        // allocator hand the replacement the dead one's address -- and an
        // address-based change test then reports "unchanged" and the map keeps
        // drawing the triangles it uploaded before.
        std::vector<GpuBatch> green {
            GpuBatch { id, fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) } };
        renderer->render(projection, green, style, background);
        afterGreen = renderer->stats().uploads;
    }

    std::vector<GpuBatch> red { GpuBatch { id, fullTileQuad(MapLayer::Water, 1.0f, 0.0f, 0.0f) } };
    const QImage& frame = renderer->render(projection, red, style, background);

    check(renderer->stats().uploads > afterGreen,
          "replacing a tile's geometry re-uploads even though its id did not change");
    if (!frame.isNull())
    {
        const QColor pixel = frame.pixelColor(kWidth / 2, kHeight / 2);
        check(near(pixel, QColor(255, 0, 0)), "and the new colour is what draws, got " +
                                                  describe(pixel));
    }
}

void test_the_frame_follows_a_resize()
{
    // Qt does not deliver resizeEvent to a widget that was never shown, so the
    // widget recomputes on paint -- which means render() gets a size change with
    // no warning and has to rebuild the render target underneath itself.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);

    for (const QSize size : { QSize(320, 240), QSize(800, 480), QSize(200, 200) })
    {
        const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                    size.width(), size.height());
        std::vector<GpuBatch> batches { GpuBatch {
            centreTile(projection), fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) } };

        const QImage& frame = renderer->render(projection, batches, style, background);
        check(!frame.isNull() && frame.size() == size,
              "the frame is " + std::to_string(size.width()) + "x" +
                  std::to_string(size.height()) + " after a resize");
        if (!frame.isNull() && frame.size() == size)
        {
            check(near(frame.pixelColor(size.width() / 2, size.height() / 2), QColor(0, 255, 0)),
                  "and still draws the tile");
        }
    }
}

void test_north_is_up_and_the_image_is_not_flipped()
{
    // A vertical flip is invisible on symmetric geometry and catastrophic on a
    // map: every backend disagrees about whether the framebuffer origin is at the
    // top or the bottom, and QRhi reports it rather than normalising it. Half a
    // tile, on the tile's northern half only, has to appear in the NORTHERN half
    // of the frame.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    // Wider than the rest of this file, so a whole 512 px tile fits on screen
    // with room either side of it. At 400x300 the tile's southern half falls off
    // the bottom and there is nowhere to check that it is empty.
    constexpr int kFlipSize = 900;
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kFlipSize, kFlipSize);
    const TileId id = centreTile(projection);

    auto half = std::make_shared<TileGeometry>();
    const auto vertex = [](float x, float y) {
        return MapVertex { x, y, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f };
    };
    // The tile's top half: local y from 0 to 0.5.
    half->vertices = { vertex(0.0f, 0.0f), vertex(1.0f, 0.0f), vertex(1.0f, 0.5f),
                       vertex(0.0f, 0.5f) };
    half->indices = { 0, 1, 2, 0, 2, 3 };
    for (std::size_t i = 0; i <= map_render::kMapLayerCount; ++i)
    {
        half->layerStart[i] = (i <= std::size_t(MapLayer::Water)) ? 0U : 4U;
        half->layerIndexStart[i] = (i <= std::size_t(MapLayer::Water)) ? 0U : 6U;
    }

    const QImage& frame = renderer->render(projection, { GpuBatch { id, half } }, style, background);
    if (frame.isNull())
    {
        return;
    }

    // Where the tile's own top edge and midline land on screen.
    const auto origin = projection.tileOrigin(id);
    const double size = projection.tileScreenSize(id.z);
    const int aboveMid = int(origin.y + (size * 0.25));
    const int belowMid = int(origin.y + (size * 0.75));
    const int column = int(origin.x + (size * 0.5));

    // Not a soft skip: if the tile is off screen the assertions below would
    // silently stop testing anything, which is the failure mode a flip test
    // exists to avoid in the first place.
    check(column >= 0 && column < kFlipSize && aboveMid >= 0 && aboveMid < kFlipSize &&
              belowMid >= 0 && belowMid < kFlipSize,
          "the tile is on screen, so the flip check actually runs");
    if (column < 0 || column >= kFlipSize || aboveMid < 0 || aboveMid >= kFlipSize ||
        belowMid < 0 || belowMid >= kFlipSize)
    {
        return;
    }

    check(near(frame.pixelColor(column, aboveMid), QColor(0, 255, 0)),
          "the tile's northern half draws in the northern half of the frame, got " +
              describe(frame.pixelColor(column, aboveMid)));
    check(near(frame.pixelColor(column, belowMid), background),
          "and its southern half is empty -- the image is not flipped, got " +
              describe(frame.pixelColor(column, belowMid)));
}


// Total green coverage in the frame, in whole-pixel equivalents.
//
// Counted as coverage rather than sampled, because the question a sub-pixel
// line raises is not "was it hit" but "how much ink did it lay down, and did
// that stay the same as the camera moved".
double greenInk(const QImage& frame, const QColor& background)
{
    double ink = 0.0;
    for (int y = 0; y < frame.height(); ++y)
    {
        for (int x = 0; x < frame.width(); ++x)
        {
            ink += double(frame.pixelColor(x, y).green() - background.green()) /
                   (255.0 - background.green());
        }
    }
    return ink;
}

// The ink a stripe of `halfPx` lays down, swept across a whole pixel of
// sub-pixel camera offset. A line narrower than a pixel lands differently at
// each offset, and the spread is the whole point: it is what the eye sees as
// crawling when the map moves.
struct InkSweep
{
    double lowest { 0.0 };
    double highest { 0.0 };
    double spread() const { return highest > 0.0 ? (highest - lowest) / highest : 0.0; }
    double middle() const { return (lowest + highest) / 2.0; }
};

InkSweep sweepInk(GpuRenderer& renderer, float halfPx)
{
    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);

    InkSweep out { 1e18, 0.0 };
    for (int step = 0; step < 10; ++step)
    {
        // Nudge the camera by a tenth of a pixel at a time, through the
        // projection's own maths so the offset really is sub-pixel.
        const Projection base(Camera { Coordinate { kIrvineLat, kIrvineLon }, 12.0, 0.0 }, kWidth,
                              kHeight);
        const auto centre = base.coordinateForScreen(
            map_render::ScreenPoint { kWidth / 2.0, (kHeight / 2.0) + (step * 0.1) });
        const Projection projection(Camera { centre, 12.0, 0.0 }, kWidth, kHeight);

        const QImage& frame = renderer.render(
            projection, { GpuBatch { centreTile(projection), fullTileStripe(halfPx) } }, style,
            background);
        if (frame.isNull())
        {
            return InkSweep {};
        }
        const double ink = greenInk(frame, background);
        out.lowest = std::min(out.lowest, ink);
        out.highest = std::max(out.highest, ink);
    }
    return out;
}

// A line thinner than a pixel does not land on pixel centres reliably. It drops
// out entirely at some sub-pixel positions and comes back at others, so it
// CRAWLS as the map moves -- and widthScaleForZoom() tapers to 0.15, which puts
// minor roads in exactly that state at low zoom.
//
// Measured against the old shader, a 0.1 px half-width swept over one pixel of
// camera offset gave ink between 0.0 and 32.1: gone entirely at some positions.
// No sample count fixes that; four coverage samples cannot represent a line
// that misses the pixel.
void test_a_hairline_road_never_disappears()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const InkSweep thin = sweepInk(*renderer, 0.1f);
    if (thin.highest <= 0.0)
    {
        return;
    }

    check(thin.lowest > 0.0,
          "a hairline road is drawn at every sub-pixel position, lowest ink " +
              std::to_string(thin.lowest));
    check(thin.spread() < 0.15,
          "and lays down the same ink wherever the camera sits, spread " +
              std::to_string(thin.spread()));
}

// The other half, and the one that is easy to get wrong: widening a hairline to
// a pixel WITHOUT fading it draws every one of them at full strength, and a
// continental view becomes a solid mat of roads -- worse than the crawl it
// replaced.
//
// Checked as linearity. Ink must stay proportional to the width asked for, so
// that five times the width is five times the ink even where both are below a
// pixel.
void test_widening_a_hairline_does_not_add_ink()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const InkSweep thin = sweepInk(*renderer, 0.1f);
    const InkSweep fivefold = sweepInk(*renderer, 0.5f);
    if (thin.highest <= 0.0 || fivefold.highest <= 0.0)
    {
        return;
    }

    const double expected = fivefold.middle() / 5.0;
    const double ratio = thin.middle() / expected;
    check(ratio > 0.75 && ratio < 1.25,
          "a fifth of the width lays down a fifth of the ink, ratio " + std::to_string(ratio));
}

// A line comfortably wider than the floor must be untouched by any of this.
void test_a_normal_width_line_is_unaffected()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 12.0, 0.0 },
                                kWidth, kHeight);
    const QImage& frame = renderer->render(
        projection, { GpuBatch { centreTile(projection), fullTileStripe(6.0f) } }, style,
        background);
    if (frame.isNull())
    {
        return;
    }

    int peak = 0;
    for (int y = 0; y < frame.height(); ++y)
    {
        peak = std::max(peak, frame.pixelColor(kWidth / 2, y).green());
    }
    check(peak > 240, "a wide line still draws solid, peak green " + std::to_string(peak));
}

// Fills carry a zero normal and a zero half-width and must never be faded --
// a washed-out lake is a bug none of the line checks above can see.
void test_a_fill_is_never_faded_by_the_line_floor()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);

    const QImage& frame = renderer->render(
        projection,
        { GpuBatch { centreTile(projection), fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) } },
        style, background);
    if (frame.isNull())
    {
        return;
    }

    check(near(frame.pixelColor(kWidth / 2, kHeight / 2), QColor(0, 255, 0)),
          "a fill draws at full strength, got " +
              describe(frame.pixelColor(kWidth / 2, kHeight / 2)));
}

// The only test that joins the tessellator to the pixels.
//
// Everything else here feeds the renderer geometry built by hand, which checks
// the renderer but says nothing about what tessellate() produces. A wrong index
// -- a quad whose second triangle is degenerate, say -- passes every count
// assertion in the tessellator tests and every pixel assertion here, because
// neither one draws what the other made.
//
// A road across the middle of a tile, tessellated for real, must come out as a
// CONTINUOUS band: covered at both ends and in the middle. Half a quad draws a
// wedge that is present at one end and gone at the other.
void test_a_tessellated_road_draws_as_a_continuous_band()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    // Wide enough that a band is unambiguous at this viewport.
    style.widths.road_primary = 8.0;
    style.road_width_scale = 2.0;
    const QColor background(0x16, 0x18, 0x1d);

    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);

    // A z14 tile is 512 px and the viewport is shorter than that, so the tile's
    // own middle is not necessarily on screen. Put the road on the row the
    // CAMERA is on, which is.
    const auto origin = projection.tileOrigin(id);
    const double tileSize = projection.tileScreenSize(id.z);
    const double localY = ((kHeight / 2.0) - origin.y) / tileSize;
    check(localY > 0.05 && localY < 0.95, "the camera sits inside the tile it picked");
    const auto roadY = std::int32_t(std::lround(localY * 4096.0));

    // A straight road right across the tile, in the layer `class: primary`
    // routes to.
    mvt::Layer roads;
    roads.name = "transportation";
    roads.extent = 4096;
    roads.keys = { "class" };
    roads.values = { mvt::Value(std::in_place_type<std::string>, "primary") };
    mvt::Feature road;
    road.type = mvt::GeomType::LineString;
    road.rings.push_back({ { 0, roadY }, { 1024, roadY }, { 2048, roadY }, { 3072, roadY },
                           { 4096, roadY } });
    road.tags = { 0, 0 };
    roads.features.push_back(std::move(road));

    mvt::Tile tile;
    tile.layers.push_back(std::move(roads));

    const auto geometry =
        std::make_shared<const TileGeometry>(map_render::tessellate(tile, style));
    check(geometry->layerIndexCount(MapLayer::RoadPrimary) > 0, "the road tessellated");

    const QImage& frame =
        renderer->render(projection, { GpuBatch { id, geometry } }, style, background);
    if (frame.isNull())
    {
        return;
    }

    // The row the road actually landed on, found rather than computed: which
    // part of the centre tile is on screen depends on where the camera sits
    // inside it, and the question here is about the road, not the tile.
    int row = -1;
    int widest = 0;
    for (int y = 0; y < frame.height(); ++y)
    {
        int run = 0;
        for (int x = 0; x < frame.width(); ++x)
        {
            if (!near(frame.pixelColor(x, y), background, 6))
            {
                ++run;
            }
        }
        if (run > widest)
        {
            widest = run;
            row = y;
        }
    }

    check(row >= 0 && widest > frame.width() / 4,
          "the road is on screen and spans a good part of it, widest run " +
              std::to_string(widest));
    if (row < 0)
    {
        return;
    }

    // Now walk that row and require the covered pixels to be CONTIGUOUS. Half a
    // quad draws a wedge: present at one end of the segment, gone at the other,
    // which shows up here as the run breaking into pieces.
    int runs = 0;
    bool inRun = false;
    for (int x = 0; x < frame.width(); ++x)
    {
        const bool covered = !near(frame.pixelColor(x, row), background, 6);
        if (covered && !inRun)
        {
            ++runs;
        }
        inRun = covered;
    }

    check(runs == 1, "the road is one unbroken band, not " + std::to_string(runs) +
                         " pieces -- a degenerate triangle in a quad shows up here");
}

// ============================================================================
// The highlight pass
// ============================================================================

// A straight primary road across the middle of the viewport, stamped with an
// OSM way id so the tessellator records the FeatureRange the highlight pass
// joins on. Shared by every highlight test below.
std::shared_ptr<const TileGeometry> roadTileWithWayId(const Projection& projection,
                                                      const TileId& id, const MapStyle_t& style,
                                                      std::uint64_t wayId)
{
    const auto origin = projection.tileOrigin(id);
    const double tileSize = projection.tileScreenSize(id.z);
    const double localY = ((kHeight / 2.0) - origin.y) / tileSize;
    const auto roadY = std::int32_t(std::lround(localY * 4096.0));

    mvt::Layer roads;
    roads.name = "transportation";
    roads.extent = 4096;
    roads.keys = { "class" };
    roads.values = { mvt::Value(std::in_place_type<std::string>, "primary") };
    mvt::Feature road;
    road.type = mvt::GeomType::LineString;
    road.rings.push_back({ { 0, roadY }, { 2048, roadY }, { 4096, roadY } });
    road.tags = { 0, 0 };
    road.hasId = true;
    road.id = wayId;
    roads.features.push_back(std::move(road));

    mvt::Tile tile;
    tile.layers.push_back(std::move(roads));
    return std::make_shared<const TileGeometry>(map_render::tessellate(tile, style));
}

// The row carrying the most non-background pixels -- where the road landed.
int widestCoveredRow(const QImage& frame, const QColor& background)
{
    int row = -1;
    int widest = 0;
    for (int y = 0; y < frame.height(); ++y)
    {
        int run = 0;
        for (int x = 0; x < frame.width(); ++x)
        {
            if (!near(frame.pixelColor(x, y), background, 6))
            {
                ++run;
            }
        }
        if (run > widest)
        {
            widest = run;
            row = y;
        }
    }
    return row;
}

// How many pixels of this column read as the given colour.
int columnRun(const QImage& frame, int x, const QColor& colour)
{
    int run = 0;
    for (int y = 0; y < frame.height(); ++y)
    {
        if (near(frame.pixelColor(x, y), colour, 12))
        {
            ++run;
        }
    }
    return run;
}

void test_a_half_faded_tile_blends_with_the_background()
{
    // The crossfade rides the per-tile uniform: at alpha 0.5 a solid fill
    // must come out as an even mix of its colour and the background, with the
    // vertex buffer untouched.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    const QColor background(0x10, 0x10, 0x10);
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);
    const auto geometry = fullTileQuad(MapLayer::Water, 0.2f, 0.4f, 0.8f);

    const QImage solid =
        renderer->render(projection, { GpuBatch { id, geometry, 1.0F } }, style, background)
            .copy();
    const std::uint64_t uploadsAfterSolid = renderer->stats().uploads;
    const QImage faded =
        renderer->render(projection, { GpuBatch { id, geometry, 0.5F } }, style, background)
            .copy();
    check(renderer->stats().uploads == uploadsAfterSolid,
          "a fade is a uniform write, never a re-upload");

    const QColor full = solid.pixelColor(kWidth / 2, kHeight / 2);
    const QColor half = faded.pixelColor(kWidth / 2, kHeight / 2);
    const auto mix = [](int a, int b) { return (a + b) / 2; };
    const QColor expected(mix(full.red(), background.red()), mix(full.green(), background.green()),
                          mix(full.blue(), background.blue()));
    check(near(half, expected, 8),
          "half alpha lands halfway between the tile's colour and the background");
}

void test_fade_state_is_part_of_the_memo_key()
{
    // A fading tile makes every frame a fresh picture. Miss it in the key and
    // the fade freezes on its first frame -- pop-in with extra steps.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    const QColor background(0x10, 0x10, 0x10);
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);
    const auto geometry = fullTileQuad(MapLayer::Water, 0.2f, 0.4f, 0.8f);

    renderer->render(projection, { GpuBatch { id, geometry, 0.5F } }, style, background);
    const std::uint64_t afterFirst = renderer->stats().reused;

    renderer->render(projection, { GpuBatch { id, geometry, 0.5F } }, style, background);
    check(renderer->stats().reused == afterFirst + 1, "an unchanged fade is served from the memo");

    renderer->render(projection, { GpuBatch { id, geometry, 0.6F } }, style, background);
    check(renderer->stats().reused == afterFirst + 1, "a fade step redraws");

    // Below the quantisation step nothing any pixel could show has changed,
    // so the memo must hold -- otherwise a slow fade renders at tick rate for
    // frames that are byte-identical.
    renderer->render(projection, { GpuBatch { id, geometry, 0.6001F } }, style, background);
    check(renderer->stats().reused == afterFirst + 2,
          "a change smaller than the quantisation step does not");
}

void test_a_highlighted_road_is_recoloured()
{
    // The whole point of the highlight pass: the road the vehicle is on comes
    // back from the matcher as an OSM way id, map_build stamps that id on the
    // tile feature, and the pass recolours the geometry already on the GPU.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    style.widths.road_primary = 8.0;
    style.road_width_scale = 2.0;
    const QColor background(0x16, 0x18, 0x1d);
    const QColor magenta(0xff, 0x00, 0xff);

    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);
    const auto geometry = roadTileWithWayId(projection, id, style, 42);
    check(!geometry->roads.empty(), "the way id was recorded for the join");

    const QImage plain =
        renderer->render(projection, { GpuBatch { id, geometry } }, style, background).copy();
    const int row = widestCoveredRow(plain, background);
    check(row >= 0, "the road is on screen");
    if (row < 0)
    {
        return;
    }
    check(!near(plain.pixelColor(kWidth / 2, row), magenta, 40),
          "unhighlighted, the road wears its own colour");

    const QImage lit = renderer
                           ->render(projection, { GpuBatch { id, geometry } }, style, background,
                                    GpuRenderer::Highlight { { 42 }, magenta, 0.0F })
                           .copy();
    check(near(lit.pixelColor(kWidth / 2, row), magenta, 12),
          "highlighted by its way id, the road turns the highlight colour");
}

void test_a_highlight_misses_unknown_way_ids()
{
    // An id no tile feature carries must draw nothing extra -- the pass joins
    // on the sorted road ranges, and a miss is silence, not a stray overlay.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    style.widths.road_primary = 8.0;
    style.road_width_scale = 2.0;
    const QColor background(0x16, 0x18, 0x1d);
    const QColor magenta(0xff, 0x00, 0xff);

    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);
    const auto geometry = roadTileWithWayId(projection, id, style, 42);

    const QImage frame = renderer
                             ->render(projection, { GpuBatch { id, geometry } }, style, background,
                                      GpuRenderer::Highlight { { 99 }, magenta, 4.0F })
                             .copy();
    bool anyMagenta = false;
    for (int y = 0; y < frame.height() && !anyMagenta; ++y)
    {
        for (int x = 0; x < frame.width(); ++x)
        {
            if (near(frame.pixelColor(x, y), magenta, 40))
            {
                anyMagenta = true;
                break;
            }
        }
    }
    check(!anyMagenta, "an unknown way id highlights nothing");
}

void test_the_highlight_widens_by_extra_half_px()
{
    // extraHalfPx is what makes a highlight readable: at exactly the road's
    // width it vanishes into the road. The extra is added after the zoom
    // taper, in the highlight's own vertex stage.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    style.widths.road_primary = 8.0;
    style.road_width_scale = 2.0;
    const QColor background(0x16, 0x18, 0x1d);
    const QColor magenta(0xff, 0x00, 0xff);

    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);
    const auto geometry = roadTileWithWayId(projection, id, style, 42);

    const QImage flush = renderer
                             ->render(projection, { GpuBatch { id, geometry } }, style, background,
                                      GpuRenderer::Highlight { { 42 }, magenta, 0.0F })
                             .copy();
    const QImage cased = renderer
                             ->render(projection, { GpuBatch { id, geometry } }, style, background,
                                      GpuRenderer::Highlight { { 42 }, magenta, 6.0F })
                             .copy();

    const int flushRun = columnRun(flush, kWidth / 2, magenta);
    const int casedRun = columnRun(cased, kWidth / 2, magenta);
    check(flushRun > 0, "the flush highlight is visible at all");
    // 6 px of extra half-width is ~12 px more band; ask for most of it so
    // antialiased edge rows cannot carry the test.
    check(casedRun >= flushRun + 8,
          "the cased highlight is measurably wider: " + std::to_string(flushRun) + " -> " +
              std::to_string(casedRun));
}

void test_a_changed_highlight_invalidates_the_memo()
{
    // The highlight is part of the frame key. Matching on it wrongly freezes
    // the lit road in place while the vehicle drives off it -- which reads as
    // the matcher being stuck, not the renderer.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    style.widths.road_primary = 8.0;
    style.road_width_scale = 2.0;
    const QColor background(0x16, 0x18, 0x1d);
    const QColor magenta(0xff, 0x00, 0xff);

    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);
    const auto geometry = roadTileWithWayId(projection, id, style, 42);
    const std::vector<GpuBatch> batches { GpuBatch { id, geometry } };
    const GpuRenderer::Highlight lit { { 42 }, magenta, 4.0F };

    renderer->render(projection, batches, style, background, lit);
    const std::uint64_t afterFirst = renderer->stats().reused;

    renderer->render(projection, batches, style, background, lit);
    check(renderer->stats().reused == afterFirst + 1,
          "an identical highlight is served from the memo");

    renderer->render(projection, batches, style, background,
                     GpuRenderer::Highlight { { 43 }, magenta, 4.0F });
    check(renderer->stats().reused == afterFirst + 1, "a different way id redraws");

    renderer->render(projection, batches, style, background,
                     GpuRenderer::Highlight { { 43 }, magenta, 8.0F });
    check(renderer->stats().reused == afterFirst + 1, "and so does a wider casing");
}

// Each tile's indices are TILE-LOCAL, and drawIndexed() is handed that tile's
// base vertex to add. Get that wrong and every tile draws tile zero's geometry.
//
// Invisible unless the tiles differ: the existing multi-tile check gives every
// tile the same quad, so indexing into the wrong one produces the same picture.
// Here neighbouring tiles carry different COLOURS, so drawing the wrong tile's
// vertices shows up as the wrong colour on screen.
void test_each_tile_draws_its_own_vertices()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 13.0, 0.0 },
                                kWidth, kHeight);

    auto tiles = projection.visibleTiles(13, 0);
    projection.sortCentreOutward(tiles);
    if (tiles.size() < 2)
    {
        return;
    }

    // Green first, then red for everything else. Whichever tile is drawn at the
    // viewport centre must show ITS colour, and the two must not agree.
    std::vector<GpuBatch> batches;
    for (std::size_t i = 0; i < tiles.size(); ++i)
    {
        batches.push_back(GpuBatch {
            tiles[i], i == 0 ? fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f)
                             : fullTileQuad(MapLayer::Water, 1.0f, 0.0f, 0.0f) });
    }

    const QImage& frame = renderer->render(projection, batches, style, background);
    if (frame.isNull())
    {
        return;
    }

    // The centre-most tile is first in the batch and is the green one, so the
    // centre pixel is green -- unless a tile drew someone else's vertices.
    const QColor centre = frame.pixelColor(kWidth / 2, kHeight / 2);
    check(near(centre, QColor(0, 255, 0)),
          "the centre tile draws its own green, got " + describe(centre));

    // And somewhere out at the edge, a different tile's red. If every tile were
    // indexing tile zero's vertices, the whole frame would be green.
    bool sawRed = false;
    for (int x = 0; x < frame.width() && !sawRed; ++x)
    {
        for (int y = 0; y < frame.height() && !sawRed; ++y)
        {
            sawRed = near(frame.pixelColor(x, y), QColor(255, 0, 0));
        }
    }
    check(sawRed, "and its neighbours draw their own red");
}

// The overpass, end to end and in pixels.
//
// A minor road crossing a motorway on a bridge. Before `brunnel` reached the
// tiles there was no way to draw this correctly: RoadMinor is drawn before
// Motorway, so the motorway painted over the bridge and the overpass read as
// the road underneath. Nothing in the archive said which was on top.
//
// Checked at the crossing pixel, which is the one place the two disagree.
void test_an_overpass_draws_over_the_road_it_crosses()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    style.road_width_scale = 2.0;
    const QColor background(0x16, 0x18, 0x1d);

    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);
    const TileId id = centreTile(projection);

    // Put the crossing where the camera is, so it is definitely on screen: a
    // z14 tile is 512 px and the viewport is smaller than that.
    const auto origin = projection.tileOrigin(id);
    const double tileSize = projection.tileScreenSize(id.z);
    const double localX = ((kWidth / 2.0) - origin.x) / tileSize;
    const double localY = ((kHeight / 2.0) - origin.y) / tileSize;
    check(localX > 0.05 && localX < 0.95 && localY > 0.05 && localY < 0.95,
          "the camera sits well inside the tile it picked");
    const auto crossX = std::int32_t(std::lround(localX * 4096.0));
    const auto crossY = std::int32_t(std::lround(localY * 4096.0));

    mvt::Layer roads;
    roads.name = "transportation";
    roads.extent = 4096;
    roads.keys = { "class", "brunnel" };
    roads.values = { mvt::Value(std::in_place_type<std::string>, "motorway"),
                     mvt::Value(std::in_place_type<std::string>, "minor"),
                     mvt::Value(std::in_place_type<std::string>, "bridge") };

    // A motorway at grade, running east-west through the crossing.
    mvt::Feature motorway;
    motorway.type = mvt::GeomType::LineString;
    motorway.rings.push_back({ { 0, crossY }, { 4096, crossY } });
    motorway.tags = { 0, 0 };
    roads.features.push_back(std::move(motorway));

    // A minor road on a bridge, running north-south through the same point.
    mvt::Feature bridge;
    bridge.type = mvt::GeomType::LineString;
    bridge.rings.push_back({ { crossX, 0 }, { crossX, 4096 } });
    bridge.tags = { 0, 1, 1, 2 };
    roads.features.push_back(std::move(bridge));

    mvt::Tile tile;
    tile.layers.push_back(std::move(roads));

    const auto geometry =
        std::make_shared<const TileGeometry>(map_render::tessellate(tile, style));
    check(geometry->layerIndexCount(MapLayer::Motorway) > 0, "the motorway is at grade");
    check(geometry->layerIndexCount(MapLayer::RoadBridge) > 0, "the minor road is on the bridge");

    const QImage& frame =
        renderer->render(projection, { GpuBatch { id, geometry } }, style, background);
    if (frame.isNull())
    {
        return;
    }

    // At the crossing the BRIDGE is what shows. The motorway is the loud
    // orange here, so getting this backwards is unmistakable.
    const QColor centre = frame.pixelColor(kWidth / 2, kHeight / 2);
    const QColor deck(0x2b, 0x30, 0x38);       // style.road_minor
    const QColor motorwayFill(0xd9, 0xa4, 0x41);

    check(near(centre, deck, 12),
          "the bridge deck is what shows at the crossing, got " + describe(centre));
    check(!near(centre, motorwayFill, 40),
          "and not the motorway it passes over, got " + describe(centre));

    // Away from the crossing the motorway is still a motorway -- the bridge
    // must not have cut a hole in it.
    bool sawMotorway = false;
    for (int x = 0; x < frame.width() && !sawMotorway; ++x)
    {
        sawMotorway = near(frame.pixelColor(x, kHeight / 2), motorwayFill, 40);
    }
    check(sawMotorway, "the motorway still draws either side of the overpass");
}

void test_many_tiles_still_render()
{
    // Every tile carries its own uniform block at a dynamic offset, and the
    // stride is the hardware's alignment rather than sizeof(the struct). A wrong
    // stride draws every tile at tile zero's transform -- which looks like one
    // tile rendering and the rest missing.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 12.0, 0.0 },
                                kWidth, kHeight);

    std::vector<GpuBatch> batches;
    for (const TileId& id : projection.visibleTiles(12, 2))
    {
        batches.push_back(GpuBatch { id, fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) });
    }
    check(batches.size() > 4, "the test really does span several tiles, got " +
                                  std::to_string(batches.size()));

    const QImage& frame = renderer->render(projection, batches, style, background);
    if (frame.isNull())
    {
        return;
    }

    // Full-tile quads over every visible tile: the viewport is covered edge to
    // edge. Any corner still showing background means a tile drew somewhere else.
    for (const auto corner : { QPoint(2, 2), QPoint(kWidth - 3, 2), QPoint(2, kHeight - 3),
                               QPoint(kWidth - 3, kHeight - 3), QPoint(kWidth / 2, kHeight / 2) })
    {
        const QColor pixel = frame.pixelColor(corner);
        check(near(pixel, QColor(0, 255, 0)),
              "every tile lands at its own transform -- (" + std::to_string(corner.x()) + "," +
                  std::to_string(corner.y()) + ") is covered, got " + describe(pixel));
    }

    check(renderer->stats().tiles == int(batches.size()), "and all of them were drawn");
}

void test_a_tile_with_no_geometry_is_harmless()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                                kWidth, kHeight);

    // An empty tile and a null geometry pointer -- both happen in the widget: an
    // ocean tile has nothing in it, and a tile can be in the visible set before
    // its response has arrived.
    std::vector<GpuBatch> batches {
        GpuBatch { centreTile(projection), std::make_shared<TileGeometry>() },
        GpuBatch { TileId { 14, 1, 1 }, nullptr },
    };

    const QImage& frame = renderer->render(projection, batches, style, background);
    check(!frame.isNull(), "an empty and a null batch render without crashing");
    if (!frame.isNull())
    {
        check(near(frame.pixelColor(kWidth / 2, kHeight / 2), background),
              "and leave the background showing");
    }
}

void test_an_unchanged_frame_is_not_drawn_again()
{
    // A widget repaints for reasons that have nothing to do with the map --
    // a sibling widget updating, an expose event. Redrawing an identical image
    // and reading it back is the entire frame cost for no change at all.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        check(false, "a GPU backend comes up");
        return;
    }

    MapStyle_t style;
    const QColor background("#0d0f13");
    const Camera camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 };
    const Projection projection(camera, kWidth, kHeight);

    std::vector<GpuBatch> batches;
    batches.push_back(GpuBatch { centreTile(projection),
                                 fullTileQuad(MapLayer::Water, 0.1f, 0.3f, 0.8f) });

    const QImage first = renderer->render(projection, batches, style, background).copy();
    check(!first.isNull(), "the first frame draws");
    const std::uint64_t afterFirst = renderer->stats().reused;

    const QImage second = renderer->render(projection, batches, style, background).copy();
    check(renderer->stats().reused == afterFirst + 1,
          "an identical second call is served from the memo");
    check(first == second, "and hands back the same pixels");

    // Every input in the key must invalidate it, or the map freezes in a way
    // that looks like the bus having stopped.
    const Projection moved(Camera { Coordinate { kIrvineLat + 0.01, kIrvineLon }, 14.0, 0.0 },
                           kWidth, kHeight);
    renderer->render(moved, batches, style, background);
    check(renderer->stats().reused == afterFirst + 1, "a camera move redraws");

    std::vector<GpuBatch> newGeometry;
    newGeometry.push_back(GpuBatch { batches[0].id,
                                     fullTileQuad(MapLayer::Water, 0.8f, 0.2f, 0.1f) });
    renderer->render(projection, newGeometry, style, background);
    check(renderer->stats().reused == afterFirst + 1, "and so does a tile arriving");

    MapStyle_t wider = style;
    wider.road_width_scale = style.road_width_scale * 2.0;
    renderer->render(projection, newGeometry, wider, background);
    check(renderer->stats().reused == afterFirst + 1, "and so does a style change");
}

void test_a_hidpi_frame_is_rendered_at_device_resolution()
{
    // THE HiDPI bug. The widget built its projection from logical width and
    // height and blitted the result as it came, so on a 2x screen the geometry
    // was rendered at half the resolution the screen has and upscaled -- while
    // the label and marker passes, which go through QPainter, were drawn at
    // full resolution over the top. Soft roads under sharp text, which reads as
    // a styling problem rather than a resolution one.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Camera camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 };

    // Square and larger than the rest of this file, so a 512 px tile's own
    // edges land inside the frame and a misplaced draw has somewhere to show.
    constexpr int kSize = 600;
    const Projection logical(camera, kSize, kSize);
    const Projection retina(camera, kSize, kSize, 2.0);

    check(retina.viewportWidth() == double(kSize) && retina.viewportHeight() == double(kSize),
          "the projection stays LOGICAL -- the labels and the marker are drawn from it");

    const TileId id = centreTile(logical);
    check(centreTile(retina) == id,
          "and the ratio does not change which tiles are wanted: the same screen shows the "
          "same map, only with more pixels in it");

    // The tile's northern half only, so the boundary between drawn and empty is
    // somewhere a shifted or mis-scaled draw cannot hide.
    auto half = std::make_shared<TileGeometry>();
    const auto vertex = [](float x, float y) {
        return MapVertex { x, y, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f };
    };
    half->vertices = { vertex(0.0f, 0.0f), vertex(1.0f, 0.0f), vertex(1.0f, 0.5f),
                       vertex(0.0f, 0.5f) };
    half->indices = { 0, 1, 2, 0, 2, 3 };
    for (std::size_t i = 0; i <= map_render::kMapLayerCount; ++i)
    {
        half->layerStart[i] = (i <= std::size_t(MapLayer::Water)) ? 0U : 4U;
        half->layerIndexStart[i] = (i <= std::size_t(MapLayer::Water)) ? 0U : 6U;
    }
    const std::vector<GpuBatch> batches { GpuBatch { id, half } };

    const QImage plain = renderer->render(logical, batches, style, background).copy();
    const QImage sharp = renderer->render(retina, batches, style, background).copy();
    if (plain.isNull() || sharp.isNull())
    {
        check(false, "both frames render");
        return;
    }

    check(plain.width() == kSize && plain.height() == kSize,
          "a 1x screen still gets a 1x frame, got " + std::to_string(plain.width()) + "x" +
              std::to_string(plain.height()));
    check(sharp.width() == kSize * 2 && sharp.height() == kSize * 2,
          "a 2x screen gets twice the pixels in each direction, got " +
              std::to_string(sharp.width()) + "x" + std::to_string(sharp.height()));

    // Without this the widget's blit would treat the frame's device pixels as
    // logical ones and cover four times the widget, which is a far louder
    // failure than the soft map it replaces -- but a failure all the same.
    check(std::abs(sharp.devicePixelRatio() - 2.0) < 1e-9,
          "and the frame carries the ratio it was rendered at, got " +
              std::to_string(sharp.devicePixelRatio()));
    check(sharp.deviceIndependentSize() == QSizeF(kSize, kSize),
          "so it still blits across the logical rectangle it was asked for");

    // Same map, more pixels: every logical pixel of the 1x frame must match the
    // device pixel it maps to. A projection that forgot the ratio anywhere --
    // the tile origin, the tile size -- draws the tile at half its place and
    // half its size, and misses this everywhere at once.
    int sampled = 0;
    int mismatched = 0;
    for (int y = 4; y < kSize; y += 17)
    {
        for (int x = 4; x < kSize; x += 17)
        {
            ++sampled;
            if (!near(plain.pixelColor(x, y), sharp.pixelColor(x * 2, y * 2)))
            {
                ++mismatched;
            }
        }
    }

    check(sampled > 500, "the comparison covers the frame, got " + std::to_string(sampled) +
                             " samples");
    // A few samples sit on the quad's own edge, where the 2x frame legitimately
    // resolves what the 1x one blurred. That is one row out of thirty-odd.
    check(mismatched <= sampled / 20,
          "the 2x frame is the same map at higher resolution, got " +
              std::to_string(mismatched) + " of " + std::to_string(sampled) +
              " samples differing");
}

void test_a_viewport_too_wide_for_a_texture_comes_back_soft_not_blank()
{
    // Rendering at the device ratio halves the logical viewport a texture can
    // hold, so the ceiling is now something a very wide widget could reach.
    // Crossing it must not be a blank map: ensureTarget() refuses the texture,
    // render() returns null, and the widget fills its background with no
    // diagnostic that says why -- so the ratio is lowered until the frame fits
    // and the map merely loses sharpness.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    const MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Camera camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 };

    // Wide and short, so the clamp bites on one axis without asking for a
    // 64-megapixel readback to prove it.
    constexpr double kWide = 5000.0;
    constexpr double kShort = 200.0;
    const Projection projection(camera, kWide, kShort, 4.0);

    const std::vector<GpuBatch> batches {
        GpuBatch { centreTile(projection), fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) }
    };

    const QImage& frame = renderer->render(projection, batches, style, background);
    check(!frame.isNull(), "a viewport past the texture limit still renders");
    if (frame.isNull())
    {
        return;
    }

    const double ratio = frame.devicePixelRatio();
    check(ratio > 1.0 && ratio < 4.0,
          "at a ratio between 1x and what the screen asked for, got " + std::to_string(ratio));
    check(frame.width() <= 8192 && frame.height() <= 8192,
          "with the texture inside what the backend will allocate, got " +
              std::to_string(frame.width()) + "x" + std::to_string(frame.height()));
    check(std::abs(frame.deviceIndependentSize().width() - kWide) < 1.0 &&
              std::abs(frame.deviceIndependentSize().height() - kShort) < 1.0,
          "and still covering the whole widget, just upscaled");
    check(std::abs(renderer->stats().devicePixelRatio - ratio) < 1e-9,
          "and status() can say so -- a soft map has no other evidence");
}

void test_line_widths_stay_logical_pixels()
{
    // A width is in SCREEN pixels and the vertices carry it unscaled -- see
    // halfWidthFor(). Rendering at 2x without also scaling the width uniform
    // leaves every road the same DEVICE width, which is half the road it should
    // be: a HiDPI map whose geometry is sharp and whose roads are hairlines.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    // Wide, so the stripe is tens of pixels across and the antialiased fringe
    // at its edges is noise rather than most of the measurement.
    style.road_width_scale = 4.0;

    const QColor background(0x16, 0x18, 0x1d);
    const Camera camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 };

    constexpr int kSize = 600;
    const Projection logical(camera, kSize, kSize);
    const Projection retina(camera, kSize, kSize, 2.0);

    const std::vector<GpuBatch> batches {
        GpuBatch { centreTile(logical), fullTileStripe(map_render::halfWidthFor(
                                            MapLayer::Motorway, style)) }
    };

    const QImage plain = renderer->render(logical, batches, style, background).copy();
    const QImage sharp = renderer->render(retina, batches, style, background).copy();
    if (plain.isNull() || sharp.isNull())
    {
        check(false, "both frames render");
        return;
    }

    const int plainPixels = greenPixels(plain);
    const int sharpPixels = greenPixels(sharp);
    check(plainPixels > 5000,
          "the stripe is actually on screen at 1x, got " + std::to_string(plainPixels) + " pixels");
    if (plainPixels == 0)
    {
        return;
    }

    // Four times the area: twice as long AND twice as wide. Getting the width
    // wrong and the length right lands on two, which is the whole point of
    // measuring area rather than checking that the line is there at all.
    const double ratio = double(sharpPixels) / double(plainPixels);
    check(ratio > 3.7 && ratio < 4.3,
          "the same road covers four times the device pixels at 2x -- so it is the same width "
          "on screen -- got " +
              std::to_string(ratio) + "x (" + std::to_string(plainPixels) + " then " +
              std::to_string(sharpPixels) + ")");
}

} // namespace

void test_a_pitched_frame_agrees_with_the_projection()
{
    // The GPU applies the tilt as a matrix; the Projection applies it as
    // arithmetic. If they disagree, everything drawn by QPainter through
    // screenFor() -- the marker, the trail -- floats off the GPU's map.
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    const Projection projection(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0, 50.0 },
                                kWidth, kHeight);
    const QColor background(0x16, 0x18, 0x1d);

    const TileId tile = centreTile(projection);
    std::vector<GpuBatch> batches { GpuBatch {
        tile, fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) } };

    const QImage& frame = renderer->render(projection, batches, style, background);
    check(!frame.isNull(), "the pitched frame renders");
    if (frame.isNull())
    {
        return;
    }

    // The tile's top edge, as the PROJECTION places it. The GPU's pixels must
    // change from green to background within a couple of rows of that line.
    const double side = std::exp2(double(tile.z));
    const auto topEdge = projection.screenFor(
        map_render::WorldPoint { (double(tile.x) + 0.5) / side, double(tile.y) / side });
    check(topEdge.y > 0.0 && topEdge.y < kHeight, "the tile's top edge is inside the viewport");

    const int x = int(std::lround(topEdge.x));
    const QColor above = frame.pixelColor(x, int(std::lround(topEdge.y)) - 3);
    const QColor below = frame.pixelColor(x, int(std::lround(topEdge.y)) + 3);
    check(near(above, background), "just above the projected edge is background, got " +
                                        describe(above));
    check(near(below, QColor(0, 255, 0)), "just below it is the tile, got " + describe(below));

    // Foreshortening, in pixels: the edge is up-screen of the centre, and the
    // tilt compresses everything up-screen toward the horizon -- so it must
    // land CLOSER to the centre line than the flat camera put it.
    const Projection flat(Camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0 },
                          kWidth, kHeight);
    const auto flatEdge = flat.screenFor(
        map_render::WorldPoint { (double(tile.x) + 0.5) / side, double(tile.y) / side });
    check(topEdge.y > flatEdge.y, "the tilted top edge is foreshortened toward the centre");
}

void test_pitch_is_part_of_the_memo_key()
{
    auto renderer = GpuRenderer::create();
    if (!renderer)
    {
        return;
    }

    MapStyle_t style;
    const QColor background(0x16, 0x18, 0x1d);
    const Camera camera { Coordinate { kIrvineLat, kIrvineLon }, 14.0, 0.0, 0.0 };
    const Projection flat(camera, kWidth, kHeight);
    std::vector<GpuBatch> batches { GpuBatch {
        centreTile(flat), fullTileQuad(MapLayer::Water, 0.0f, 1.0f, 0.0f) } };

    renderer->render(flat, batches, style, background);
    renderer->render(flat, batches, style, background);
    const std::uint64_t afterRepeat = renderer->stats().reused;
    check(afterRepeat >= 1, "an unchanged flat frame is served from the memo");

    Camera pitched = camera;
    pitched.pitch = 45.0;
    renderer->render(Projection(pitched, kWidth, kHeight), batches, style, background);
    check(renderer->stats().reused == afterRepeat,
          "tilting the camera is a new picture, not a memo hit");
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    // QGuiApplication rather than QApplication: this test touches no widgets, and
    // QRhi needs the platform integration up before a backend can be created.
    QGuiApplication app(argc, argv);

    test_a_backend_comes_up_with_no_window();
    test_an_unchanged_frame_is_not_drawn_again();
    test_an_empty_frame_is_the_background_colour();
    test_geometry_reaches_the_pixels();
    test_layer_order_beats_tile_order();
    test_panning_does_not_reupload();
    test_new_geometry_for_the_same_tile_does_reupload();
    test_the_frame_follows_a_resize();
    test_north_is_up_and_the_image_is_not_flipped();
    test_a_fill_is_never_faded_by_the_line_floor();
    test_a_hairline_road_never_disappears();
    test_widening_a_hairline_does_not_add_ink();
    test_a_normal_width_line_is_unaffected();
    test_a_fill_is_never_faded_by_the_line_floor();
    test_a_tessellated_road_draws_as_a_continuous_band();
    test_a_half_faded_tile_blends_with_the_background();
    test_fade_state_is_part_of_the_memo_key();
    test_a_highlighted_road_is_recoloured();
    test_a_highlight_misses_unknown_way_ids();
    test_the_highlight_widens_by_extra_half_px();
    test_a_changed_highlight_invalidates_the_memo();
    test_an_overpass_draws_over_the_road_it_crosses();
    test_each_tile_draws_its_own_vertices();
    test_many_tiles_still_render();
    test_a_tile_with_no_geometry_is_harmless();
    test_a_hidpi_frame_is_rendered_at_device_resolution();
    test_line_widths_stay_logical_pixels();
    test_a_viewport_too_wide_for_a_texture_comes_back_soft_not_blank();
    test_a_pitched_frame_agrees_with_the_projection();
    test_pitch_is_part_of_the_memo_key();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all GPU renderer checks passed");
    return 0;
}
