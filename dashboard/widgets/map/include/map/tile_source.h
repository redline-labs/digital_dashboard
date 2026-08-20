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

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mvt/tile.h"

#include "map/projection.h"
#include "map/tile_cache.h"
#include "map/tile_workers.h"
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
    // Tiles asked for at a zoom the archive does not hold. Only reachable
    // before the first reply teaches the range, so a number that keeps
    // climbing means the range is not being applied.
    std::uint64_t outOfRange { 0 };
    // Tiles a request could not carry because it was already full, summed over
    // every request. They are not lost -- the next paint asks again -- but a
    // number climbing every frame means the viewport is permanently larger than
    // one request, which is the difference between "filling in" and "stuck".
    std::uint64_t deferred { 0 };
    std::size_t cached { 0 };
    // What those tiles weigh, decoded and tessellated. The count alone does not
    // say: an empty ocean tile and a city tile differ by three orders of
    // magnitude, and it is the bytes that decide whether a long drive fits in
    // memory.
    std::size_t cachedBytes { 0 };
    std::size_t inFlight { 0 };
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
    //
    // NOT const: asking for a tile is what marks it as in use, and that is what
    // keeps the cache from evicting the ground the driver is looking at. See
    // the eviction note below.
    std::vector<CachedTile> ready(const std::vector<TileId>& wanted);

    // Move anything that arrived since the last call out of the mailbox and
    // into the cache. GUI thread only.
    //
    // Returns how many tiles were taken, so the caller can skip a repaint when
    // the answer is zero.
    std::size_t drain();

    // Cached AND carrying geometry worth drawing.
    //
    // Not the same question as "is it cached": an ABSENT tile is cached too,
    // with nothing in it, so that it is not asked for again. As a stand-in for
    // some other tile it would occupy a draw slot and paint nothing.
    //
    // GUI thread only, like ready() -- and, like ready(), asking counts as use.
    // A tile serving as a stand-in is on screen, so it must not be the next
    // thing evicted.
    bool drawable(const TileId& id);

    TileSourceStats stats() const;

    // The zoom levels the archive actually holds, as the server reported them.
    // Empty until the first reply lands.
    //
    // This is what the widget clamps its tile requests to. It is NOT the
    // camera's range -- a camera zoomed past the deepest level draws that level
    // magnified, which for vector tiles stays sharp. See MapConfig_t.
    struct ZoomRange
    {
        std::uint8_t min { 0 };
        std::uint8_t max { 0 };
    };
    std::optional<ZoomRange> archiveZoomRange() const;

    // True ONCE, the first time the server's range arrives. The widget has to
    // repaint on it and cannot infer it from drain(): the range changes which
    // tile level the next paint asks for, and the case that needs it most is
    // the one where nothing was drained at all -- a camera parked past the
    // archive, every tile back as outOfRange, an empty mailbox and no other
    // reason to redraw.
    bool takeArchiveRangeLearned();

    // Drop everything. Used when the tileset changes under the widget.
    void clear();

  private:
    void deliver(const TileId& id, CachedTile tile, bool absent, bool failed);
    // What the server said about one tile of a batch. Decomposed from the
    // capnp status here rather than passed through, so this header stays free
    // of the generated schema -- the switch that maps MapStatus onto it lives
    // in the .cpp, where -Wswitch-enum can see it.
    enum class Outcome
    {
        // Bytes to decode.
        Ok,
        // The archive has nothing there. NORMAL, and cached as an empty tile.
        Absent,
        // The archive does not go to that ZOOM LEVEL at all. Not the same as
        // Absent: absence is a fact about a coordinate and is worth caching
        // forever, while this is a fact about the request, and once the range
        // is known the tile is simply never asked for again.
        OutOfRange,
        // The server could not answer. Backed off, never cached.
        Failed,
    };

    // Decode, tessellate and hand one tile to the mailbox. On a zenoh thread.
    void deliverResult(const TileId& id, Outcome outcome, std::span<const std::uint8_t> bytes);
    // Mark every tile of a batch failed -- the request itself did not land.
    void failBatch(const std::vector<TileId>& ids);

    std::string mTileset;
    // Read from zenoh threads during tessellation and never written after
    // construction, so no lock.
    MapStyle_t mStyle;
    std::function<void()> mOnTilesReady;

    // Decode and tessellation, spread across threads.
    //
    // DECLARED BEFORE THE CLIENT, so it is destroyed AFTER it: members go in
    // reverse declaration order, and the reply callback that dispatches into
    // this pool belongs to the client. A pool torn down while a callback is
    // still inside runAll() joins threads that are mid-job.
    TileWorkers mWorkers;

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
    // Learned from the first reply and unchanged after -- an archive does not
    // grow levels under a running server. Guarded by mMutex because it is
    // written on a zenoh thread and read on the GUI thread.
    std::optional<ZoomRange> mArchiveZoom;
    bool mArchiveZoomLearned { false };

    // GUI thread only, so no lock. The eviction policy -- least recently used,
    // bounded by both count and bytes -- lives in TileCache, which is testable
    // without a zenoh session.
    TileCache mCache;

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
