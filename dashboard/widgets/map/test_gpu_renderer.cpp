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

#include "map/gpu_renderer.h"
#include "map/tessellator.h"

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

using map_widget::Camera;
using map_widget::Coordinate;
using map_widget::GpuBatch;
using map_widget::GpuRenderer;
using map_widget::MapLayer;
using map_widget::MapVertex;
using map_widget::Projection;
using map_widget::TileGeometry;
using map_widget::TileId;

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

    // Tile-local [0,1], two triangles.
    geometry->vertices = { vertex(0.0f, 0.0f), vertex(1.0f, 0.0f), vertex(1.0f, 1.0f),
                           vertex(0.0f, 0.0f), vertex(1.0f, 1.0f), vertex(0.0f, 1.0f) };

    // Everything before `layer` starts at 0 and everything from it on ends at 6.
    for (std::size_t i = 0; i <= map_widget::kMapLayerCount; ++i)
    {
        geometry->layerStart[i] = (i <= std::size_t(layer)) ? 0U : 6U;
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

    geometry->vertices = { vertex(0.0f, -1.0f), vertex(1.0f, -1.0f), vertex(1.0f, 1.0f),
                           vertex(0.0f, -1.0f), vertex(1.0f, 1.0f),  vertex(0.0f, 1.0f) };

    for (std::size_t i = 0; i <= map_widget::kMapLayerCount; ++i)
    {
        geometry->layerStart[i] = (i <= std::size_t(MapLayer::Motorway)) ? 0U : 6U;
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
    check(stats.vertices == 6, "and six vertices, got " + std::to_string(stats.vertices));
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
    half->vertices = { vertex(0.0f, 0.0f),  vertex(1.0f, 0.0f), vertex(1.0f, 0.5f),
                       vertex(0.0f, 0.0f),  vertex(1.0f, 0.5f), vertex(0.0f, 0.5f) };
    for (std::size_t i = 0; i <= map_widget::kMapLayerCount; ++i)
    {
        half->layerStart[i] = (i <= std::size_t(MapLayer::Water)) ? 0U : 6U;
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
                       vertex(0.0f, 0.0f), vertex(1.0f, 0.5f), vertex(0.0f, 0.5f) };
    for (std::size_t i = 0; i <= map_widget::kMapLayerCount; ++i)
    {
        half->layerStart[i] = (i <= std::size_t(MapLayer::Water)) ? 0U : 6U;
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
        GpuBatch { centreTile(logical), fullTileStripe(map_widget::halfWidthFor(
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
    test_many_tiles_still_render();
    test_a_tile_with_no_geometry_is_harmless();
    test_a_hidpi_frame_is_rendered_at_device_resolution();
    test_line_widths_stay_logical_pixels();
    test_a_viewport_too_wide_for_a_texture_comes_back_soft_not_blank();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all GPU renderer checks passed");
    return 0;
}
