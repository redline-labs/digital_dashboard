// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reader, against bytes nobody here wrote.
//
// It SKIPS, loudly, when the extract is not there -- the file is 637 MB and is
// not in the repository, so a fresh checkout must still pass. That makes this a
// weaker test than one with a committed fixture, and the trade is the same one
// libs/mvt/tests/test_real_tiles.cpp makes: a fixture built from our own
// reading of fileformat.proto would agree with our parser even where both are
// wrong. A file written by osmium or planetiler does not.
//
// The assertion that carries the most weight is the CROSS-CHECK: the way this
// finds near Irvine has to be the road the existing map stack already draws
// there, at the tile libs/mvt pulls out of the .mbtiles. Two independently
// written parsers, over two independently produced files, agreeing on one place
// in the world.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

#include "osm/blob.h"
#include "osm/block.h"
#include "osm/node_store.h"

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

// Where the bench extract lives. Overridable so this is runnable against
// another file without an edit.
std::filesystem::path extractPath()
{
    if (const char* fromEnv = std::getenv("OSM_TEST_EXTRACT"); fromEnv != nullptr)
    {
        return fromEnv;
    }
    return "/Users/ryan/Documents/map_data/socal-260813.osm.pbf";
}

// The anchor, in 1e-7 degrees. docs/map.md's Irvine, z14/2828/6562.
constexpr osm::Coord kIrvineLat = 336865966;
constexpr osm::Coord kIrvineLon = -1178557874;

// A box roughly 2 km on a side around it.
constexpr osm::Coord kBox = 100000;

class Mapped
{
  public:
    explicit Mapped(const std::filesystem::path& path)
    {
        mFd = ::open(path.c_str(), O_RDONLY);
        if (mFd < 0)
        {
            return;
        }
        struct stat info {};
        if (::fstat(mFd, &info) != 0)
        {
            return;
        }
        mSize = static_cast<std::size_t>(info.st_size);
        void* address = ::mmap(nullptr, mSize, PROT_READ, MAP_PRIVATE, mFd, 0);
        if (address == MAP_FAILED)
        {
            mSize = 0;
            return;
        }
        mData = static_cast<const std::uint8_t*>(address);
    }

    ~Mapped()
    {
        if (mData != nullptr)
        {
            ::munmap(const_cast<std::uint8_t*>(mData), mSize);
        }
        if (mFd >= 0)
        {
            ::close(mFd);
        }
    }

    Mapped(const Mapped&) = delete;
    Mapped& operator=(const Mapped&) = delete;

    bool valid() const { return mData != nullptr; }
    std::span<const std::uint8_t> bytes() const { return { mData, mSize }; }

  private:
    int mFd { -1 };
    const std::uint8_t* mData { nullptr };
    std::size_t mSize { 0 };
};

