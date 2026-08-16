// SPDX-License-Identifier: GPL-3.0-or-later
//
// Tiles, from nodes/map_server, decoded and kept.
//
// The seam between the bus and the paint pass. It owns the in-flight requests,
// the decoded-tile cache, and the one piece of threading in this widget:
// replies arrive on zenoh threads and the map is painted on the GUI thread.
//
// The hand-off follows dashboard/expression_subscription.h -- a mutex held only
// long enough to move a decoded tile into a mailbox, and a GUI-thread timer
// that drains it. NOT a queued invoke per reply: a pan across a city is a few
// hundred tiles, and one QMetaCallEvent each is how the CarPlay widget fell
// behind before it was rewritten this way.
//
// The cache is keyed by TileId across zooms, so zooming out and back in does
// not re-fetch. It holds decoded mvt::Tile objects rather than raw bytes
// because decode is the expensive half and the bytes are useless without it.
//
// It also holds the TESSELLATED form, and tessellates it on the zenoh thread
// alongside the decode. Turning a tile into triangles was measured at ~1.9 ms,
// which is several frames' worth if it happens on the GUI thread during a pan --
// and it is the same triangles every frame until the tile is evicted. Doing it
// once, off the GUI thread, is what makes the camera free to move.
//
// That works only because the style cannot change under a live widget: a config
// change rebuilds the child (editor/selection_frame.h), so the style handed in
// at construction is the style for this TileSource's whole life. If that ever
// stops being true, the geometry cache needs a style revision to invalidate on.
#ifndef MAP_TILE_SOURCE_H
#define MAP_TILE_SOURCE_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mvt/tile.h"

#include "map/projection.h"
#include "map/style.h"
#include "map/tessellator.h"

namespace pub_sub
{
template <typename Req, typename Resp>
class ZenohAsyncClient;
}

struct MapTileRequest;
struct MapTileResponse;

namespace map_widget
{

struct TileSourceStats
{
    std::uint64_t requested { 0 };
    std::uint64_t decoded { 0 };
    // The server had nothing there. Expected and common: coverage is sparse.
    std::uint64_t absent { 0 };
    // A reply that did not decode, or never came.
    std::uint64_t failed { 0 };
    // Tiles currently in backoff after a failure, and so deliberately not
    // being asked for. A steady non-zero here with a dead server is the
    // system working, not the system stuck.
    std::size_t backingOff { 0 };
    std::size_t cached { 0 };
    std::size_t inFlight { 0 };
};

// One tile, in both the forms the widget draws from: the triangles for the GPU
// and the decoded features for the label pass.
//
// Both, because labels are placed on the CPU with viewport-global collision and
// so cannot be baked per tile the way the geometry is. Keeping the mvt::Tile is
// what pays for that; see map/labels.h.
struct CachedTile
{
    std::shared_ptr<const mvt::Tile> tile;
    std::shared_ptr<const TileGeometry> geometry;

    explicit operator bool() const { return tile != nullptr; }
};

class TileSource
{
  public:
    // `onTilesReady` is called on the GUI thread whenever newly decoded tiles
    // have landed, which is the widget's cue to repaint. It is NOT called per
    // tile: a pan produces one repaint per drain, not one per reply.
    //
    // `style` is copied and is fixed for this object's lifetime -- see the
    // header comment.
    TileSource(std::string tilesetName, std::string tileKey, std::uint64_t timeoutMs,
               MapStyle_t style, std::function<void()> onTilesReady);
    ~TileSource();

    TileSource(const TileSource&) = delete;
    TileSource& operator=(const TileSource&) = delete;

    // Ask for everything in `wanted` that is not already cached or in flight.
    // Called from the GUI thread on every camera change; cheap when nothing has
    // moved.
    void request(const std::vector<TileId>& wanted);

    // What is available to draw right now, in the order asked for. Tiles that
    // have not arrived come back default-constructed -- a partially filled map
    // is the normal state during a pan, not an error.
    std::vector<CachedTile> ready(const std::vector<TileId>& wanted) const;

    // Move anything that arrived since the last call out of the mailbox and
    // into the cache. GUI thread only.
    //
    // Returns how many tiles were taken, so the caller can skip a repaint when
    // the answer is zero.
    std::size_t drain();

    TileSourceStats stats() const;

    // Drop everything. Used when the tileset changes under the widget.
    void clear();

  private:
    void deliver(const TileId& id, CachedTile tile, bool absent, bool failed);
    void evictIfNeeded();

    std::string mTileset;
    // Read from zenoh threads during tessellation and never written after
    // construction, so no lock.
    MapStyle_t mStyle;
    std::function<void()> mOnTilesReady;

    std::unique_ptr<pub_sub::ZenohAsyncClient<::MapTileRequest, ::MapTileResponse>> mClient;

    // Written from zenoh threads, drained on the GUI thread. The mutex covers
    // only the mailbox and the in-flight set, and is never held across a
    // decode.
    mutable std::mutex mMutex;
    std::vector<std::pair<TileId, CachedTile>> mMailbox;
    // Tiles whose request failed, waiting to be folded into mBackoff by the
    // next drain. Separate from the mailbox because there is nothing to cache
    // -- only the fact of the failure.
    std::vector<TileId> mFailures;
    std::unordered_set<TileId, TileIdHash> mInFlight;
    TileSourceStats mStats;

    // GUI thread only, so no lock.
    std::unordered_map<TileId, CachedTile, TileIdHash> mCache;
    // Insertion order, for eviction. A tile is cheap to re-fetch and expensive
    // to keep: a z14 tile decodes to a few hundred kilobytes of vectors, and an
    // unbounded cache over a long drive is unbounded memory.
    std::deque<TileId> mCacheOrder;

    // A failed tile is not cached -- there is nothing to draw -- so without
    // this it is re-requested on the very next paint, forever. Against a
    // server that is down, that is a permanent kMaxInFlight-deep queue of
    // queries at fix rate, each waiting out the full timeout.
    //
    // Absence of a tile is NOT a failure: that answer is cached as an empty
    // tile and never lands here. This is only for a reply that errored or
    // never came, which is the case that can recover -- so it backs off rather
    // than giving up.
    struct Backoff
    {
        // Consecutive failures, for the doubling.
        unsigned attempts { 0 };
        std::chrono::steady_clock::time_point retryAt;
    };
    std::unordered_map<TileId, Backoff, TileIdHash> mBackoff;
};

} // namespace map_widget

#endif // MAP_TILE_SOURCE_H
