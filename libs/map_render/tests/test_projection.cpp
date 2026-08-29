// SPDX-License-Identifier: GPL-3.0-or-later
//
// Web Mercator and the tile arithmetic.
//
// Pure maths, so this runs with no display, no bus and no tiles. It is also the
// only place these numbers get checked at all: every mistake possible here
// draws a perfectly good map in the wrong place, and a screenshot of a map you
// have not been to before looks exactly like a screenshot of the right one.
//
// The anchor is Irvine, CA at z14. The expected tile -- 2828/6562 -- was worked
// out from the Web Mercator formula by hand and then confirmed independently:
// libs/mvt's real-tile test pulls that same tile out of the SoCal archive and
// finds a city's worth of roads in it. A wrong projection here would name a
// tile that is empty ocean.

#include "map_render/projection.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <unordered_set>
#include <cmath>
#include <string>

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

bool near(double a, double b, double tolerance)
{
    return std::abs(a - b) <= tolerance;
}

using map_render::Camera;
using map_render::Coordinate;
using map_render::Projection;
using map_render::ScreenPoint;
using map_render::kMaxTileZoom;
using map_render::substituteTiles;

// The old signature took a vector<bool>; the predicate is what callers hold
// naturally. These adapt the tests' literals.
std::function<bool(std::size_t)> haveOf(std::vector<bool> flags)
{
    return [held = std::move(flags)](std::size_t i) { return held[i]; };
}
const auto haveNone = [](std::size_t) { return false; };
using map_render::TileId;
using map_render::TileIdHash;
using map_render::WorldPoint;

constexpr Coordinate kIrvine { 33.6865966, -117.8557874 };

// ============================================================================
// World coordinates
// ============================================================================

void test_the_anchors_of_the_projection()
{
    // Null island is the middle of the world in both axes.
    const auto origin = map_render::worldFor(Coordinate { 0.0, 0.0 });
    check(near(origin.x, 0.5, 1e-12), "longitude 0 is world x = 0.5");
    check(near(origin.y, 0.5, 1e-12), "latitude 0 is world y = 0.5");

    // The corners. West and north are 0; east and south are 1.
    const auto northWest = map_render::worldFor(Coordinate { map_render::kMaxLatitude, -180.0 });
    check(near(northWest.x, 0.0, 1e-12), "longitude -180 is world x = 0");
    check(near(northWest.y, 0.0, 1e-9), "the northern Mercator limit is world y = 0");

    const auto southEast =
        map_render::worldFor(Coordinate { -map_render::kMaxLatitude, 179.9999999 });
    check(southEast.x > 0.999, "longitude +180 is world x = 1");
    check(near(southEast.y, 1.0, 1e-9), "the southern Mercator limit is world y = 1");

    // y is NOT linear in latitude -- that is the whole point of Mercator, and
    // a projection that used lat/180 would put everything at the right
    // longitude and the wrong latitude.
    const auto at45 = map_render::worldFor(Coordinate { 45.0, 0.0 });
    check(at45.y < 0.5, "45N is north of the equator");
    check(!near(at45.y, 0.5 - (45.0 / 180.0), 1e-3),
          "and is NOT where a linear projection would put it");
    check(near(at45.y, 0.35972503, 1e-6), "but at the Mercator y for 45 degrees");
}

void test_world_coordinates_round_trip()
{
    const Coordinate places[] = {
        kIrvine,
        { 0.0, 0.0 },
        { 51.5074, -0.1278 },    // London
        { -33.8688, 151.2093 },  // Sydney, southern and eastern
        { 71.0, -8.0 },          // high northern latitude
        { -54.8, -68.3 },        // high southern latitude
    };

    for (const Coordinate& place : places)
    {
        const auto world = map_render::worldFor(place);
        const auto back = map_render::coordinateFor(world);
        check(near(back.latitude, place.latitude, 1e-9),
              "latitude round-trips: " + std::to_string(place.latitude));
        check(near(back.longitude, place.longitude, 1e-9),
              "longitude round-trips: " + std::to_string(place.longitude));
    }
}

