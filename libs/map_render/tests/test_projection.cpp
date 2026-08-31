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
#include <numbers>
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
    // THE reason visibleTiles() is row-major. This order reaches OffscreenRenderer,
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

// ------------------------------------------------------------------- pitch

void test_an_unpitched_camera_takes_the_flat_path()
{
    // pitch 0 must be indistinguishable from the projection as it was before
    // pitch existed -- the widget runs at 0 except in perspective mode, and
    // Stage-gating relies on these frames not changing by a bit.
    const Projection projection(Camera { kIrvine, 14.0, 30.0 }, 800.0, 600.0);
    check(!projection.pitched(), "pitch 0 reports unpitched");

    const auto transform = projection.tileTransform(TileId { 14, 2828, 6562 });
    check(!transform.pitched, "and hands out an unpitched tile transform");
    check(near(transform.scaleAt(0.3, 0.7), 1.0, 1e-12), "whose scale is exactly 1");
}

void test_the_centre_is_invariant_under_pitch()
{
    // The tilt pivots about the viewport centre, so the camera's own position
    // must not move when the view tilts -- that is what makes toggling the
    // perspective read as the SAME place leaning back.
    const Projection projection(Camera { kIrvine, 14.0, 0.0, 45.0 }, 800.0, 600.0);

    const ScreenPoint centre = projection.screenFor(kIrvine);
    check(near(centre.x, 400.0, 1e-6), "the camera centre stays at half the width under pitch");
    check(near(centre.y, 300.0, 1e-6), "and half the height");
}

void test_a_pitched_point_lands_where_the_formula_says()
{
    // Worked by hand from the tilt: a point d pixels up-screen on the flat map
    // lands at cy - d*cos(p)*f/(f + d*sin(p)), with f = 1.5 * height. Computed
    // here from first principles, not by calling the code under test.
    const double height = 600.0;
    const double focal = 1.5 * height;
    const double pitch = 45.0 * std::numbers::pi / 180.0;
    const double d = 200.0;

    // A coordinate that the FLAT camera puts exactly d pixels above centre.
    const Projection flat(Camera { kIrvine, 14.0, 0.0 }, 800.0, height);
    const Coordinate up = flat.coordinateForScreen(ScreenPoint { 400.0, 300.0 - d });

    const Projection pitched(Camera { kIrvine, 14.0, 0.0, 45.0 }, 800.0, height);
    const ScreenPoint seen = pitched.screenFor(up);

    const double expectedY = 300.0 - (d * std::cos(pitch) * focal / (focal + (d * std::sin(pitch))));
    check(near(seen.x, 400.0, 1e-9), "a point straight up-screen stays on the centre line");
    check(near(seen.y, expectedY, 1e-6), "and recedes by exactly the worked amount");
    check(seen.y > 300.0 - d, "which is less far up than on the flat map");

    // The mirror point below the centre comes CLOSER instead.
    const Coordinate down = flat.coordinateForScreen(ScreenPoint { 400.0, 300.0 + d });
    const ScreenPoint nearer = pitched.screenFor(down);
    const double expectedDown = 300.0 + (d * std::cos(pitch) * focal / (focal - (d * std::sin(pitch))));
    check(near(nearer.y, expectedDown, 1e-6), "a point below the centre is magnified toward the eye");
    check(nearer.y > 300.0 + d * std::cos(pitch), "and lands lower than pure foreshortening alone");
}

void test_pitch_round_trips_everywhere_it_is_defined()
{
    // The inverse was derived by algebra; only a round-trip over the whole
    // screen at several pitches and bearings says the algebra was right.
    const double pitches[] = { 15.0, 30.0, 45.0, 60.0 };
    const double bearings[] = { 0.0, 45.0, 210.0 };
    const ScreenPoint points[] = { { 0.0, 0.0 },     { 800.0, 0.0 }, { 0.0, 600.0 },
                                   { 800.0, 600.0 }, { 400.0, 300.0 }, { 123.0, 45.0 },
                                   { 700.0, 580.0 } };

    for (const double pitch : pitches)
    {
        for (const double bearing : bearings)
        {
            const Projection projection(Camera { kIrvine, 14.0, bearing, pitch }, 800.0, 600.0);
            for (const ScreenPoint& point : points)
            {
                const ScreenPoint back =
                    projection.screenFor(projection.worldForScreen(point));
                check(near(back.x, point.x, 1e-6) && near(back.y, point.y, 1e-6),
                      "screen -> world -> screen round-trips at pitch " + std::to_string(pitch) +
                          " bearing " + std::to_string(bearing) + " (" + std::to_string(point.x) +
                          ", " + std::to_string(point.y) + ")");
            }
        }
    }
}

