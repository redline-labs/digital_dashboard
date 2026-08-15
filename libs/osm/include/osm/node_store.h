// SPDX-License-Identifier: GPL-3.0-or-later
//
// Node id -> coordinate, at continental scale.
//
// A way holds node ids; building its geometry needs their coordinates. Both
// obvious answers fail:
//
//   - A hash map of id -> coord. Fine for a city, and for a continent it is
//     ~2 billion entries of pointer-chasing.
//   - A dense array indexed by the raw OSM node id. Ids are assigned
//     chronologically and the space is now around 1.3e10, so even a small
//     extract's ids are scattered across the whole range: ~100 GB of array
//     REGARDLESS of how small the extract is.
//
// What this does instead is bitset -> rank -> dense array:
//
//   1. Pass A marks a bit per node id referenced by a way or relation. Over the
//      full id space that is ~1.6 GB, which is nothing.
//   2. A rank index over the bitset turns an id into a compact index in O(1).
//   3. Pass B fills an array of 8 bytes per REFERENCED node, indexed by rank.
//
// Continental US is ~2.2e9 referenced nodes: ~17.6 GB of coordinates plus the
// bitset and rank, so ~20 GB. THAT MUST BE RESIDENT. The lookups in pass B are
// random across the whole array, and ~2.5e9 of them against an SSD-backed
// mmap at even 20 us each is days rather than the ~12 minutes they take in RAM.
// This is a workstation requirement, not a code change, and it is written here
// because the alternative is someone concluding the design is wrong after
// running it on a laptop.
//
// The escape hatch, if planet scale ever arrives: spill (nodeId, wayId, ordinal)
// triples in pass A, external-sort by nodeId, and turn the random reads into a
// sequential merge. Nothing here needs it, but the way-reference consumer in
// map_build is a callback for exactly that reason -- it could be redirected to a
// spill file without touching this.
#ifndef OSM_NODE_STORE_H
#define OSM_NODE_STORE_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "osm/entity.h"
#include "osm/error.h"

namespace osm
{

class NodeStore
{
  public:
    // Bits per superblock in the rank index. 512 is the usual choice: the
    // cumulative counts cost 1/64th of the bitset and a query scans at most
    // eight words.
    static constexpr std::uint64_t kSuperblockBits = 512;

    // PASS A. Mark an id as referenced. Ids may arrive in any order, and the
    // bitset grows to fit -- SIZED FROM THE FILE, never from a constant. The id
    // space grows by ~0.5-1e9 a year, and a hardcoded ceiling silently truncates
    // the top of it, which presents as the newest edits missing.
    void markReferenced(std::int64_t id);

    // Build the rank index and allocate the coordinate array. Call once,
    // between the passes. Every coordinate starts as kNoCoord.
    Result<void> finalise();

    // PASS B. Record a coordinate. Ids that were never marked are ignored --
    // most of a file's nodes are not part of any way.
    void set(std::int64_t id, Coord lat, Coord lon);

    bool finalised() const { return mFinalised; }
    bool referenced(std::int64_t id) const;

    // The coordinate, or nothing if this id was never referenced or never seen
    // in pass B.
    //
    // ABSENCE IS THE POINT. A way referencing a node outside the extract is
    // routine at a boundary, and pass A will have allocated a slot for it that
    // pass B never fills. Returning a default would put that vertex at lat 0 /
    // lon 0 -- a real place in the Gulf of Guinea -- and the result is a
    // motorway drawn across the world and an 8000 km routing edge with a
    // four-minute cost that a router will use for every long trip. Callers
    // must drop the whole way; see map_build.
    std::optional<std::pair<Coord, Coord>> get(std::int64_t id) const;

    struct Stats
    {
        // Bits set in pass A: nodes some way or relation asked for.
        std::uint64_t referenced { 0 };
        // Of those, how many pass B actually filled.
        std::uint64_t resolved { 0 };
        std::int64_t maxId { 0 };
        // Bytes held by the bitset, rank index and coordinate array.
        std::uint64_t bytes { 0 };
    };

    Stats stats() const;

  private:
    std::uint64_t rank(std::uint64_t id) const;

    std::vector<std::uint64_t> mBits;
    // Cumulative set bits before each superblock. Empty until finalise().
    std::vector<std::uint64_t> mRank;
    // Two Coords per referenced node, indexed by rank.
    std::vector<Coord> mCoords;

    std::int64_t mMaxId { -1 };
    std::uint64_t mReferenced { 0 };
    std::uint64_t mResolved { 0 };
    bool mFinalised { false };
};

// Verifies (type, id) ordering while streaming.
//
// The two-pass build relies on nodes preceding ways preceding relations.
// HeaderBlock.optional_features may say `Sort.Type_then_ID`, but it is ADVISORY,
// nothing validates it, and plenty of perfectly sorted extracts omit it -- so
// trusting the flag rejects good files and believing its absence means unsorted
// rejects most of them.
//
// Checking while reading costs one comparison per entity and turns an unsorted
// file into a hard error AT THE BYTE WHERE ORDER BREAKS, rather than into a
// graph with holes discovered forty minutes later.
class OrderCheck
{
  public:
    Result<void> node(std::int64_t id, std::size_t offset);
    Result<void> way(std::int64_t id, std::size_t offset);
    Result<void> relation(std::int64_t id, std::size_t offset);

  private:
    enum class Phase : std::uint8_t
    {
        Nodes,
        Ways,
        Relations,
    };

    Result<void> advance(Phase phase, std::int64_t id, std::size_t offset, const char* what);

    Phase mPhase { Phase::Nodes };
    std::int64_t mLastId { std::numeric_limits<std::int64_t>::min() };
};

} // namespace osm

#endif // OSM_NODE_STORE_H