void test_latitude_is_clamped_and_longitude_wraps()
{
    // Mercator's tan() runs to infinity at the poles. Unclamped, the pole
    // projects to infinity and every subsequent arithmetic yields NaN -- which
    // paints as nothing at all, silently.
    const auto northPole = map_render::worldFor(Coordinate { 90.0, 0.0 });
    check(std::isfinite(northPole.y), "the north pole projects to a finite y");
    // To within floating point: kMaxLatitude is a decimal rounding of
    // atan(sinh(pi)), so the projected edge lands a few ulp either side of
    // exactly 0. What matters is that it is bounded, not that it is exact.
    check(northPole.y >= -1e-9 && northPole.y <= 1.0 + 1e-9, "inside the world");

    const auto southPole = map_render::worldFor(Coordinate { -90.0, 0.0 });
    check(std::isfinite(southPole.y), "the south pole projects to a finite y");

    check(near(map_render::clampLatitude(90.0), map_render::kMaxLatitude, 1e-9),
          "latitude is clamped to the Mercator limit");

    // Longitudes wrap; clamping one would stop a camera dragged past the date
    // line rather than carrying it round.
    check(near(map_render::wrapLongitude(-190.0), 170.0, 1e-9), "-190 wraps to 170");
    check(near(map_render::wrapLongitude(190.0), -170.0, 1e-9), "190 wraps to -170");
    check(near(map_render::wrapLongitude(540.0), -180.0, 1e-9), "540 wraps to -180");
    check(near(map_render::wrapLongitude(-117.8557874), -117.8557874, 1e-12),
          "a longitude already in range is untouched");
}

// ============================================================================
// Tiles
// ============================================================================

void test_irvine_lands_in_the_expected_tile()
{
    // THE anchor. 2828/6562 at z14 is the tile libs/mvt pulls out of the real
    // archive and finds 2462 roads in. A projection error here names a tile
    // that is empty water and the map draws nothing, with no error anywhere.
    const auto world = map_render::worldFor(kIrvine);
    const double side = std::exp2(14.0);
    const auto x = static_cast<std::uint32_t>(std::floor(world.x * side));
    const auto y = static_cast<std::uint32_t>(std::floor(world.y * side));

    check(x == 2828, "Irvine is in tile column 2828 at z14, got " + std::to_string(x));
    check(y == 6562, "Irvine is in tile row 6562 at z14, got " + std::to_string(y));

    // And the zoom relationship: each level up halves the coordinates.
    const double side13 = std::exp2(13.0);
    check(static_cast<std::uint32_t>(std::floor(world.x * side13)) == 2828 / 2,
          "and in 1414 at z13");
    check(static_cast<std::uint32_t>(std::floor(world.y * side13)) == 6562 / 2,
          "and in row 3281 at z13");
}

void test_the_camera_centre_lands_in_the_middle_of_the_widget()
{
    const Projection projection(Camera { kIrvine, 14.0, 0.0 }, 800.0, 600.0);

    const ScreenPoint centre = projection.screenFor(kIrvine);
    check(near(centre.x, 400.0, 1e-6), "the camera centre is at half the width");
    check(near(centre.y, 300.0, 1e-6), "and half the height");
}

void test_screen_and_world_round_trip()
{
    const Projection projection(Camera { kIrvine, 14.0, 0.0 }, 800.0, 600.0);

    const ScreenPoint points[] = { { 0.0, 0.0 }, { 800.0, 600.0 }, { 123.0, 456.0 } };
    for (const ScreenPoint& point : points)
    {
        const Coordinate coordinate = projection.coordinateForScreen(point);
        const ScreenPoint back = projection.screenFor(coordinate);
        check(near(back.x, point.x, 1e-6) && near(back.y, point.y, 1e-6),
              "screen -> coordinate -> screen round-trips at (" + std::to_string(point.x) + ", " +
                  std::to_string(point.y) + ")");
    }
}

void test_north_is_up_and_east_is_right()
{
    // The sign check. Getting either backwards produces a mirrored map, which
    // is the single most likely projection bug and the hardest to see in a
    // screenshot of unfamiliar terrain.
    const Projection projection(Camera { kIrvine, 12.0, 0.0 }, 800.0, 600.0);

    const ScreenPoint north =
        projection.screenFor(Coordinate { kIrvine.latitude + 0.05, kIrvine.longitude });
    check(north.y < 300.0, "a point to the NORTH is ABOVE the centre");

    const ScreenPoint east =
        projection.screenFor(Coordinate { kIrvine.latitude, kIrvine.longitude + 0.05 });
    check(east.x > 400.0, "a point to the EAST is RIGHT of the centre");
}

