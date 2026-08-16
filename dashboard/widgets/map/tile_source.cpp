// SPDX-License-Identifier: GPL-3.0-or-later

#include "map/tile_source.h"

#include "mvt/decode.h"
#include "mvt/gzip.h"

#include "pub_sub/zenoh_async_client.h"

#include "map_tiles.capnp.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>

namespace map_widget
{
namespace
{

// How many decoded tiles to keep. A z14 tile decodes to a few hundred kilobytes
// of vectors, so this is tens of megabytes at the ceiling -- enough for a
// viewport, its surroundings, and a couple of zoom levels either side, which is
// what makes a pinch-zoom instant rather than a refetch.
constexpr std::size_t kMaxCachedTiles = 256;

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

            for (const auto result : reply->getTiles())
            {
                const auto coord = result.getCoord();
                const TileId id { coord.getZ(), coord.getX(), coord.getY() };

                Outcome outcome = Outcome::Failed;
                switch (result.getStatus())
                {
                    case ::MapStatus::OK:
                        outcome = Outcome::Ok;
                        break;

                    case ::MapStatus::NOT_FOUND:
                    case ::MapStatus::OUT_OF_RANGE:
                        outcome = Outcome::Absent;
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
            }
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

std::size_t TileSource::drain()
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

        // insert_or_assign, not insert: a tile re-fetched after eviction must
        // replace the old entry, and a duplicate reply must not push a second
        // eviction record for the same id.
        if (mCache.insert_or_assign(id, std::move(tile)).second)
        {
            mCacheOrder.push_back(id);
        }
    }

    evictIfNeeded();
    return arrived.size();
}

void TileSource::evictIfNeeded()
{
    while (mCacheOrder.size() > kMaxCachedTiles)
    {
        const TileId oldest = mCacheOrder.front();
        mCacheOrder.pop_front();
        mCache.erase(oldest);
    }
}

std::vector<CachedTile> TileSource::ready(const std::vector<TileId>& wanted) const
{
    std::vector<CachedTile> out;
    out.reserve(wanted.size());

    for (const TileId& id : wanted)
    {
        const auto found = mCache.find(id);
        out.push_back((found == mCache.end()) ? CachedTile {} : found->second);
    }

    return out;
}

TileSourceStats TileSource::stats() const
{
    const std::lock_guard<std::mutex> guard(mMutex);
    TileSourceStats out = mStats;
    out.cached = mCache.size();
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
    mCacheOrder.clear();
    // Cleared too: a new tileset is a different question, and the old one's
    // failures say nothing about it.
    mBackoff.clear();
}

} // namespace map_widget
