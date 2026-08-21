// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/tile_source.h"

#include "mvt/decode.h"
#include "mvt/gzip.h"

#include "pub_sub/zenoh_async_client.h"

#include "map_tiles.capnp.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <variant>

namespace map_widget
{
namespace
{

// How many tiles one request may name.
//
// A viewport plus its prefetch ring is a few dozen, so this is normally the
// whole screen in one query. The cap is not about the wire -- it is about a
// fast pan queuing more than anybody will look at: whatever does not fit is
// asked for on the next drain, at most a frame away, and because the request
// list is ordered centre-outward what gets left behind is what is furthest
// from where the driver is looking.
constexpr std::size_t kMaxTilesPerRequest = 64;

// How long to wait before asking again for a tile whose request FAILED, and
// the ceiling the doubling stops at. The first wait is deliberately short: the
// common failure is map_server not being up yet when the dashboard starts, and
// a driver should not wait 30 s for the map once it does come up.
constexpr std::chrono::milliseconds kFirstRetry { 500 };
constexpr std::chrono::milliseconds kMaxRetry { 30000 };

std::chrono::milliseconds retryDelay(unsigned attempts)
{
    std::chrono::milliseconds delay = kFirstRetry;
    for (unsigned i = 1; i < attempts && delay < kMaxRetry; ++i)
    {
        delay *= 2;
    }
    return std::min(delay, kMaxRetry);
}

using Client = pub_sub::ZenohAsyncClient<::MapTileRequest, ::MapTileResponse>;

} // namespace

TileSource::TileSource(std::string tilesetName, std::string tileKey, std::uint64_t timeoutMs,
                       MapStyle_t style, std::function<void()> onTilesReady) :
    mTileset(std::move(tilesetName)),
    mStyle(std::move(style)),
    mOnTilesReady(std::move(onTilesReady)),
    mClient(std::make_unique<Client>(std::move(tileKey), timeoutMs))
{
}

TileSource::~TileSource() = default;

void TileSource::request(const std::vector<TileId>& wanted)
{
    const auto now = std::chrono::steady_clock::now();

    // ONE request for the whole viewport, not one per tile. A pan used to
    // issue a few dozen zenoh queries to fill one screen, each with its own
    // capnp message, its own round trip and its own reply -- and needed a
    // 32-slot in-flight budget to stop a fast pan queuing thousands.
    std::vector<TileId> ask;
    ask.reserve(std::min(wanted.size(), kMaxTilesPerRequest));

    {
        const std::lock_guard<std::mutex> guard(mMutex);
        for (const TileId& id : wanted)
        {
            if (ask.size() >= kMaxTilesPerRequest)
            {
                // Not dropped -- deferred. The rest is asked for on the next
                // paint, at most a frame away, and because the list is ordered
                // centre-outward what waits is what is furthest from where the
                // driver is looking. Counted so that a cap which is biting
                // every single frame is visible rather than inferred from a map
                // that fills in slowly.
                mStats.deferred += wanted.size() - ask.size();
                break;
            }
            if (mCache.contains(id))
            {
                continue;
            }
            if (const auto backoff = mBackoff.find(id); backoff != mBackoff.end())
            {
                if (now < backoff->second.retryAt)
                {
                    continue;
                }
                // Due for another go. The entry stays until the retry
                // succeeds, so a second failure doubles the wait rather than
                // restarting it.
            }
            if (!mInFlight.insert(id).second)
            {
                continue;
            }
            ask.push_back(id);
        }
        mStats.requested += ask.size();
    }

    if (ask.empty())
    {
        return;
    }

    const bool sent = mClient->request(
        [this, &ask](::MapTileRequest::Builder& builder) {
            builder.setTileset(mTileset);
            auto list = builder.initTiles(static_cast<unsigned>(ask.size()));
            for (unsigned i = 0; i < ask.size(); ++i)
            {
                auto coord = list[i];
                coord.setZ(ask[i].z);
                coord.setX(ask[i].x);
                coord.setY(ask[i].y);
            }
        },
        [this, ask](Client::Status status, const ::MapTileResponse::Reader* reply) {
            // ON A ZENOH THREAD. Nothing here may touch Qt.
            if (status != Client::Status::Ok || reply == nullptr)
            {
                failBatch(ask);
                return;
            }

            bool learned = false;
            {
                // Before anything else: an archive does not grow levels under
                // a running server, so one reply is enough and every later one
                // says the same thing. Learning it here is what stops the
                // widget ever asking out of range again -- see map_tiles.capnp
                // for why the per-tile answers cannot be used for this.
                std::lock_guard<std::mutex> lock(mMutex);
                if (!mArchiveZoom.has_value())
                {
                    mArchiveZoom = ZoomRange { reply->getMinzoom(), reply->getMaxzoom() };
                    mArchiveZoomLearned = true;
                    learned = true;
                }
            }

            // Outside the lock, and unconditionally: the widget may have asked
            // at a level this archive does not have, in which case nothing will
            // arrive in the mailbox to prompt a repaint and the newly known
            // range would sit unused until something else moved the camera.
            if (learned && mOnTilesReady)
            {
                mOnTilesReady();
            }

            switch (reply->getStatus())
            {
                case ::MapStatus::OK:
                    break;

                case ::MapStatus::UNKNOWN:
                case ::MapStatus::NOT_FOUND:
                case ::MapStatus::OUT_OF_RANGE:
                case ::MapStatus::NO_SUCH_TILESET:
                case ::MapStatus::BAD_REQUEST:
                case ::MapStatus::FAILED:
                    // The whole REQUEST failed -- a bad tileset name, an
                    // archive that will not open. Every tile in it is a
                    // failure, not an absence: absence is a fact about a
                    // working archive and would be cached forever.
                    SPDLOG_ERROR("[map] tileset '{}': {}", mTileset, reply->getError().cStr());
                    failBatch(ask);
                    return;
            }

            // Decoded and tessellated ACROSS THREADS, blocking until every one
            // is done. A batch is up to 64 tiles at ~1.9 ms each, which is a
            // fifth of a second of work that used to run serially on this one
            // zenoh thread -- and that time is exactly how long a pan takes to
            // fill in.
            //
            // Blocking rather than posting and forgetting: `result.getData()`
            // is a span into the capnp reply, which dies with this callback.
            // See map/tile_workers.h.
            const auto tiles = reply->getTiles();
            mWorkers.runAll(tiles.size(), [&](std::size_t i) {
                const auto result = tiles[static_cast<unsigned>(i)];
                const auto coord = result.getCoord();
                const TileId id { coord.getZ(), coord.getX(), coord.getY() };

                Outcome outcome = Outcome::Failed;
                switch (result.getStatus())
                {
                    case ::MapStatus::OK:
                        outcome = Outcome::Ok;
                        break;

                    case ::MapStatus::NOT_FOUND:
                        outcome = Outcome::Absent;
                        break;

                    case ::MapStatus::OUT_OF_RANGE:
                        // Only reachable before the range is known -- one batch
                        // at startup, at most. Deliberately NOT cached as
                        // absent: that would be a permanent "nothing here" for
                        // a coordinate the archive simply does not go to, and
                        // it would survive a server restart onto a deeper
                        // archive.
                        outcome = Outcome::OutOfRange;
                        break;

                    case ::MapStatus::UNKNOWN:
                    case ::MapStatus::NO_SUCH_TILESET:
                    case ::MapStatus::BAD_REQUEST:
                    case ::MapStatus::FAILED:
                        SPDLOG_ERROR("[map] tile {}/{}/{}: {}", id.z, id.x, id.y,
                                     result.getError().cStr());
                        outcome = Outcome::Failed;
                        break;
                }

                const auto data = result.getData();
                deliverResult(id, outcome,
                              std::span<const std::uint8_t>(
                                  reinterpret_cast<const std::uint8_t*>(data.begin()),
                                  data.size()));
            });
        });

    if (!sent)
    {
        failBatch(ask);
    }
}

void TileSource::deliverResult(const TileId& id, Outcome outcome,
                               std::span<const std::uint8_t> bytes)
{
    switch (outcome)
    {
        case Outcome::Ok:
            break;

        case Outcome::OutOfRange:
            // Nothing to cache and nothing to back off: the range was learned
            // from this very reply, so the tile is not asked for again.
            {
                // Both under the lock. mStats is read by stats() from the GUI
                // thread and this runs on a zenoh one.
                const std::lock_guard<std::mutex> guard(mMutex);
                ++mStats.outOfRange;
                mInFlight.erase(id);
            }
            return;

        case Outcome::Absent:
            // The normal answer for most of the pyramid. Cached as an empty
            // tile so it is not asked for again every frame -- an absent tile
            // re-requested forever is a steady stream of queries for nothing.
            deliver(id,
                    CachedTile { std::make_shared<const mvt::Tile>(),
                                 std::make_shared<const TileGeometry>() },
                    true, false);
            return;

        case Outcome::Failed:
            deliver(id, CachedTile {}, false, true);
            return;
    }

    // Inflate first: the server ships tiles exactly as the archive stored
    // them, which for vector tiles is gzipped. Feeding gzip to the decoder
    // does not fail loudly enough to be obvious.
    auto raw = mvt::inflateIfCompressed(bytes);
    if (!raw)
    {
        SPDLOG_ERROR("[map] tile {}/{}/{}: {}", id.z, id.x, id.y, mvt::to_string(raw.error()));
        deliver(id, CachedTile {}, false, true);
        return;
    }

    auto tile = mvt::decode(*raw);
    if (!tile)
    {
        SPDLOG_ERROR("[map] tile {}/{}/{}: {}", id.z, id.x, id.y, mvt::to_string(tile.error()));
        deliver(id, CachedTile {}, false, true);
        return;
    }

    auto decoded = std::make_shared<const mvt::Tile>(std::move(*tile));

    // Tessellated HERE, on the zenoh thread, not on the GUI thread when the
    // tile is first drawn. It is the expensive step by an order of magnitude
    // and its result is reused every frame; doing it on arrival means the
    // frame that shows a new tile costs the same as the frame before it.
    auto geometry = std::make_shared<const TileGeometry>(tessellate(*decoded, mStyle));

    deliver(id, CachedTile { std::move(decoded), std::move(geometry) }, false, false);
}

void TileSource::failBatch(const std::vector<TileId>& ids)
{
    for (const TileId& id : ids)
    {
        deliver(id, CachedTile {}, false, true);
    }
}

void TileSource::deliver(const TileId& id, CachedTile tile, bool absent, bool failed)
{
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mInFlight.erase(id);

        if (absent)
        {
            ++mStats.absent;
        }
        else if (failed)
        {
            ++mStats.failed;
        }
        else
        {
            ++mStats.decoded;
        }

        if (tile)
        {
            mMailbox.emplace_back(id, std::move(tile));
        }
        else if (failed)
        {
            mFailures.push_back(id);
        }
    }