void test_zoom_scales_by_powers_of_two()
{
    const Projection near12(Camera { kIrvine, 12.0, 0.0 }, 800.0, 600.0);
    const Projection near13(Camera { kIrvine, 13.0, 0.0 }, 800.0, 600.0);

    check(near(near13.worldPixels(), near12.worldPixels() * 2.0, 1e-6),
          "one zoom level doubles the world's pixel size");

    const Coordinate offset { kIrvine.latitude, kIrvine.longitude + 0.01 };
    const double at12 = near12.screenFor(offset).x - 400.0;
    const double at13 = near13.screenFor(offset).x - 400.0;
    check(near(at13, at12 * 2.0, 1e-6), "and doubles the on-screen distance to a fixed point");
}

void test_tile_zoom_is_rounded_and_clamped()
{
    const auto zoomFor = [](double cameraZoom, std::uint8_t lo, std::uint8_t hi) {
        return Projection(Camera { kIrvine, cameraZoom, 0.0 }, 800.0, 600.0).tileZoom(lo, hi);
    };

    check(zoomFor(14.0, 0, 14) == 14, "an exact zoom uses its own level");
    check(zoomFor(13.9, 0, 14) == 14,
          "13.9 rounds UP to 14 -- truncating would draw z13 at double size and look soft");
    check(zoomFor(13.4, 0, 14) == 13, "13.4 rounds down to 13");

    // Past the archive's coverage, the deepest available tiles are drawn
    // magnified. Requesting z18 from an archive that stops at 14 would ask for
    // tiles that are simply not there, and the map would go blank on zoom-in.
    check(zoomFor(18.0, 0, 14) == 14, "a zoom past maxzoom is clamped to maxzoom");
    check(zoomFor(2.0, 6, 14) == 6, "a zoom below minzoom is clamped to minzoom");
    check(zoomFor(10.0, 14, 6) == 10, "an inverted min/max pair is put in order rather than empty");
}

void test_the_device_pixel_ratio_is_carried_and_not_applied()
{
    // The ratio exists for one consumer -- the GPU pass, which renders into a
    // texture of its own and can afford the screen's real resolution. Every
    // other pass draws through QPainter in logical pixels, so a projection that
    // APPLIED the ratio would put the labels, the marker and the trail at twice
    // their coordinates: a map whose text has slid off the bottom right.
    const Camera camera { kIrvine, 14.0, 0.0 };
    const Projection logical(camera, 800.0, 600.0);
    const Projection retina(camera, 800.0, 600.0, 2.0);

    check(near(logical.devicePixelRatio(), 1.0, 1e-12), "a projection is 1x unless told otherwise");
    check(near(retina.devicePixelRatio(), 2.0, 1e-12), "and carries the ratio it was given");

    // A ratio of zero reaches here from a window system that has not worked out
    // which screen a widget is on yet. Taking it at face value divides the map
    // by nothing.
    check(near(Projection(camera, 800.0, 600.0, 0.0).devicePixelRatio(), 1.0, 1e-12),
          "a non-positive ratio is taken as 1 rather than propagated");

    check(near(retina.viewportWidth(), 800.0, 1e-12) &&
              near(retina.viewportHeight(), 600.0, 1e-12),
          "the viewport stays the logical one");
    check(near(retina.tileScreenSize(14), logical.tileScreenSize(14), 1e-12),
          "and a tile is the same number of logical pixels across");

    const ScreenPoint sameSpot =
        retina.screenFor(Coordinate { kIrvine.latitude + 0.01, kIrvine.longitude + 0.01 });
    const ScreenPoint reference =
        logical.screenFor(Coordinate { kIrvine.latitude + 0.01, kIrvine.longitude + 0.01 });
    check(near(sameSpot.x, reference.x, 1e-9) && near(sameSpot.y, reference.y, 1e-9),
          "so a coordinate lands on exactly the same screen pixel");

    // The trap on the other side: scaling the viewport into device pixels
    // instead of carrying a ratio would cover four times the world at the same
    // zoom, and the widget would fetch four times the tiles to draw the same
    // map.
    check(retina.visibleTiles(14, 1) == logical.visibleTiles(14, 1),
          "and the same screen wants exactly the same tiles at either ratio");
}

// ---------------------------------------------------- standing in for a tile

// A cache stub: whatever ids are put in it are "drawable", nothing else is.
struct FakeCache
{
    std::unordered_set<TileId, TileIdHash> have;

    void add(std::uint8_t z, std::uint32_t x, std::uint32_t y) { have.insert(TileId { z, x, y }); }