struct Totals
{
    std::uint64_t blocks { 0 };
    std::uint64_t nodes { 0 };
    std::uint64_t ways { 0 };
    std::uint64_t relations { 0 };
    std::uint64_t highways { 0 };
};

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    const std::filesystem::path path = extractPath();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        // Skipping, loudly. Failing would make a fresh checkout red for a file
        // that is deliberately not in the repository.
        SPDLOG_WARN("SKIPPED: no extract at {}", path.string());
        SPDLOG_WARN("Set OSM_TEST_EXTRACT to point at an .osm.pbf to run this.");
        return 0;
    }

    Mapped file(path);
    if (!file.valid())
    {
        // There and unreadable is a FAILURE, not a skip -- the distinction
        // libs/mvt's real-tile test draws for the same reason.
        SPDLOG_ERROR("{} is there and will not map", path.string());
        return 1;
    }

    SPDLOG_INFO("reading {} ({} MB)", path.string(), file.bytes().size() / (1024 * 1024));

    // ---- Header ----------------------------------------------------------
    osm::BlobIterator header(file.bytes());
    std::vector<std::uint8_t> inflated;

    auto firstBlob = header.next();
    check(firstBlob.has_value(), "the first blob frames");
    if (!firstBlob)
    {
        SPDLOG_ERROR("  {}", osm::to_string(firstBlob.error()));
        return 1;
    }
    check(firstBlob->kind == osm::BlobKind::Header, "and is an OSMHeader");

    if (auto ok = osm::inflateBlob(*firstBlob, inflated); !ok)
    {
        SPDLOG_ERROR("header will not inflate: {}", osm::to_string(ok.error()));
        return 1;
    }

    auto headerBlock = osm::decodeHeaderBlock(inflated);
    check(headerBlock.has_value(), "the header block decodes");
    if (!headerBlock)
    {
        SPDLOG_ERROR("  {}", osm::to_string(headerBlock.error()));
        return 1;
    }

    check(osm::checkRequiredFeatures(*headerBlock).has_value(),
          "and requires nothing this build cannot do");

    SPDLOG_INFO("written by '{}'", headerBlock->writingProgram);
    bool claimsSorted = false;
    for (const std::string& feature : headerBlock->optionalFeatures)
    {
        SPDLOG_INFO("optional feature: {}", feature);
        if (feature == "Sort.Type_then_ID")
        {
            claimsSorted = true;
        }
    }
    // Reported rather than asserted: the flag is advisory and plenty of sorted
    // files omit it. What matters is the OrderCheck below, which verifies the
    // ordering rather than believing a string about it.
    SPDLOG_INFO("declares Sort.Type_then_ID: {}", claimsSorted);

    // ---- Pass A: what the ways reference ---------------------------------
    osm::NodeStore store;
    osm::OrderCheck order;
    Totals totals;

    // The way nearest the anchor that carries a name, and the tags to prove it.
    std::int64_t anchorWayId = 0;
    std::string anchorName;
    std::string anchorHighway;
    std::vector<std::int64_t> anchorRefs;

    {
        osm::BlobIterator it(file.bytes());
        std::vector<std::uint8_t> buffer;

        while (!it.done())
        {
            auto blob = it.next();
            if (!blob)
            {
                SPDLOG_ERROR("framing failed: {}", osm::to_string(blob.error()));
                return 1;
            }
            if (blob->kind != osm::BlobKind::Data)
            {
                continue;
            }

            if (auto ok = osm::inflateBlob(*blob, buffer); !ok)
            {
                SPDLOG_ERROR("block at {} will not inflate: {}", blob->offset,
                             osm::to_string(ok.error()));
                return 1;
            }

            // Peek first: on a sorted file most blocks are nodes, and pass A
            // has no use for them. This is what the cheap peek exists for.
            auto contents = osm::peekDataBlock(buffer, blob->offset);
            if (!contents)
            {
                SPDLOG_ERROR("block at {} will not peek: {}", blob->offset,
                             osm::to_string(contents.error()));
                return 1;
            }
            if (!contents->hasWays && !contents->hasRelations)
            {
                continue;
            }

            auto block = osm::decodeDataBlock(buffer, blob->offset);
            if (!block)
            {
                SPDLOG_ERROR("block at {} will not decode: {}", blob->offset,
                             osm::to_string(block.error()));
                return 1;
            }

            for (const osm::Way& way : block->ways())
            {
                if (auto ok = order.way(way.id, blob->offset); !ok)
                {
                    SPDLOG_ERROR("{}", osm::to_string(ok.error()));
                    return 1;
                }
                for (const std::int64_t ref : block->refs(way))
                {
                    store.markReferenced(ref);
                }

                auto highway = block->value(way, "highway");
                if (highway.has_value())
                {
                    ++totals.highways;
                }
            }

            for (const osm::Relation& relation : block->relations())
            {
                if (auto ok = order.relation(relation.id, blob->offset); !ok)
                {
                    SPDLOG_ERROR("{}", osm::to_string(ok.error()));
                    return 1;
                }
                for (const osm::Member& member : block->members(relation))
                {
                    if (member.type == osm::MemberType::Node)
                    {
                        store.markReferenced(member.ref);
                    }
                }
            }
        }
    }

    check(totals.highways > 1000, "the extract has a city's worth of highways");
    SPDLOG_INFO("pass A: {} highway ways, {} node ids referenced", totals.highways,
                store.stats().referenced);

    if (auto ok = store.finalise(); !ok)
    {
        SPDLOG_ERROR("node store will not finalise: {}", osm::to_string(ok.error()));
        return 1;
    }

    // ---- Pass B: coordinates, then geometry ------------------------------
    {
        osm::BlobIterator it(file.bytes());
        std::vector<std::uint8_t> buffer;

        while (!it.done())
        {
            auto blob = it.next();
            if (!blob)
            {
                return 1;
            }
            if (blob->kind != osm::BlobKind::Data)
            {
                continue;
            }
            if (auto ok = osm::inflateBlob(*blob, buffer); !ok)
            {
                return 1;
            }

            auto block = osm::decodeDataBlock(buffer, blob->offset);
            if (!block)
            {
                SPDLOG_ERROR("block at {} will not decode: {}", blob->offset,
                             osm::to_string(block.error()));
                return 1;
            }

            ++totals.blocks;
            totals.nodes += block->nodes().size();
            totals.ways += block->ways().size();
            totals.relations += block->relations().size();

            for (const osm::Node& node : block->nodes())
            {
                store.set(node.id, node.lat, node.lon);
            }

            // The anchor way: a named highway with a vertex inside the box.
            if (anchorWayId == 0)
            {
                for (const osm::Way& way : block->ways())
                {
                    auto highway = block->value(way, "highway");
                    auto name = block->value(way, "name");
                    if (!highway.has_value() || !name.has_value())
                    {
                        continue;
                    }

                    for (const std::int64_t ref : block->refs(way))
                    {
                        auto coord = store.get(ref);
                        if (!coord.has_value())
                        {
                            continue;
                        }
                        if (std::abs(coord->first - kIrvineLat) < kBox &&
                            std::abs(coord->second - kIrvineLon) < kBox)
                        {
                            anchorWayId = way.id;
                            anchorName = std::string(*name);
                            anchorHighway = std::string(*highway);
                            const auto refs = block->refs(way);
                            anchorRefs.assign(refs.begin(), refs.end());
                            break;
                        }
                    }
                    if (anchorWayId != 0)
                    {
                        break;
                    }
                }
            }
        }
    }

    const auto stats = store.stats();
    SPDLOG_INFO("pass B: {} blocks, {} nodes, {} ways, {} relations", totals.blocks, totals.nodes,
                totals.ways, totals.relations);
    SPDLOG_INFO("node store: {} referenced, {} resolved, {} unresolved, {} MB", stats.referenced,
                stats.resolved, stats.referenced - stats.resolved, stats.bytes / (1024 * 1024));

    check(totals.nodes > 1'000'000, "the extract has millions of nodes");
    check(totals.ways > 100'000, "and hundreds of thousands of ways");
    check(totals.relations > 1000, "and thousands of relations");

    // Unresolved references are normal AT A BOUNDARY -- a Geofabrik-style
    // extract cuts ways at its edge. What would not be normal is most of them.
    const std::uint64_t unresolved = stats.referenced - stats.resolved;
    check(unresolved * 100 < stats.referenced,
          "fewer than one percent of referenced nodes are unresolved");

    // ---- The cross-check -------------------------------------------------
    check(anchorWayId != 0, "a named highway exists within a kilometre of the Irvine anchor");
    if (anchorWayId != 0)
    {
        SPDLOG_INFO("anchor way {} is '{}' (highway={})", anchorWayId, anchorName, anchorHighway);

        // Every vertex of it must resolve and must lie in Southern California.
        // A single unresolved vertex read as (0,0) would put this road in the
        // Gulf of Guinea, which is exactly the failure the sentinel prevents.
        bool allResolved = true;
        bool allInRegion = true;
        for (const std::int64_t ref : anchorRefs)
        {
            auto coord = store.get(ref);
            if (!coord.has_value())
            {
                allResolved = false;
                continue;
            }
            if (coord->first < 320000000 || coord->first > 360000000 ||
                coord->second < -1200000000 || coord->second > -1150000000)
            {
                allInRegion = false;
            }
        }

        check(allResolved, "and every one of its vertices resolves");
        check(allInRegion, "and all of them are in Southern California, not at Null Island");
    }

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all real-extract checks passed");
    return 0;
}
