// SPDX-License-Identifier: GPL-3.0-or-later
//
// A suballocator over one GPU buffer, counted in ELEMENTS rather than bytes so
// the vertex and index buffers can each own one without either having to know
// its stride.
//
// It exists so a tile already on the GPU stays where it is when the visible set
// changes. The renderer used to flatten every resident tile into one array and
// re-upload the whole thing whenever any tile arrived or left; adding one tile
// therefore cost a copy and an upload of everything on screen, which at a
// pitched camera is tens of megabytes several times a second because the far
// field crosses an LOD boundary constantly.
//
// Deliberately a plain first-fit free list with coalescing rather than a buddy
// or slab allocator. The live block count is the number of tiles on screen --
// a couple of hundred at the very worst -- so a linear scan is cheaper than any
// structure that would avoid one, and it stays small enough to reason about.
// Fragmentation is not the allocator's problem: allocate() reports failure and
// the caller compacts, which is exactly the whole-buffer rebuild that used to
// happen every frame anyway.
#ifndef MAP_BUFFER_ARENA_H
#define MAP_BUFFER_ARENA_H

#include <cstdint>
#include <limits>
#include <map>

namespace map_render
{

class BufferArena
{
  public:
    // Not zero: zero is a perfectly good offset for the first block, so a
    // failure has to be a value no block can have.
    static constexpr std::uint32_t kNoBlock = std::numeric_limits<std::uint32_t>::max();

    // Drops every outstanding block. The caller is re-placing everything, which
    // is the only way the arena ever forgets.
    void reset(std::uint32_t capacity)
    {
        mFree.clear();
        mCapacity = capacity;
        mUsed = 0;
        if (capacity > 0)
        {
            mFree.emplace(0U, capacity);
        }
    }

    std::uint32_t capacity() const { return mCapacity; }
    std::uint32_t used() const { return mUsed; }
    std::size_t freeBlocks() const { return mFree.size(); }

    // kNoBlock when no single free run is long enough -- which is a
    // fragmentation report, not an out-of-memory one: used() may be far below
    // capacity() and the answer still be no.
    std::uint32_t allocate(std::uint32_t count)
    {
        if (count == 0)
        {
            return kNoBlock;
        }
        for (auto it = mFree.begin(); it != mFree.end(); ++it)
        {
            if (it->second < count)
            {
                continue;
            }
            const std::uint32_t offset = it->first;
            const std::uint32_t remainder = it->second - count;
            mFree.erase(it);
            if (remainder > 0)
            {
                mFree.emplace(offset + count, remainder);
            }
            mUsed += count;
            return offset;
        }
        return kNoBlock;
    }

    // Coalescing is what keeps this from degenerating: a map that pans one tile
    // at a time frees and allocates blocks of very similar size forever, and
    // without a merge the free list would grow by one entry per tile that ever
    // left the screen.
    void release(std::uint32_t offset, std::uint32_t count)
    {
        if (count == 0)
        {
            return;
        }
        mUsed -= count;

        auto next = mFree.lower_bound(offset);
        if (next != mFree.end() && next->first == offset + count)
        {
            count += next->second;
            next = mFree.erase(next);
        }
        if (next != mFree.begin())
        {
            const auto previous = std::prev(next);
            if (previous->first + previous->second == offset)
            {
                previous->second += count;
                return;
            }
        }
        mFree.emplace(offset, count);
    }

  private:
    // Offset -> length, always coalesced, so adjacent entries are never
    // adjacent runs.
    std::map<std::uint32_t, std::uint32_t> mFree;
    std::uint32_t mCapacity { 0 };
    std::uint32_t mUsed { 0 };
};

} // namespace map_render

#endif // MAP_BUFFER_ARENA_H