    std::function<bool(const TileId&)> predicate() const
    {
        return [this](const TileId& id) { return have.contains(id); };
    }
};

bool contains(const std::vector<TileId>& tiles, std::uint8_t z, std::uint32_t x, std::uint32_t y)
{
    return std::find(tiles.begin(), tiles.end(), TileId { z, x, y }) != tiles.end();
}


// The coarse overview prefetch only pays off if substituteTiles() can actually
// reach the level it fetches. MapWidget fetches four levels down; the
// substitute walk looks five up, and a static_assert ties the two together --
// this is the behavioural half of that.
//
// The scenario is the one the prefetch exists for: ground the drive has NEVER
// visited at any zoom, so the only thing in the cache is the overview.
void test_a_four_level_overview_stands_in_for_ground_never_visited()
{
    // A z14 viewport: four tiles, none of them arrived, and no z13 or z12
    // ancestor in the cache because the camera was never there.
    const std::vector<TileId> wanted {
        { 14, 2828, 6562 }, { 14, 2829, 6562 }, { 14, 2828, 6563 }, { 14, 2829, 6563 }
    };

    FakeCache cache;
    // Only the overview, four levels down: 2828 >> 4 == 176, 6562 >> 4 == 410.
    cache.add(10, 176, 410);

    const auto out = substituteTiles(wanted, haveNone, cache.predicate(), 64);
    check(out.size() == 1,
          "one overview tile covers the whole viewport, got " + std::to_string(out.size()));
    check(contains(out, 10, 176, 410), "and it is the z10 ancestor the prefetch asked for");
}

// One level further than the prefetch fetches, to show the walk is not merely
// finding it by luck -- and one level past the cap, to show where it stops.
void test_the_substitute_walk_reaches_five_levels_and_no_further()
{
    const std::vector<TileId> wanted { { 14, 2828, 6562 } };

    FakeCache atTheLimit;
    atTheLimit.add(9, 2828 >> 5, 6562 >> 5);
    check(substituteTiles(wanted, haveNone, atTheLimit.predicate(), 64).size() == 1,
          "five levels up is still found");

    FakeCache pastTheLimit;
    pastTheLimit.add(8, 2828 >> 6, 6562 >> 6);
    check(substituteTiles(wanted, haveNone, pastTheLimit.predicate(), 64).empty(),
          "six is not, which is why the overview delta may not grow past five");
}

void test_nothing_stands_in_for_a_tile_that_arrived()
{
    // The whole point is filling GAPS. Drawing a stand-in under a tile that is
    // already there is the same ground drawn twice for nothing.
    const std::vector<TileId> wanted { { 14, 2828, 6562 }, { 14, 2829, 6562 } };
    FakeCache cache;
    cache.add(13, 1414, 3281);

    const auto out = substituteTiles(wanted, haveOf({ true, true }), cache.predicate(), 64);
    check(out.empty(), "a fully arrived set needs no stand-ins");
}

void test_one_ancestor_covers_a_tile_and_its_siblings()
{
    // Four children of one parent, all missing. The economy of preferring
    // ancestors is exactly this: one extra draw call, not four.
    const std::vector<TileId> wanted {
        { 14, 2828, 6562 }, { 14, 2829, 6562 }, { 14, 2828, 6563 }, { 14, 2829, 6563 }
    };
    FakeCache cache;
    cache.add(13, 1414, 3281);

    const auto out = substituteTiles(wanted, haveNone, cache.predicate(), 64);
    check(out.size() == 1, "four missing siblings share one ancestor, got " +
                               std::to_string(out.size()));
    check(contains(out, 13, 1414, 3281), "and it is their parent");
}

void test_the_nearest_ancestor_wins()
{
    // The nearest is the least magnified. Taking a further one when a nearer is
    // cached would throw away detail that was already in hand.
    const std::vector<TileId> wanted { { 14, 2828, 6562 } };
    FakeCache cache;
    cache.add(13, 1414, 3281);
    cache.add(12, 707, 1640);

    const auto out = substituteTiles(wanted, haveNone, cache.predicate(), 64);
    check(out.size() == 1 && contains(out, 13, 1414, 3281),
          "the parent is used rather than the grandparent");
}