void test_every_screen_pixel_meets_the_ground_at_the_pitch_cap()
{
    // The reason kMaxPitch exists: at 60 degrees the horizon is still above
    // the top edge, so all four corners -- the extreme rays -- must invert to
    // finite ground and project straight back.
    const Projection projection(Camera { kIrvine, 14.0, 30.0, 60.0 }, 800.0, 600.0);

    const ScreenPoint corners[] = { { 0.0, 0.0 }, { 800.0, 0.0 }, { 0.0, 600.0 },
                                    { 800.0, 600.0 } };
    for (const ScreenPoint& corner : corners)
    {
        const auto world = projection.worldForScreen(corner);
        check(std::isfinite(world.x) && std::isfinite(world.y),
              "a corner inverts to finite ground at the pitch cap");
        const ScreenPoint back = projection.screenFor(world);
        check(near(back.x, corner.x, 1e-6) && near(back.y, corner.y, 1e-6),
              "and projects straight back to the corner");
    }

    // A point far BEHIND the eye still projects to something finite (the w
    // clamp), and lands well below the bottom edge where bounds tests reject
    // it -- total, and harmless.
    const Coordinate farBehind { kIrvine.latitude - 2.0, kIrvine.longitude };
    const ScreenPoint behind = projection.screenFor(farBehind);
    check(std::isfinite(behind.x) && std::isfinite(behind.y),
          "a point behind the camera still projects finitely");
    check(behind.y > 600.0, "and lands below the bottom edge, never inside the frame");
}

void test_pitch_is_clamped_to_the_cap()
{
    // A camera asking for 80 degrees gets 60: past the cap the top of the
    // screen leaves the ground plane and the inverse stops existing.
    const Projection wild(Camera { kIrvine, 14.0, 0.0, 80.0 }, 800.0, 600.0);
    const Projection capped(Camera { kIrvine, 14.0, 0.0, map_render::kMaxPitch }, 800.0, 600.0);

    const ScreenPoint point { 250.0, 80.0 };
    const auto a = wild.screenFor(wild.worldForScreen(point));
    const auto sameGround = wild.screenFor(capped.worldForScreen(point));
    check(near(a.x, point.x, 1e-6) && near(a.y, point.y, 1e-6), "an over-cap pitch still inverts");
    check(near(sameGround.x, point.x, 1e-6) && near(sameGround.y, point.y, 1e-6),
          "and behaves exactly as the cap");
}

void test_the_tile_transform_agrees_with_the_projection_under_pitch()
{
    // The label pass places every glyph through TileTransform::map, and the
    // marker goes through screenFor. If the two disagree, labels float off
    // their roads -- only under pitch, only away from the centre.
    const Projection projection(Camera { kIrvine, 14.0, 25.0, 50.0 }, 800.0, 600.0);

    const TileId tile { 14, 2828, 6562 };
    const auto transform = projection.tileTransform(tile);
    check(transform.pitched, "the tile transform carries the tilt");

    const double side = std::exp2(14.0);
    const double locals[][2] = { { 0.0, 0.0 }, { 1.0, 1.0 }, { 0.3, 0.7 }, { 0.9, 0.1 } };
    for (const auto& local : locals)
    {
        const ScreenPoint viaTransform = transform.map(local[0], local[1]);
        const ScreenPoint viaProjection = projection.screenFor(map_render::WorldPoint {
            (double(tile.x) + local[0]) / side, (double(tile.y) + local[1]) / side });
        check(near(viaTransform.x, viaProjection.x, 1e-6) &&
                  near(viaTransform.y, viaProjection.y, 1e-6),
              "TileTransform::map matches screenFor at local (" + std::to_string(local[0]) + ", " +
                  std::to_string(local[1]) + ")");
    }
}

void test_scale_shrinks_up_screen_and_grows_down_screen()
{
    const Projection projection(Camera { kIrvine, 14.0, 0.0, 45.0 }, 800.0, 600.0);
    const auto transform = projection.tileTransform(TileId { 14, 2828, 6562 });

    // The camera's own tile: find the local y of the viewport centre, then
    // sample above and below it.
    const ScreenPoint origin = projection.tileOrigin(TileId { 14, 2828, 6562 });
    const double centreLocalY = (300.0 - origin.y) / projection.tileScreenSize(14);

    const double atCentre = transform.scaleAt(0.5, centreLocalY);
    const double above = transform.scaleAt(0.5, centreLocalY - 0.3);
    const double below = transform.scaleAt(0.5, centreLocalY + 0.3);
    check(near(atCentre, 1.0, 1e-9), "scale is exactly 1 at the viewport centre");
    check(above < 1.0, "smaller up-screen, where the ground recedes");
    check(below > 1.0, "larger down-screen, where it approaches the eye");
}

