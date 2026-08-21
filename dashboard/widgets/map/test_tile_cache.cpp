// SPDX-License-Identifier: GPL-3.0-or-later
//
// The eviction policy, which is invisible in a screenshot.
//
// A cache that drops the wrong tile does not draw anything wrong -- it
// re-fetches, and the map looks like a slow server. These are the checks that
// tell the two apart.

#include "map/tile_cache.h"

#include <spdlog/spdlog.h>

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

using map_widget::CachedTile;
using map_widget::TileCache;
using map_widget::TileId;

// A tile carrying `vertexCount` vertices, so its weight is predictable, and the
// indices that make it drawable -- the geometry is indexed, so a tile with
// vertices and no indices draws nothing.
CachedTile tileOf(std::size_t vertexCount)
{
    auto geometry = std::make_shared<map_widget::TileGeometry>();
    geometry->vertices.resize(vertexCount);
    geometry->indices.resize(vertexCount);
    geometry->layerStart.fill(static_cast<std::uint32_t>(vertexCount));
    geometry->layerStart[0] = 0;
    geometry->layerIndexStart.fill(static_cast<std::uint32_t>(vertexCount));
    geometry->layerIndexStart[0] = 0;

    CachedTile tile;
    tile.labels = std::make_shared<map_widget::LabelSet>();
    tile.geometry = std::move(geometry);
    return tile;
}

// An ABSENT tile: cached so it is never asked for again, but with no geometry
// at all -- no vertices and, more to the point, no indices.
CachedTile absentTile()
{
    CachedTile tile;
    tile.labels = std::make_shared<map_widget::LabelSet>();
    tile.geometry = std::make_shared<map_widget::TileGeometry>();
    return tile;
}

TileId at(std::uint32_t x)
{
    return TileId { 14, x, 6562 };
}

// ============================================================================

void test_a_tile_that_is_still_being_drawn_is_not_the_next_evicted()
{
    TileCache cache;

    const TileId underTheVehicle = at(0);
    cache.insert(underTheVehicle, tileOf(64));
    for (std::uint32_t i = 1; i < 40; ++i)
    {
        cache.insert(at(i), tileOf(64));
    }

    // Insertion order would make the first tile the next to go. It is the one
    // the driver is looking at.
    const TileId* before = cache.nextEviction();
    check(before != nullptr && *before == underTheVehicle,
          "before it is drawn, the oldest tile is indeed the next to go");

    // The paint pass asks for it, which is what "in use" means here.
    check(cache.find(underTheVehicle) != nullptr, "and it is still cached");

    const TileId* after = cache.nextEviction();
    check(after != nullptr && !(*after == underTheVehicle),
          "once drawn it is no longer the next evicted -- an insertion-ordered "
          "cache would drop the ground under the vehicle");
}

// A stand-in is on screen. drawable() is how the paint pass finds one, so it
// has to count as use too.
void test_serving_as_a_stand_in_counts_as_use()
{
    TileCache cache;
    const TileId ancestor = at(0);
    cache.insert(ancestor, tileOf(64));
    cache.insert(at(1), tileOf(64));
    cache.insert(at(2), tileOf(64));

    check(cache.drawable(ancestor), "the ancestor has geometry worth drawing");
    const TileId* next = cache.nextEviction();
    check(next != nullptr && !(*next == ancestor),
          "and asking that question promoted it");
}

// An absent tile is cached on purpose -- most of the pyramid is empty and
// without caching the miss it is re-requested every frame forever -- but it
// must never be handed out as a stand-in, because it would take a draw slot
// and paint nothing.
void test_an_absent_tile_is_cached_but_not_drawable()
{
    TileCache cache;
    const TileId empty = at(7);
    cache.insert(empty, absentTile());

    check(cache.contains(empty), "the miss is cached, so it is not asked for again");
    check(!cache.drawable(empty), "but it is not offered as something to draw");
}

// contains() is the "do I still need to fetch this?" question, asked of the
// whole viewport AND its prefetch ring on every paint. Treating it as use
// would promote ring tiles that are never drawn -- precisely the speculative
// ones that should go first.
void test_asking_whether_a_tile_needs_fetching_is_not_use()
{
    TileCache cache;
    const TileId ringTile = at(0);
    cache.insert(ringTile, tileOf(64));
    cache.insert(at(1), tileOf(64));

    check(cache.contains(ringTile), "it is cached");
    const TileId* next = cache.nextEviction();
    check(next != nullptr && *next == ringTile,
          "and asking did not promote it above the tile that was drawn");
}