void test_a_deeper_cache_stands_in_when_there_is_no_ancestor()
{
    // The zoom-OUT case: what you have is deeper than what you asked for.
    // Children PARTITION their parent, so unlike an ancestor they overlay
    // nothing and all four are worth drawing.
    const std::vector<TileId> wanted { { 13, 1414, 3281 } };
    FakeCache cache;
    cache.add(14, 2828, 6562);
    cache.add(14, 2829, 6562);
    cache.add(14, 2828, 6563);
    cache.add(14, 2829, 6563);

    const auto out = substituteTiles(wanted, haveNone, cache.predicate(), 64);
    check(out.size() == 4, "all four children stand in, got " + std::to_string(out.size()));
    check(contains(out, 14, 2828, 6562) && contains(out, 14, 2829, 6563),
          "and they are the right ones");
}

void test_a_partial_deeper_cache_still_helps()
{
    // Half a map beats none: the user asked for exactly this -- "zoomed out and
    // only covering a portion of the window".
    const std::vector<TileId> wanted { { 13, 1414, 3281 } };
    FakeCache cache;
    cache.add(14, 2828, 6562);

    const auto out = substituteTiles(wanted, haveNone, cache.predicate(), 64);
    check(out.size() == 1 && contains(out, 14, 2828, 6562),
          "the one cached child is drawn rather than nothing");
}

void test_an_ancestor_beats_descendants()
{
    // One draw call against four, for the same ground.
    const std::vector<TileId> wanted { { 13, 1414, 3281 } };
    FakeCache cache;
    cache.add(12, 707, 1640);
    cache.add(14, 2828, 6562);
    cache.add(14, 2829, 6562);

    const auto out = substituteTiles(wanted, haveNone, cache.predicate(), 64);
    check(out.size() == 1 && contains(out, 12, 707, 1640),
          "the ancestor is preferred, got " + std::to_string(out.size()) + " tiles");
}

void test_the_budget_is_not_exceeded()
{
    // The renderer draws a bounded number of tiles and truncates the TAIL, and
    // the real tiles are appended after these. A stand-in that overran the
    // budget would push a tile that actually arrived off the frame.
    //
    // Run down BOTH paths: the ancestor walk and the descendant walk count
    // against the same budget, and a check on only one of them bounds only
    // half the cases.
    for (const bool viaAncestor : { true, false })
    {
        std::vector<TileId> wanted;
        std::vector<bool> have;
        FakeCache cache;
        for (std::uint32_t i = 0; i < 40; ++i)
        {
            // Far enough apart that no two share a parent, so each needs a
            // stand-in of its own and the cap is what bounds the result.
            const std::uint32_t x = 1000 + (i * 64);
            wanted.push_back(TileId { 13, x, 3281 });
            have.push_back(false);
            if (viaAncestor)
            {
                cache.add(12, x / 2, 1640);
            }
            else
            {
                cache.add(14, x * 2, 6562);
                cache.add(14, (x * 2) + 1, 6562);
                cache.add(14, x * 2, 6563);
                cache.add(14, (x * 2) + 1, 6563);
            }
        }

        const std::string path = viaAncestor ? " (ancestors)" : " (descendants)";
        const auto out = substituteTiles(wanted, haveOf(have), cache.predicate(), 7);
        check(out.size() <= 7, "the cap holds" + path + ", got " + std::to_string(out.size()));
        check(!out.empty(), "and it did not give up entirely" + path);

        check(substituteTiles(wanted, haveOf(have), cache.predicate(), 0).empty(),
              "a budget of zero yields nothing rather than one" + path);
    }
}

void test_the_edges_of_the_pyramid_do_not_overflow()
{
    // Against a cache that says YES TO EVERYTHING, so the guards are what stops
    // the walk rather than a lookup that happens to miss. Without that this
    // passes whether or not the guards are there: an unguarded walk above z0
    // computes z = 255 and any real cache simply says no to it.
    //
    // The invariant is not "nothing is proposed" -- at z0 the descendants are a
    // perfectly good answer, and at the deepest level the ancestors are -- but
    // that every id proposed names a level that EXISTS.
    const auto anything = [](const TileId&) { return true; };

    for (const TileId& id : substituteTiles({ TileId { 0, 0, 0 } }, haveNone, anything, 64))
    {
        check(id.z <= kMaxTileZoom,
              "nothing above z0 is proposed -- an unguarded walk up wraps to z255, got z" +
                  std::to_string(id.z));
    }

    // The descendant guard needs a cache that says no to every ANCESTOR, or the
    // walk up succeeds at the first try and the code below it never runs.
    const auto onlyImpossiblyDeep = [](const TileId& id) { return id.z > kMaxTileZoom; };
    for (const TileId& id :
         substituteTiles({ TileId { kMaxTileZoom, 5, 5 } }, haveNone, onlyImpossiblyDeep, 64))
    {
        check(id.z <= kMaxTileZoom,
              "and nothing deeper than the projection goes, got z" + std::to_string(id.z));
    }
}

