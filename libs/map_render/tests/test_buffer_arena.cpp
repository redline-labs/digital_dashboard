// SPDX-License-Identifier: GPL-3.0-or-later
//
// The suballocator the renderer places tile geometry with.
//
// It is tested apart from the renderer because its failure modes are silent
// there: an arena that hands out overlapping blocks draws one tile's triangles
// with another's coordinates, which looks like a tessellation bug, and one that
// leaks blocks reports fragmentation and forces a whole-buffer rebuild, which
// looks like no bug at all -- just a renderer that never got faster.

#include "map_render/buffer_arena.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

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

using map_render::BufferArena;

// Every live block, so overlap can be asserted rather than hoped for.
struct Block
{
    std::uint32_t offset;
    std::uint32_t count;
};

bool anyOverlap(const std::vector<Block>& blocks)
{
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        for (std::size_t j = i + 1; j < blocks.size(); ++j)
        {
            const std::uint32_t aEnd = blocks[i].offset + blocks[i].count;
            const std::uint32_t bEnd = blocks[j].offset + blocks[j].count;
            if (blocks[i].offset < bEnd && blocks[j].offset < aEnd)
            {
                return true;
            }
        }
    }
    return false;
}

void test_blocks_never_overlap_and_stay_inside_the_arena()
{
    BufferArena arena;
    arena.reset(1000);

    std::vector<Block> live;
    for (const std::uint32_t size : { 100U, 250U, 50U, 300U, 200U })
    {
        const std::uint32_t offset = arena.allocate(size);
        check(offset != BufferArena::kNoBlock, "a block that fits is allocated");
        check(offset + size <= 1000, "and lands inside the arena");
        live.push_back(Block { offset, size });
    }
    check(!anyOverlap(live), "no two live blocks overlap");
    check(arena.used() == 900, "used() is the sum of the live blocks, got " +
                                   std::to_string(arena.used()));
}

void test_a_full_arena_reports_failure_rather_than_overcommitting()
{
    BufferArena arena;
    arena.reset(100);
    check(arena.allocate(100) != BufferArena::kNoBlock, "a block exactly the size of the arena fits");
    check(arena.allocate(1) == BufferArena::kNoBlock, "and one more element does not");
    check(arena.allocate(0) == BufferArena::kNoBlock, "an empty request is not a valid block");
}

void test_freed_neighbours_merge_back_into_one_run()
{
    // The whole reason release() is more than a push_back. A map that pans one
    // tile at a time frees and allocates blocks of very similar size forever;
    // without a merge the free list would gain an entry per tile that ever left
    // the screen, and an arena that is entirely free would still refuse a block
    // larger than the last one released.
    BufferArena arena;
    arena.reset(300);
    const std::uint32_t a = arena.allocate(100);
    const std::uint32_t b = arena.allocate(100);
    const std::uint32_t c = arena.allocate(100);

    arena.release(a, 100);
    arena.release(c, 100);
    check(arena.freeBlocks() == 2, "two blocks with a live one between them stay separate");
    check(arena.allocate(300) == BufferArena::kNoBlock, "and cannot serve the whole arena");

    arena.release(b, 100);
    check(arena.freeBlocks() == 1, "freeing the middle merges all three, got " +
                                       std::to_string(arena.freeBlocks()));
    check(arena.used() == 0, "and nothing is left in use");
    check(arena.allocate(300) == 0, "so the whole arena is available again in one run");
}

void test_a_fragmented_arena_says_no_rather_than_splitting_a_block()
{
    // The report the renderer compacts on: there is room, just not in one
    // piece. Answering with a block anyway -- or with two -- would put a tile's
    // vertices in two places, and drawIndexed() has exactly one base vertex.
    BufferArena arena;
    arena.reset(300);
    const std::uint32_t a = arena.allocate(100);
    (void)arena.allocate(100);
    const std::uint32_t c = arena.allocate(100);
    arena.release(a, 100);
    arena.release(c, 100);

    check(arena.used() == 100, "a third of the arena is live");
    check(arena.allocate(150) == BufferArena::kNoBlock,
          "and a block larger than either hole is refused even though 200 is free");
    check(arena.allocate(100) != BufferArena::kNoBlock, "while one that fits a hole is served");
}

void test_reset_forgets_everything()
{
    BufferArena arena;
    arena.reset(100);
    (void)arena.allocate(100);
    arena.reset(100);
    check(arena.used() == 0, "reset drops the live blocks");
    check(arena.allocate(100) == 0, "and hands the arena back out from the start");
}

void test_an_empty_arena_serves_nothing()
{
    BufferArena arena;
    check(arena.allocate(1) == BufferArena::kNoBlock, "an arena that was never reset is empty");
    arena.reset(0);
    check(arena.allocate(1) == BufferArena::kNoBlock, "and so is one reset to zero");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_blocks_never_overlap_and_stay_inside_the_arena();
    test_a_full_arena_reports_failure_rather_than_overcommitting();
    test_freed_neighbours_merge_back_into_one_run();
    test_a_fragmented_arena_says_no_rather_than_splitting_a_block();
    test_reset_forgets_everything();
    test_an_empty_arena_serves_nothing();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all buffer arena checks passed");
    return 0;
}