void test_the_count_bound_holds()
{
    TileCache cache;
    for (std::uint32_t i = 0; i < TileCache::kMaxTiles + 50; ++i)
    {
        cache.insert(at(i), tileOf(16));
    }
    check(cache.size() <= TileCache::kMaxTiles,
          "the cache stops at its ceiling, got " + std::to_string(cache.size()));
    check(cache.size() >= TileCache::kMinTiles, "and does not empty itself getting there");
}

// The bound the count alone could not express: an ocean tile and a downtown
// tile differ by three orders of magnitude, so 256 of them is anywhere between
// nothing and eighty megabytes.
void test_heavy_tiles_are_bounded_by_bytes_before_they_hit_the_count()
{
    TileCache cache;

    // A megabyte of vertices each. The budget holds ~128 of these, which is
    // above the floor -- so here the BYTE bound is the one doing the work, and
    // the cache must settle well short of its 256-tile ceiling.
    constexpr std::size_t kBytesPerTile = 1024u * 1024u;
    constexpr std::size_t kVertices = kBytesPerTile / sizeof(map_widget::MapVertex);
    const std::size_t expected = TileCache::kMaxBytes / kBytesPerTile;
    static_assert(TileCache::kMaxBytes / kBytesPerTile > TileCache::kMinTiles,
                  "this test only exercises the byte bound if it bites above the floor");

    for (std::uint32_t i = 0; i < TileCache::kMaxTiles; ++i)
    {
        cache.insert(at(i), tileOf(kVertices));
    }

    check(cache.size() < TileCache::kMaxTiles,
          "heavy tiles are evicted before the count bound is reached, got " +
              std::to_string(cache.size()));
    check(cache.size() > TileCache::kMinTiles,
          "and the byte bound, not the floor, is what stopped it");
    // Within one tile of the budget: eviction stops as soon as it is under.
    check(cache.size() <= expected + 1,
          "the cache settles at its byte budget, got " + std::to_string(cache.size()) +
              " tiles against " + std::to_string(expected));
}

// The floor. Without it a viewport bigger than the byte budget evicts tiles it
// is still drawing, and re-requests them on the very next paint -- a refetch
// loop that reads as a slow server rather than as a small cache.
void test_the_byte_bound_never_evicts_below_the_floor()
{
    TileCache cache;

    // Absurdly heavy: two of these alone blow the whole budget, so a byte
    // bound with no floor would evict down to almost nothing -- including
    // tiles the paint pass is mid-frame on.
    constexpr std::size_t kVertices = (80u * 1024u * 1024u) / sizeof(map_widget::MapVertex);
    for (std::uint32_t i = 0; i < TileCache::kMinTiles + 20; ++i)
    {
        cache.insert(at(i), tileOf(kVertices));
    }

    check(cache.size() == TileCache::kMinTiles,
          "the byte bound yields to the floor rather than emptying the viewport, got " +
              std::to_string(cache.size()));
    check(cache.bytes() > TileCache::kMaxBytes,
          "which does mean the budget is knowingly exceeded in this case");
}

// Re-inserting must not leak weight. Without reading the outgoing entry's
// bytes first, the running total only ever grows and the byte bound eventually
// evicts down to the floor on every single insert.
void test_replacing_a_tile_does_not_leak_its_weight()
{
    TileCache cache;
    const TileId id = at(3);

    cache.insert(id, tileOf(4096));
    const std::size_t once = cache.bytes();

    for (int i = 0; i < 50; ++i)
    {
        cache.insert(id, tileOf(4096));
    }

    check(cache.size() == 1, "re-inserting the same id keeps one entry");
    check(cache.bytes() == once,
          "and one entry's weight, got " + std::to_string(cache.bytes()) + " against " +
              std::to_string(once));
}

void test_clear_forgets_everything_including_the_weight()
{
    TileCache cache;
    cache.insert(at(1), tileOf(4096));
    cache.insert(at(2), tileOf(4096));
    cache.clear();

    check(cache.size() == 0, "cleared");
    check(cache.bytes() == 0, "and the running weight went with it");
    check(cache.nextEviction() == nullptr, "with nothing queued to evict");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_tile_that_is_still_being_drawn_is_not_the_next_evicted();
    test_serving_as_a_stand_in_counts_as_use();
    test_an_absent_tile_is_cached_but_not_drawable();
    test_asking_whether_a_tile_needs_fetching_is_not_use();

    test_the_count_bound_holds();
    test_heavy_tiles_are_bounded_by_bytes_before_they_hit_the_count();
    test_the_byte_bound_never_evicts_below_the_floor();

    test_replacing_a_tile_does_not_leak_its_weight();
    test_clear_forgets_everything_including_the_weight();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all tile cache checks passed");
    return 0;
}