    // Outside the lock. The callback hops to the GUI thread and could, in
    // principle, re-enter -- and holding a lock across a thread hand-off is how
    // a UI stops for the length of a network timeout.
    if (mOnTilesReady)
    {
        mOnTilesReady();
    }
}

TileSource::Drained TileSource::drain()
{
    std::vector<std::pair<TileId, CachedTile>> arrived;
    std::vector<TileId> failures;
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        arrived.swap(mMailbox);
        failures.swap(mFailures);
    }

    const auto now = std::chrono::steady_clock::now();
    for (const TileId& id : failures)
    {
        Backoff& backoff = mBackoff[id];
        ++backoff.attempts;
        backoff.retryAt = now + retryDelay(backoff.attempts);
    }

    for (auto& [id, tile] : arrived)
    {
        // Anything that arrived is working again, including the absent-tile
        // answer -- that is the server talking, not a failure.
        mBackoff.erase(id);

        // What the entry being replaced weighed, read BEFORE insert_or_assign
        // overwrites it. Without this the running total only ever grows.
        // Weighing, ordering and eviction are the cache's business.
        mCache.insert(id, std::move(tile));
    }

    return Drained { arrived.size(), failures.size() };
}

std::optional<std::chrono::steady_clock::time_point> TileSource::nextRetryAt() const
{
    const auto now = std::chrono::steady_clock::now();
    const std::lock_guard<std::mutex> guard(mMutex);
    std::optional<std::chrono::steady_clock::time_point> due;
    for (const auto& [id, backoff] : mBackoff)
    {
        // Already asked again: its reply (or timeout) is the wake-up.
        if (mInFlight.contains(id))
        {
            continue;
        }
        // Due, and the last paint did not ask: the tile has left the viewport.
        // If it scrolls back in, that paint's request() retries it on the
        // spot; a timer for it here would fire, paint, skip it and spin.
        if (backoff.retryAt <= now)
        {
            continue;
        }
        due = due ? std::min(*due, backoff.retryAt) : backoff.retryAt;
    }
    return due;
}


