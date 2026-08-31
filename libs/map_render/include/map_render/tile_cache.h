// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoded-tile cache, and the policy that decides what leaves it.
//
// Split out of TileSource because that class cannot be built without a zenoh
// session, and the eviction policy is exactly the part worth testing: the
// difference between dropping the oldest tile and dropping the one the driver
// is looking at is invisible in a screenshot and obvious in a unit test.
//
// GUI thread only. TileSource decodes on zenoh threads and hands the result
// across through its mailbox; nothing here takes a lock.
#ifndef MAP_TILE_CACHE_H
#define MAP_TILE_CACHE_H

#include <cstdint>
#include <list>
#include <memory>
#include <unordered_map>

#include "map_render/label_candidates.h"
#include "map_render/projection.h"
#include "map_render/tessellator.h"

namespace map_render
{

// One tile, in both the forms the widget draws from: the triangles for the
// GPU and the extracted label candidates for the label pass.
//
// The decoded mvt::Tile itself is NOT here, deliberately. The label pass was
// its only consumer after tessellation, and everything camera-free about a
// label is extracted once at decode time (see map/labels.h) -- so the tile
// dies on the worker that decoded it, and a dense downtown entry weighs
// vertices plus a few kilobytes of names instead of vertices plus hundreds of
// kilobytes of features it would never read again.
struct CachedTile
{
    std::shared_ptr<const LabelSet> labels;
    std::shared_ptr<const TileGeometry> geometry;
    // Roughly what this tile occupies, measured once when it is inserted rather
    // than walked again on every eviction check. Approximate on purpose -- this
    // is a budget, not an allocator.
    std::size_t bytes { 0 };

    explicit operator bool() const { return geometry != nullptr; }
};

// Roughly what a decoded tile occupies, vertices and features together.
std::size_t approximateBytes(const CachedTile& tile);

class TileCache
{
  public:
    // How many decoded tiles to keep, and what they may weigh.
    //
    // Both bounds, because neither works alone. The count alone is what this
    // had, and it is the wrong bound: an empty ocean tile is a few dozen bytes
    // and a downtown z14 tile is a few hundred kilobytes, so 256 of them is
    // anywhere between nothing and eighty megabytes depending entirely on where
    // the drive went. The byte bound alone would let a city of tiny tiles grow
    // the map itself without limit.
    // The byte figure is set against what a dense tile actually weighs.
    // docs/map.md measures four z14 tiles at 96 822 vertices, so ~24k each,
    // which is ~865 kB of MapVertex before the decoded features are counted --
    // call it 1.5 MB for downtown. 256 of those is nearly 400 MB, which is what
    // the count bound alone was quietly permitting.
    static constexpr std::size_t kMaxTiles = 256;
    static constexpr std::size_t kMaxBytes = 128u * 1024u * 1024u;

    // ...but the byte bound may not evict below this many tiles.
    //
    // Without a floor, a viewport bigger than the budget evicts tiles it is
    // still drawing, which re-requests them on the very next paint -- a refetch
    // loop that reads as a slow server rather than as a small cache.
    //
    // 80 because offscreen_renderer.h puts a 3840x2160 viewport at 40 tiles, and 70
    // with the prefetch ring. The set the paint pass just asked for is
    // therefore always safe, and safe is the point: an evicted visible tile
    // costs a round trip AND a re-tessellation on every frame, forever.
    //
    // THE FLOOR WINS OVER THE BYTE BUDGET, and with worst-case tiles it can
    // exceed it -- 80 downtown tiles is ~120 MB against a 128 MB budget, and
    // pathological ones would go past. That is deliberate. Memory spent holding
    // the screen is worth more than memory saved thrashing it.
    static constexpr std::size_t kMinTiles = 80;
    static_assert(kMinTiles < kMaxTiles);

    // Add or replace. Weighs the tile, then evicts until both bounds hold.
    void insert(const TileId& id, CachedTile tile);

    // The tile, or null. ASKING COUNTS AS USE: this is what keeps the ground
    // under the vehicle out of the eviction queue, and it is why there is no
    // const overload.
    const CachedTile* find(const TileId& id);

    // Cached AND carrying geometry worth drawing.
    //
    // Not the same question as "is it cached": an ABSENT tile is cached too,
    // with nothing in it, so that it is not asked for again. As a stand-in for
    // some other tile it would occupy a draw slot and paint nothing.
    //
    // Counts as use, like find() -- a tile serving as a stand-in is on screen.
    bool drawable(const TileId& id);

    // Whether the tile is held at all, WITHOUT counting as use.
    //
    // The distinction matters: request() asks this of the whole viewport plus
    // its prefetch ring on every paint, to decide what still needs fetching.
    // Treating that as use would promote ring tiles that are requested and
    // never drawn, and they are precisely the speculative ones that should go
    // first when the cache is full.
    bool contains(const TileId& id) const { return mEntries.contains(id); }

    void clear();

    std::size_t size() const { return mEntries.size(); }
    std::size_t bytes() const { return mBytes; }

    // The least recently used id, i.e. whatever eviction takes next. For tests
    // and for diagnosing a cache that is thrashing.
    const TileId* nextEviction() const;

  private:
    void touch(const TileId& id);
    void evictIfNeeded();

    std::unordered_map<TileId, CachedTile, TileIdHash> mEntries;
    // LEAST recently used first.
    //
    // Least-recently-used rather than insertion order, because insertion order
    // evicts the tile under the vehicle in favour of one the drive left behind
    // an hour ago -- and an evicted tile that is still on screen is re-fetched
    // immediately, so the cost is a round trip and a re-tessellation every
    // frame rather than once.
    //
    // A list plus an index into it, so promoting a tile is a pointer splice.
    // The paint pass asks about every visible tile, which at a 4K viewport with
    // the prefetch ring is seventy questions a frame.
    std::list<TileId> mOrder;
    std::unordered_map<TileId, std::list<TileId>::iterator, TileIdHash> mAt;
    std::size_t mBytes { 0 };
};

} // namespace map_render

#endif // MAP_TILE_CACHE_H
