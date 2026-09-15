#ifndef SCOPE_MAP_TILE_READER_H_
#define SCOPE_MAP_TILE_READER_H_

// Tiles, read straight out of an .mbtiles file. No map_server, no zenoh, no bus.
//
// The counterpart of dashboard/widgets/map's TileSource, and deliberately NOT a
// subclass of it or of anything shared. They answer the same question -- "what
// can I draw here" -- from opposite ends: TileSource holds a zenoh client and
// batches queries at a node; this holds an open SQLite file. Everything
// downstream of the bytes (decode, tessellate, extract labels, cache) is the
// same, and that half is shared through libs/map_render.
//
// WHAT THE LOCAL PATH LETS US DROP, and it is most of TileSource's complexity:
//
//   * No backoff and no retry timer. A read succeeds, is absent, or fails with
//     a reason from SQLite. There is no server that might come up later, so
//     nextRetryAt(), the in-flight/past-due exclusions and the
//     single-arming-site rule all have nothing to do here.
//   * No zoom-range learning. metadata().minzoom/maxzoom is known the moment
//     the file opens, so there is no takeArchiveRangeLearned() and no repaint
//     that has to be triggered by it. The caller clamps before asking.
//   * No outOfRange/badRequest distinction. The caller clamps its tile zoom to
//     zoomRange() and its x/y to 2^z, so a coordinate outside the pyramid is
//     never formed. checkTileRange() stays in nodes/map_server, where a client
//     it does not control can send anything.
//
// THREADING, which is the one thing that is not simpler. Reads happen on a
// dedicated thread, never the GUI thread: a warm tile read is ~0.015 ms but
// decode plus tessellation is ~1.9 ms per tile, and a viewport batch is dozens.
// That thread spreads a batch across map_render::TileWorkers -- the same pool,
// used the same way, standing in for the zenoh reply thread one for one -- and
// hands results to the GUI thread through a mailbox drained by drain().
#include "map_render/style.h"
#include "map_render/tile_cache.h"
#include "map_render/tile_workers.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace mbtiles
{
class Archive;
}

namespace scope
{

struct TileReaderStats
{
    std::uint64_t requested { 0 };
    std::uint64_t decoded { 0 };
    // The archive has nothing there. Expected and common: most of the pyramid
    // is empty, and an absent tile is cached as absent so it is never asked
    // for twice.
    std::uint64_t absent { 0 };
    // A read or a decode that failed. Distinct from absent because absence is a
    // fact about a coordinate and this is a fault.
    std::uint64_t failed { 0 };
    std::size_t cached { 0 };
    std::size_t cachedBytes { 0 };
    std::size_t inFlight { 0 };
};

class TileReader
{
  public:
    // Opens the archive. An archive that will not open is KEPT, with its error,
    // rather than the object failing to construct: "that tileset is not
    // configured" and "it is configured and cannot be read" are different
    // answers, and a panel says which. Check ok() before believing anything
    // else.
    //
    // `style` is copied and fixed for this object's lifetime -- tessellation
    // bakes colours into vertices and runs on the reader thread, so a style
    // that changed underneath would need a geometry revision to invalidate on.
    // A config change rebuilds the panel instead.
    TileReader(std::string path, MapStyle_t style, std::function<void()> onTilesReady);
    ~TileReader();

    TileReader(const TileReader&) = delete;
    TileReader& operator=(const TileReader&) = delete;

    bool ok() const { return archive_ != nullptr; }
    const std::string& error() const { return error_; }
    const std::string& path() const { return path_; }

    // What the archive holds, known at open. Meaningless when !ok().
    struct ZoomRange
    {
        std::uint8_t min { 0 };
        std::uint8_t max { 0 };
    };
    ZoomRange zoomRange() const { return zoom_range_; }

    // Ask for everything in `wanted` that is not already cached or queued.
    // GUI thread; cheap when nothing has moved.
    void request(const std::vector<map_render::TileId>& wanted);

    // What is drawable right now, in the order asked for, written into `out`
    // (cleared first, capacity kept -- the paint pass reuses its scratch).
    // Tiles that have not arrived come back default-constructed; a partially
    // filled map is the normal state during a pan, not an error.
    //
    // NOT const: asking marks a tile as in use, which is what keeps the ground
    // being drawn out of the eviction queue.
    void ready(const std::vector<map_render::TileId>& wanted,
               std::vector<map_render::CachedTile>& out);

    // Move anything that arrived since the last call into the cache. Returns
    // how many, so the caller can decide whether a repaint is warranted. GUI
    // thread only.
    std::size_t drain();

    // Cached AND carrying geometry worth drawing. Not the same as "is it
    // cached": an absent tile is cached too, with nothing in it, and as a
    // stand-in it would occupy a draw slot and paint nothing.
    bool drawable(const map_render::TileId& id);

    TileReaderStats stats() const;

  private:
    void readerLoop();
    // Read, decode, tessellate and extract labels for one batch. On the reader
    // thread.
    void serve(const std::vector<map_render::TileId>& batch);

    std::string path_;
    std::string error_;
    // Read from the reader thread and from TileWorkers during tessellation,
    // never written after construction, so no lock.
    MapStyle_t style_;
    std::function<void()> on_tiles_ready_;
    ZoomRange zoom_range_;

    // mbtiles::Archive is thread-safe over a capped pool of read-only
    // connections, so the worker threads can read through it directly.
    std::unique_ptr<mbtiles::Archive> archive_;

    // DECLARED BEFORE THE THREAD, so it is destroyed AFTER it -- members go in
    // reverse declaration order, and a pool torn down while the reader thread
    // is still inside runAll() joins threads that are mid-job.
    map_render::TileWorkers workers_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::vector<map_render::TileId>> queue_;
    std::vector<std::pair<map_render::TileId, map_render::CachedTile>> mailbox_;
    // Queued or being served. Kept so a repeated request during a pan does not
    // re-queue the same tile every frame.
    std::unordered_set<map_render::TileId, map_render::TileIdHash> in_flight_;
    bool stopping_ { false };
    TileReaderStats stats_;

    // GUI thread only; no lock.
    map_render::TileCache cache_;

    // LAST. The thread's loop touches every member above, so it must not start
    // until they are constructed and must be joined before they are destroyed.
    std::thread thread_;
};

}  // namespace scope

#endif  // SCOPE_MAP_TILE_READER_H_