void test_a_mismatched_call_is_refused_rather_than_read_off_the_end()
{
    // The length-mismatch case the vector<bool> signature had is gone by
    // construction -- a predicate cannot be the wrong length. What remains
    // refusable is a missing predicate on either side.
    FakeCache cache;
    cache.add(13, 1414, 3281);
    const std::vector<TileId> wanted { { 14, 2828, 6562 }, { 14, 2829, 6562 } };

    check(substituteTiles(wanted, nullptr, cache.predicate(), 64).empty(),
          "a missing have predicate yields nothing rather than a crash");
    check(substituteTiles(wanted, haveNone, nullptr, 64).empty(),
          "and so does a missing cache predicate");
}

void test_visible_tiles_cover_the_viewport()
{
    const Projection projection(Camera { kIrvine, 14.0, 0.0 }, 1024.0, 768.0);
    const auto tiles = projection.visibleTiles(14);

    check(!tiles.empty(), "a viewport has tiles in it");

    // The tile under the camera centre must be among them. If it is not,
    // nothing else matters -- the middle of the screen would be blank.
    const bool hasCentre =
        std::any_of(tiles.begin(), tiles.end(),
                    [](const TileId& id) { return id.x == 2828 && id.y == 6562; });
    check(hasCentre, "including the one under the camera");

    // Every corner of the viewport must be inside some returned tile, or the
    // map has a hole in it.
    const ScreenPoint corners[] = { { 1.0, 1.0 }, { 1023.0, 1.0 }, { 1023.0, 767.0 }, { 1.0, 767.0 } };
    for (const ScreenPoint& corner : corners)
    {
        const auto world = projection.worldForScreen(corner);
        const double side = std::exp2(14.0);
        const auto wantX = static_cast<std::uint32_t>(std::floor(world.x * side));
        const auto wantY = static_cast<std::uint32_t>(std::floor(world.y * side));
        const bool covered = std::any_of(tiles.begin(), tiles.end(), [&](const TileId& id) {
            return id.x == wantX && id.y == wantY;
        });
        check(covered, "the tile under screen corner (" + std::to_string(corner.x) + ", " +
                           std::to_string(corner.y) + ") is requested");
    }

    auto ordered = tiles;
    projection.sortCentreOutward(ordered);
    check(ordered.front().x == 2828 && ordered.front().y == 6562,
          "and sorted centre-outward the middle tile is requested FIRST");
    check(ordered.size() == tiles.size(), "without gaining or losing any");
}

void test_the_visible_order_does_not_move_with_the_camera()
{
    // THE reason visibleTiles() is row-major. This order reaches GpuRenderer,
    // which compares the batch list positionally to decide whether its vertex
    // buffer is still valid -- so if a camera nudge that changes nothing about
    // WHICH tiles are visible reorders them, the renderer re-uploads every
    // tile's geometry for no reason. Measured at 30 uploads over a 300-frame
    // drive that crossed ~12 tile boundaries.
    //
    // A centre-outward sort does exactly that: two tiles at nearly equal
    // distance swap the moment the camera passes the midpoint between them.
    // A z14 tile is about 2 km across at this latitude, so this crosses
    // several tile boundaries over the run rather than jiggling inside one.
    const double step = 0.005;
    std::vector<TileId> previous;
    int reorderings = 0;
    int setChanges = 0;

    for (int i = 0; i < 40; ++i)
    {
        const Coordinate here { kIrvine.latitude, kIrvine.longitude + (i * step) };
        const Projection projection(Camera { here, 14.0, 0.0 }, 660.0, 640.0);
        const auto tiles = projection.visibleTiles(14);

        if (!previous.empty())
        {
            std::vector<TileId> a = previous;
            std::vector<TileId> b = tiles;
            const auto byId = [](const TileId& l, const TileId& r) {
                return (l.z != r.z) ? (l.z < r.z)
                                    : ((l.x != r.x) ? (l.x < r.x) : (l.y < r.y));
            };
            std::sort(a.begin(), a.end(), byId);
            std::sort(b.begin(), b.end(), byId);

            if (a != b)
            {
                ++setChanges;
            }
            else if (previous != tiles)
            {
                // Same tiles, different order. This is the case that used to
                // cost a full re-upload.
                ++reorderings;
            }
        }
        previous = tiles;
    }

    check(setChanges > 0, "the camera moved far enough to change the tile set");
    check(reorderings == 0,
          "and the same set of tiles never comes back in a different order");
}