std::vector<CachedTile> TileSource::ready(const std::vector<TileId>& wanted)
{
    std::vector<CachedTile> out;
    out.reserve(wanted.size());

    for (const TileId& id : wanted)
    {
        // Asking counts as use, which is what keeps the ground the driver is
        // looking at out of the eviction queue. See TileCache.
        const CachedTile* found = mCache.find(id);
        out.push_back(found != nullptr ? *found : CachedTile {});
    }

    return out;
}

bool TileSource::drawable(const TileId& id)
{
    return mCache.drawable(id);
}

std::optional<TileSource::ZoomRange> TileSource::archiveZoomRange() const
{
    const std::lock_guard<std::mutex> guard(mMutex);
    return mArchiveZoom;
}

bool TileSource::takeArchiveRangeLearned()
{
    const std::lock_guard<std::mutex> guard(mMutex);
    const bool learned = mArchiveZoomLearned;
    mArchiveZoomLearned = false;
    return learned;
}

TileSourceStats TileSource::stats() const
{
    const std::lock_guard<std::mutex> guard(mMutex);
    TileSourceStats out = mStats;
    out.cached = mCache.size();
    out.cachedBytes = mCache.bytes();
    out.inFlight = mInFlight.size();
    out.backingOff = mBackoff.size();
    return out;
}

void TileSource::clear()
{
    {
        const std::lock_guard<std::mutex> guard(mMutex);
        mMailbox.clear();
        // In-flight requests are deliberately NOT cleared: their callbacks are
        // still going to fire, and forgetting them here would let the same tile
        // be requested again immediately. They land in the mailbox and are
        // dropped by the next drain into an empty cache, which costs one tile
        // and no correctness.
    }

    mCache.clear();
    // Cleared too: a new tileset is a different question, and the old one's
    // failures say nothing about it.
    mBackoff.clear();
}

} // namespace map_widget
