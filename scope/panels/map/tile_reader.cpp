#include "map_panel/tile_reader.h"

#include "mbtiles/archive.h"
#include "mbtiles/error.h"

#include "map_render/labels.h"
#include "map_render/tessellator.h"

#include "mvt/decode.h"
#include "mvt/gzip.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace scope
{
namespace
{

// How many tiles one queued batch may hold. Not a wire limit -- there is no
// wire -- but a work-splitting one: TileWorkers::runAll blocks until the whole
// batch is done, so an unbounded batch is an unbounded time before anything
// reaches the mailbox and the map fills in all at once at the end instead of
// progressively.
constexpr std::size_t kMaxTilesPerBatch = 64;

}  // namespace

TileReader::TileReader(std::string path, MapStyle_t style, std::function<void()> onTilesReady)
    : path_(std::move(path)), style_(std::move(style)), on_tiles_ready_(std::move(onTilesReady))
{
    auto archive = mbtiles::Archive::open(path_);
    if (!archive)
    {
        // Kept, not thrown. A panel needs to say WHY there is no map, and
        // "configured but unreadable" is a different sentence from "not
        // configured" -- collapsing them makes a permissions problem look like
        // a typo.
        error_ = mbtiles::to_string(archive.error());
        SPDLOG_ERROR("[map] {}: {}", path_, error_);
        return;
    }

    archive_ = std::make_unique<mbtiles::Archive>(std::move(*archive));

    const mbtiles::Metadata& meta = archive_->metadata();
    zoom_range_ = {meta.minzoom, meta.maxzoom};

    thread_ = std::thread([this]() { readerLoop(); });
}

TileReader::~TileReader()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();

    if (thread_.joinable())
    {
        thread_.join();
    }
}

void TileReader::request(const std::vector<map_render::TileId>& wanted)
{
    if (!ok())
    {
        return;
    }

    std::vector<map_render::TileId> batch;
    batch.reserve(std::min(wanted.size(), kMaxTilesPerBatch));

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const map_render::TileId& id : wanted)
        {
            // contains() rather than drawable(): an absent tile is cached with
            // nothing in it precisely so it is never asked for again, and
            // contains() deliberately does not count as use -- the caller asks
            // this of the whole viewport plus its prefetch ring every paint,
            // and treating that as use would protect exactly the speculative
            // tiles that should be evicted first.
            if (cache_.contains(id) || in_flight_.contains(id))
            {
                continue;
            }
            in_flight_.insert(id);
            batch.push_back(id);
            ++stats_.requested;

            if (batch.size() == kMaxTilesPerBatch)
            {
                queue_.push_back(std::move(batch));
                batch.clear();
                batch.reserve(kMaxTilesPerBatch);
            }
        }

        if (!batch.empty())
        {
            queue_.push_back(std::move(batch));
        }

        if (queue_.empty())
        {
            return;
        }
    }

    wake_.notify_one();
}

void TileReader::ready(const std::vector<map_render::TileId>& wanted,
                       std::vector<map_render::CachedTile>& out)
{
    out.clear();
    out.reserve(wanted.size());
    for (const map_render::TileId& id : wanted)
    {
        const map_render::CachedTile* tile = cache_.find(id);
        out.push_back(tile != nullptr ? *tile : map_render::CachedTile{});
    }
}

std::size_t TileReader::drain()
{
    std::vector<std::pair<map_render::TileId, map_render::CachedTile>> arrived;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        arrived.swap(mailbox_);
    }

    for (auto& [id, tile] : arrived)
    {
        cache_.insert(id, std::move(tile));
    }

    return arrived.size();
}

bool TileReader::drawable(const map_render::TileId& id)
{
    return cache_.drawable(id);
}

TileReaderStats TileReader::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    TileReaderStats out = stats_;
    out.cached = cache_.size();
    out.cachedBytes = cache_.bytes();
    out.inFlight = in_flight_.size();
    return out;
}

void TileReader::readerLoop()
{
    for (;;)
    {
        std::vector<map_render::TileId> batch;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_)
            {
                return;
            }
            batch = std::move(queue_.front());
            queue_.pop_front();
        }

        serve(batch);

        if (on_tiles_ready_)
        {
            on_tiles_ready_();
        }
    }
}

void TileReader::serve(const std::vector<map_render::TileId>& batch)
{
    std::vector<map_render::CachedTile> results(batch.size());
    std::vector<int> outcomes(batch.size(), 0);  // 0 ok, 1 absent, 2 failed

    // Blocking parallel-for, exactly as TileSource uses it for a zenoh reply:
    // this thread had all of this work to do serially anyway, and blocking here
    // keeps every borrowed buffer alive for the duration of the job.
    workers_.runAll(batch.size(), [&](std::size_t i) {
        const map_render::TileId& id = batch[i];

        // The BORROWING overload: the sink is handed SQLite's bytes directly and
        // they are decoded in place, so a tile never lands in an intermediate
        // vector on the way. The span is valid only inside the callback.
        bool present = false;
        auto read = archive_->tile(
            id.z, id.x, id.y,
            [&](std::span<const std::uint8_t> bytes, mbtiles::Encoding /*encoding*/) {
                present = true;

                auto raw = mvt::inflateIfCompressed(bytes);
                if (!raw)
                {
                    outcomes[i] = 2;
                    return;
                }
                auto tile = mvt::decode(*raw);
                if (!tile)
                {
                    outcomes[i] = 2;
                    return;
                }

                map_render::CachedTile cached;
                cached.geometry = std::make_shared<const map_render::TileGeometry>(
                    map_render::tessellate(*tile, style_));
                cached.labels = std::make_shared<const map_render::LabelSet>(
                    map_render::extractLabels(*tile));
                cached.bytes = map_render::approximateBytes(cached);
                results[i] = std::move(cached);
            });

        if (!read)
        {
            outcomes[i] = 2;
            SPDLOG_ERROR("[map] {} {}/{}/{}: {}", path_, id.z, id.x, id.y,
                         mbtiles::to_string(read.error()));
            return;
        }

        if (!present)
        {
            // The common answer, and not logged: the caller asks for whatever
            // is under the viewport and most of the pyramid is empty.
            outcomes[i] = 1;
        }
    });

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (std::size_t i = 0; i < batch.size(); ++i)
        {
            in_flight_.erase(batch[i]);

            switch (outcomes[i])
            {
                case 0:
                    ++stats_.decoded;
                    break;
                case 1:
                    ++stats_.absent;
                    break;
                default:
                    ++stats_.failed;
                    break;
            }

            // ABSENT TILES ARE CACHED, empty. Without that the caller
            // re-requests every hole in the pyramid on every paint, forever.
            // A FAILED tile is not: it is a fault, and caching it would make a
            // transient read error permanent for the life of the panel.
            if (outcomes[i] != 2)
            {
                mailbox_.emplace_back(batch[i], std::move(results[i]));
            }
        }
    }
}

}  // namespace scope