void test_the_margined_walk_agrees_with_two_separate_ones()
{
    // One traversal now produces both sets. They have to be exactly what two
    // separate calls produced, or the paint pass draws the prefetch ring (which
    // it must not) or drops tiles that are on screen (which leaves holes).
    const Camera cameras[] = {
        Camera { kIrvine, 14.0, 0.0 },
        Camera { kIrvine, 14.0, 45.0 },
        // Astride the date line, where x wraps and the ring straddles the seam.
        Camera { Coordinate { 0.0, 179.99 }, 5.0, 0.0 },
        Camera { Coordinate { 0.0, -179.99 }, 5.0, 30.0 },
        // Hard against the northern Mercator limit, where y is clamped.
        Camera { Coordinate { 85.0, 0.0 }, 4.0, 0.0 },
    };

    for (const Camera& camera : cameras)
    {
        const Projection projection(camera, 660.0, 640.0);
        const auto z = static_cast<std::uint8_t>(camera.zoom);

        const auto both = projection.visibleTilesWithMargin(z, 1);
        const auto drawnAlone = projection.visibleTiles(z, 0);
        const auto ringAlone = projection.visibleTiles(z, 1);

        check(both.drawn == drawnAlone,
              "the drawn set matches an un-margined walk at bearing " +
                  std::to_string(camera.bearing));
        check(both.withMargin == ringAlone,
              "and the margined set matches a margined walk at bearing " +
                  std::to_string(camera.bearing));

        // The ring must be a superset, or a tile would be drawn without ever
        // having been requested.
        for (const TileId& id : both.drawn)
        {
            const bool present = std::any_of(
                both.withMargin.begin(), both.withMargin.end(),
                [&](const TileId& other) { return other == id; });
            check(present, "every drawn tile is also requested");
        }
        check(both.withMargin.size() >= both.drawn.size(), "and the ring is no smaller");
    }
}

void test_a_rotated_viewport_still_covers_its_corners()
{
    // With the map turned, the axis-aligned box of two corners does not contain
    // the other two -- so a naive implementation leaves untiled triangles that
    // appear and disappear as the vehicle turns.
    const Projection projection(Camera { kIrvine, 14.0, 45.0 }, 1024.0, 768.0);
    const auto tiles = projection.visibleTiles(14);

    const ScreenPoint corners[] = { { 1.0, 1.0 }, { 1023.0, 1.0 }, { 1023.0, 767.0 }, { 1.0, 767.0 } };
    int uncovered = 0;
    for (const ScreenPoint& corner : corners)
    {
        const auto world = projection.worldForScreen(corner);
        const double side = std::exp2(14.0);
        const auto wantX = static_cast<std::uint32_t>(std::floor(world.x * side));
        const auto wantY = static_cast<std::uint32_t>(std::floor(world.y * side));
        if (std::none_of(tiles.begin(), tiles.end(),
                         [&](const TileId& id) { return id.x == wantX && id.y == wantY; }))
        {
            ++uncovered;
        }
    }
    check(uncovered == 0, "every corner of a rotated viewport is covered");
}

void test_rotation_turns_the_map_the_right_way()
{
    // Bearing 90 means "east is up". A point to the east must therefore appear
    // ABOVE the centre, not to the right of it.
    const Projection projection(Camera { kIrvine, 12.0, 90.0 }, 800.0, 600.0);

    const ScreenPoint east =
        projection.screenFor(Coordinate { kIrvine.latitude, kIrvine.longitude + 0.05 });
    check(east.y < 300.0, "with bearing 90, east is up");
    check(near(east.x, 400.0, 1.0), "and no longer to the right");

    // Round-tripping through the inverse must still work with rotation on --
    // the inverse rotation is the transpose, and getting the sign wrong there
    // makes clicks land somewhere other than where they were made.
    const ScreenPoint point { 200.0, 150.0 };
    const ScreenPoint back = projection.screenFor(projection.coordinateForScreen(point));
    check(near(back.x, point.x, 1e-6) && near(back.y, point.y, 1e-6),
          "and the inverse still round-trips");
}