// ------------------------------------------------------- pitched tile walk

void test_a_nearly_flat_descent_agrees_with_the_grid_walk()
{
    // The descent is a different algorithm; at (almost) no pitch it must reach
    // the same answer as the grid walk or the two modes would draw different
    // maps for the same view.
    const Projection flat(Camera { kIrvine, 14.0, 0.0 }, 800.0, 600.0);
    const Projection tipped(Camera { kIrvine, 14.0, 0.0, 0.05 }, 800.0, 600.0);

    const auto grid = flat.visibleTilesWithMargin(14, 0);
    const auto descent = tipped.visibleTilesWithMargin(14, 0);

    const auto asSet = [](const std::vector<TileId>& tiles) {
        std::unordered_set<TileId, TileIdHash> set(tiles.begin(), tiles.end());
        return set;
    };
    check(asSet(grid.drawn) == asSet(descent.drawn),
          "a hair of pitch draws the same tile set as the flat grid walk");
}

void test_the_pitched_leaves_partition_the_viewport()
{
    // Every screen pixel must be covered by EXACTLY one drawn leaf: a missed
    // pixel is a hole in the map, a doubled one is the same ground drawn
    // twice, fighting itself at the seam.
    const double pitches[] = { 30.0, 45.0, 60.0 };
    for (const double pitch : pitches)
    {
        const Projection projection(Camera { kIrvine, 14.0, 35.0, pitch }, 800.0, 600.0);
        const auto tiles = projection.visibleTilesWithMargin(14, 0);
        check(!tiles.truncated, "the walk is not truncated");

        for (double sy = 3.7; sy < 600.0; sy += 43.1)
        {
            for (double sx = 2.3; sx < 800.0; sx += 41.7)
            {
                const auto world = projection.worldForScreen(ScreenPoint { sx, sy });
                int owners = 0;
                for (const TileId& id : tiles.drawn)
                {
                    const double side = std::exp2(static_cast<double>(id.z));
                    const double tx = world.x * side;
                    const double ty = world.y * side;
                    if (tx >= id.x && tx < id.x + 1 && ty >= id.y && ty < id.y + 1)
                    {
                        ++owners;
                    }
                }
                check(owners == 1, "screen point (" + std::to_string(sx) + ", " +
                                       std::to_string(sy) + ") at pitch " + std::to_string(pitch) +
                                       " is owned by exactly one leaf, not " +
                                       std::to_string(owners));
            }
        }
    }
}

void test_the_pitched_walk_is_deterministic()
{
    const Camera camera { kIrvine, 14.0, 35.0, 55.0 };
    const Projection a(camera, 800.0, 600.0);
    const Projection b(camera, 800.0, 600.0);

    const auto first = a.visibleTilesWithMargin(14, 2);
    const auto second = b.visibleTilesWithMargin(14, 2);
    check(first.drawn == second.drawn && first.withMargin == second.withMargin,
          "the same camera walks to the identical ordered lists");
}

void test_the_far_field_is_coarser_than_the_near_field()
{
    // The point of the descent. At a steep pitch the top of the screen is far
    // away and must come back at a shallower zoom than the bottom.
    const Projection projection(Camera { kIrvine, 14.0, 0.0, 60.0 }, 800.0, 600.0);
    const auto tiles = projection.visibleTilesWithMargin(14, 0);

    int nearZ = 0;
    int farZ = 99;
    for (const TileId& id : tiles.drawn)
    {
        nearZ = std::max(nearZ, int(id.z));
        farZ = std::min(farZ, int(id.z));
    }
    check(nearZ == 14, "the near field is at the target zoom");
    check(farZ < 14, "and the far field is shallower");

    // And it is the far field: every shallow leaf must sit above (up-screen
    // of) the deepest ones. Compare centres through the projection.
    double lowestShallow = 1e300;
    double highestDeep = -1e300;
    for (const TileId& id : tiles.drawn)
    {
        const double side = std::exp2(static_cast<double>(id.z));
        const auto centre = projection.screenFor(
            map_render::WorldPoint { (id.x + 0.5) / side, (id.y + 0.5) / side });
        if (int(id.z) == nearZ)
        {
            highestDeep = std::max(highestDeep, centre.y);
        }
        if (int(id.z) == farZ)
        {
            lowestShallow = std::min(lowestShallow, centre.y);
        }
    }
    check(lowestShallow < highestDeep, "the shallow leaves sit up-screen of the deep ones");
}

