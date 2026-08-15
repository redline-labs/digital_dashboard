// SPDX-License-Identifier: GPL-3.0-or-later
#include "osm/node_store.h"

#include <algorithm>
#include <bit>
#include <string>

namespace osm
{
namespace
{

// std::size_t rather than uint64_t so the arithmetic below stays in one type;
// on a 64-bit platform they are the same width but not the same type.
constexpr std::size_t kWordsPerSuperblock =
    static_cast<std::size_t>(NodeStore::kSuperblockBits / 64);

} // namespace

void NodeStore::markReferenced(std::int64_t id)
{
    if (id < 0)
    {
        // Negative ids exist only in editor scratch files, never in a published
        // extract. Ignored rather than refused: one is not worth failing a
        // continental build over, and the way that references it will be
        // dropped by the missing-coordinate path anyway.
        return;
    }

    const auto uid = static_cast<std::uint64_t>(id);
    const std::size_t word = static_cast<std::size_t>(uid / 64);

    if (word >= mBits.size())
    {
        // Grown to fit what the file actually contains. Geometric growth so a
        // file whose ids climb steadily does not re-allocate per block.
        const std::size_t wanted = word + 1;
        mBits.resize(std::max(wanted, mBits.size() + mBits.size() / 2 + 1), 0);
    }

    const std::uint64_t mask = std::uint64_t { 1 } << (uid % 64);
    if ((mBits[word] & mask) == 0)
    {
        mBits[word] |= mask;
        ++mReferenced;
    }

    mMaxId = std::max(mMaxId, id);
}

Result<void> NodeStore::finalise()
{
    if (mFinalised)
    {
        return malformed("node store finalised twice");
    }

    // Cumulative set bits before each superblock.
    const std::size_t superblocks = (mBits.size() + kWordsPerSuperblock - 1) / kWordsPerSuperblock;
    mRank.assign(superblocks + 1, 0);

    std::uint64_t running = 0;
    for (std::size_t s = 0; s < superblocks; ++s)
    {
        mRank[s] = running;
        const std::size_t begin = s * kWordsPerSuperblock;
        const std::size_t end = std::min(begin + kWordsPerSuperblock, mBits.size());
        for (std::size_t w = begin; w < end; ++w)
        {
            running += static_cast<std::uint64_t>(std::popcount(mBits[w]));
        }
    }
    mRank[superblocks] = running;

    if (running != mReferenced)
    {
        return malformed("rank index counted " + std::to_string(running) + " referenced nodes, " +
                         "the bitset says " + std::to_string(mReferenced));
    }

    // Two Coords per node, both kNoCoord. Filling with a sentinel rather than
    // zero is what makes an unresolved reference a checkable condition instead
    // of a plausible position off the coast of Africa.
    mCoords.assign(static_cast<std::size_t>(mReferenced) * 2, kNoCoord);
    mFinalised = true;
    return {};
}

std::uint64_t NodeStore::rank(std::uint64_t id) const
{
    const std::size_t word = static_cast<std::size_t>(id / 64);
    const std::size_t superblock = word / kWordsPerSuperblock;

    std::uint64_t count = mRank[superblock];
    for (std::size_t w = superblock * kWordsPerSuperblock; w < word; ++w)
    {
        count += static_cast<std::uint64_t>(std::popcount(mBits[w]));
    }

    // Bits below this one in its own word.
    const std::uint64_t below = mBits[word] & ((std::uint64_t { 1 } << (id % 64)) - 1);
    return count + static_cast<std::uint64_t>(std::popcount(below));
}

bool NodeStore::referenced(std::int64_t id) const
{
    if (id < 0)
    {
        return false;
    }
    const auto uid = static_cast<std::uint64_t>(id);
    const std::size_t word = static_cast<std::size_t>(uid / 64);
    if (word >= mBits.size())
    {
        return false;
    }
    return (mBits[word] & (std::uint64_t { 1 } << (uid % 64))) != 0;
}

void NodeStore::set(std::int64_t id, Coord lat, Coord lon)
{
    if (!mFinalised || !referenced(id))
    {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(rank(static_cast<std::uint64_t>(id)));
    if (mCoords[index * 2] == kNoCoord)
    {
        ++mResolved;
    }
    mCoords[index * 2] = lat;
    mCoords[index * 2 + 1] = lon;
}

std::optional<std::pair<Coord, Coord>> NodeStore::get(std::int64_t id) const
{
    if (!mFinalised || !referenced(id))
    {
        return std::nullopt;
    }

    const std::size_t index = static_cast<std::size_t>(rank(static_cast<std::uint64_t>(id)));
    const Coord lat = mCoords[index * 2];
    const Coord lon = mCoords[index * 2 + 1];
    if (!hasCoord(lat) || !hasCoord(lon))
    {
        return std::nullopt;
    }
    return std::pair<Coord, Coord> { lat, lon };
}

NodeStore::Stats NodeStore::stats() const
{
    Stats out;
    out.referenced = mReferenced;
    out.resolved = mResolved;
    out.maxId = mMaxId;
    out.bytes = static_cast<std::uint64_t>(mBits.size()) * 8 +
                static_cast<std::uint64_t>(mRank.size()) * 8 +
                static_cast<std::uint64_t>(mCoords.size()) * sizeof(Coord);
    return out;
}

Result<void> OrderCheck::advance(Phase phase, std::int64_t id, std::size_t offset, const char* what)
{
    if (phase < mPhase)
    {
        return out_of_order(std::string("a ") + what + " after the " +
                                (mPhase == Phase::Relations ? "relations" : "ways") +
                                " began; the two-pass node store needs nodes, then ways, then "
                                "relations",
                            offset);
    }

    if (phase > mPhase)
    {
        mPhase = phase;
        mLastId = std::numeric_limits<std::int64_t>::min();
    }

    if (id < mLastId)
    {
        return out_of_order(std::string(what) + " id " + std::to_string(id) + " after " +
                                std::to_string(mLastId),
                            offset);
    }

    mLastId = id;
    return {};
}

Result<void> OrderCheck::node(std::int64_t id, std::size_t offset)
{
    return advance(Phase::Nodes, id, offset, "node");
}

Result<void> OrderCheck::way(std::int64_t id, std::size_t offset)
{
    return advance(Phase::Ways, id, offset, "way");
}

Result<void> OrderCheck::relation(std::int64_t id, std::size_t offset)
{
    return advance(Phase::Relations, id, offset, "relation");
}

} // namespace osm