void test_a_camera_at_the_date_line_takes_the_short_way_round()
{
    // A camera at 179E looking at a point at 179W: they are two degrees apart,
    // not 358. Without the wrap the point lands most of a world off screen.
    const Projection projection(Camera { Coordinate { 0.0, 179.0 }, 8.0, 0.0 }, 800.0, 600.0);

    const ScreenPoint across = projection.screenFor(Coordinate { 0.0, -179.0 });
    check(std::abs(across.x - 400.0) < projection.worldPixels() / 4.0,
          "a point across the date line is near the camera, not a world away");
    check(across.x > 400.0, "and to the east of it");
}

void test_tile_placement_matches_the_projection()
{
    const Projection projection(Camera { kIrvine, 14.0, 0.0 }, 800.0, 600.0);

    const TileId centre { 14, 2828, 6562 };
    const ScreenPoint origin = projection.tileOrigin(centre);
    const double size = projection.tileScreenSize(14);

    check(size > 0.0, "a tile has a positive on-screen size");

    // The camera centre is inside its own tile, and the tile's box must
    // contain it. An off-by-one in tileOrigin puts every tile one place over,
    // which draws a complete, coherent, shifted map.
    const ScreenPoint cameraOnScreen = projection.screenFor(kIrvine);
    check(cameraOnScreen.x >= origin.x && cameraOnScreen.x <= origin.x + size,
          "the camera falls inside its own tile horizontally");
    check(cameraOnScreen.y >= origin.y && cameraOnScreen.y <= origin.y + size,
          "and vertically");

    // The next tile east begins exactly where this one ends.
    const ScreenPoint nextEast = projection.tileOrigin(TileId { 14, 2829, 6562 });
    check(near(nextEast.x, origin.x + size, 1e-6), "the tile to the east abuts it exactly");

    const ScreenPoint nextSouth = projection.tileOrigin(TileId { 14, 2828, 6563 });
    check(near(nextSouth.y, origin.y + size, 1e-6), "and the one to the south");
}

void test_a_tile_is_drawn_at_its_native_size_when_zoom_matches()
{
    // The definition of the tile size: at camera zoom == tile zoom, one tile is
    // exactly tileSize pixels. Anything else and the whole map is scaled.
    const Projection projection(Camera { kIrvine, 14.0, 0.0 }, 800.0, 600.0, 512);
    check(near(projection.tileScreenSize(14), 512.0, 1e-9),
          "at zoom 14 a z14 tile is 512 pixels");
    check(near(projection.tileScreenSize(13), 1024.0, 1e-9),
          "and a z13 tile covers twice as much, so it is drawn twice as large");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_the_anchors_of_the_projection();
    test_world_coordinates_round_trip();
    test_latitude_is_clamped_and_longitude_wraps();

    test_irvine_lands_in_the_expected_tile();
    test_the_camera_centre_lands_in_the_middle_of_the_widget();
    test_screen_and_world_round_trip();
    test_north_is_up_and_east_is_right();
    test_zoom_scales_by_powers_of_two();

    test_tile_zoom_is_rounded_and_clamped();
    test_the_device_pixel_ratio_is_carried_and_not_applied();

    test_nothing_stands_in_for_a_tile_that_arrived();
    test_a_four_level_overview_stands_in_for_ground_never_visited();
    test_the_substitute_walk_reaches_five_levels_and_no_further();
    test_one_ancestor_covers_a_tile_and_its_siblings();
    test_the_nearest_ancestor_wins();
    test_a_deeper_cache_stands_in_when_there_is_no_ancestor();
    test_a_partial_deeper_cache_still_helps();
    test_an_ancestor_beats_descendants();
    test_the_budget_is_not_exceeded();
    test_the_edges_of_the_pyramid_do_not_overflow();
    test_a_mismatched_call_is_refused_rather_than_read_off_the_end();
    test_visible_tiles_cover_the_viewport();
    test_the_visible_order_does_not_move_with_the_camera();
    test_the_margined_walk_agrees_with_two_separate_ones();
    test_a_rotated_viewport_still_covers_its_corners();
    test_rotation_turns_the_map_the_right_way();
    test_a_camera_at_the_date_line_takes_the_short_way_round();

    test_tile_placement_matches_the_projection();
    test_a_tile_is_drawn_at_its_native_size_when_zoom_matches();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all projection checks passed");
    return 0;
}