void test_the_pitched_walk_stays_within_the_frame_budget()
{
    // The renderer preallocates uniforms for 192 tiles a frame. The descent
    // exists to keep a pitched view inside that; a count near it would mean
    // the leaf test is wrong.
    const double zooms[] = { 10.0, 14.0, 16.0 };
    const struct { double w, h; } sizes[] = { { 660.0, 640.0 }, { 2560.0, 1440.0 } };
    for (const double zoom : zooms)
    {
        for (const auto& size : sizes)
        {
            const Projection projection(Camera { kIrvine, zoom, 30.0, 60.0 }, size.w, size.h);
            const auto tiles = projection.visibleTilesWithMargin(
                static_cast<std::uint8_t>(zoom), 0);
            check(tiles.drawn.size() < 100,
                  "at zoom " + std::to_string(zoom) + " in " + std::to_string(int(size.w)) + "x" +
                      std::to_string(int(size.h)) + " the pitched view draws " +
                      std::to_string(tiles.drawn.size()) + " tiles, well inside the budget");
        }
    }
}

void test_the_leaf_floor_is_respected()
{
    // An archive that starts at z12 cannot serve the z9 leaves a steep pitch
    // would like; the floor forces those regions to subdivide down to
    // something that exists.
    const Projection projection(Camera { kIrvine, 14.0, 0.0, 60.0 }, 800.0, 600.0);
    const auto tiles = projection.visibleTilesWithMargin(14, 0, 12);
    for (const TileId& id : tiles.drawn)
    {
        check(id.z >= 12, "no leaf is shallower than the archive floor");
    }

    // And the floor changes nothing about coverage: same partition property.
    const auto unfloored = projection.visibleTilesWithMargin(14, 0);
    check(tiles.drawn.size() >= unfloored.drawn.size(),
          "the floor can only add tiles, never remove coverage");
}

void test_the_margin_ring_is_a_superset_under_pitch()
{
    const Projection projection(Camera { kIrvine, 14.0, 20.0, 45.0 }, 800.0, 600.0);
    const auto tiles = projection.visibleTilesWithMargin(14, 2);

    std::unordered_set<TileId, TileIdHash> ring(tiles.withMargin.begin(),
                                                tiles.withMargin.end());
    for (const TileId& id : tiles.drawn)
    {
        check(ring.count(id) == 1, "every drawn leaf is also in the margined list");
    }
    check(tiles.withMargin.size() > tiles.drawn.size(),
          "and the margin actually adds prefetch tiles");
}

void test_centre_outward_sorting_handles_mixed_zoom_lists()
{
    // The pitched request list mixes zoom levels; the sort must compare
    // world-space distances or a z9 tile at the horizon sorts as if it were a
    // z14 neighbour.
    const Projection projection(Camera { kIrvine, 14.0, 0.0, 60.0 }, 800.0, 600.0);
    auto tiles = projection.visibleTilesWithMargin(14, 1).withMargin;
    projection.sortCentreOutward(tiles);

    // The first tile after sorting must contain (or nearly contain) the
    // camera; a unit mix-up puts a far coarse tile first.
    const auto world = map_render::worldFor(kIrvine);
    const TileId& first = tiles.front();
    const double side = std::exp2(static_cast<double>(first.z));
    const double dx = ((first.x + 0.5) / side) - world.x;
    const double dy = ((first.y + 0.5) / side) - world.y;
    const double tileSpan = 1.0 / side;
    check(std::abs(dx) <= tileSpan && std::abs(dy) <= tileSpan,
          "the nearest-sorted request list starts at the camera");
}

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

    test_an_unpitched_camera_takes_the_flat_path();
    test_the_centre_is_invariant_under_pitch();
    test_a_pitched_point_lands_where_the_formula_says();
    test_pitch_round_trips_everywhere_it_is_defined();
    test_every_screen_pixel_meets_the_ground_at_the_pitch_cap();
    test_pitch_is_clamped_to_the_cap();
    test_the_tile_transform_agrees_with_the_projection_under_pitch();
    test_scale_shrinks_up_screen_and_grows_down_screen();

    test_a_nearly_flat_descent_agrees_with_the_grid_walk();
    test_the_pitched_leaves_partition_the_viewport();
    test_the_pitched_walk_is_deterministic();
    test_the_far_field_is_coarser_than_the_near_field();
    test_the_pitched_walk_stays_within_the_frame_budget();
    test_the_leaf_floor_is_respected();
    test_the_margin_ring_is_a_superset_under_pitch();
    test_centre_outward_sorting_handles_mixed_zoom_lists();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all projection checks passed");
    return 0;
}
