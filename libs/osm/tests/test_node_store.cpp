// SPDX-License-Identifier: GPL-3.0-or-later
//
// bitset -> rank -> dense array, and the ordering check that guards it.
//
// The failure this exists to prevent is the quiet one: a way referencing a node
// that is not in the extract. Pass A allocates a slot for it because the way
// asked, pass B never fills it, and a store that returned a default would put
// that vertex at lat 0 / lon 0 -- the Gulf of Guinea. On the map that is a line
// across the world; in the graph it is an 8000 km edge with a four-minute cost
// that a router will choose for every long trip. Both look like data, not bugs.

#include <cstdint>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

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

void test_referenced_nodes_round_trip()
{
    osm::NodeStore store;
    store.markReferenced(1000);
    store.markReferenced(2000);
    store.markReferenced(1500);

    check(store.finalise().has_value(), "the store finalises");

    store.set(1000, 100, 200);
    store.set(1500, 300, 400);
    store.set(2000, 500, 600);
    // A node nobody referenced: silently ignored, because most of a file's
    // nodes are not part of any way.
    store.set(9999, 700, 800);

    auto first = store.get(1000);
    check(first.has_value() && first->first == 100 && first->second == 200,
          "the first node reads back");

    auto middle = store.get(1500);
    check(middle.has_value() && middle->first == 300,
          "and so does one inserted out of order");

    auto last = store.get(2000);
    check(last.has_value() && last->second == 600, "and the last");

    check(!store.get(9999).has_value(), "an unreferenced node is absent");

    const auto stats = store.stats();
    check(stats.referenced == 3, "three nodes were referenced");
    check(stats.resolved == 3, "and all three resolved");
    check(stats.maxId == 2000, "with the right maximum id");
}

void test_an_unresolved_reference_is_absent_rather_than_null_island()
{
    // THE case. A way at the edge of an extract references a node outside it.
    osm::NodeStore store;
    store.markReferenced(1000);
    store.markReferenced(2000);
    check(store.finalise().has_value(), "the store finalises");

    store.set(1000, 336865966, -1178557874);
    // 2000 is never set -- it is outside the extract.

    check(store.get(1000).has_value(), "the node that is in the extract resolves");

    auto missing = store.get(2000);
    check(!missing.has_value(),
          "and the one that is not is ABSENT rather than (0,0)");

    const auto stats = store.stats();
    check(stats.referenced == 2, "both were referenced");
    check(stats.resolved == 1, "and the gap is visible in the stats");
}

void test_id_zero_and_the_first_word()
{
    // Rank over the first word has no superblock to accumulate from, and id 0
    // has no bits below it -- both are off-by-one country.
    osm::NodeStore store;
    store.markReferenced(0);
    store.markReferenced(1);
    store.markReferenced(63);
    check(store.finalise().has_value(), "the store finalises");

    store.set(0, 10, 11);
    store.set(1, 20, 21);
    store.set(63, 30, 31);

    auto zero = store.get(0);
    check(zero.has_value() && zero->first == 10, "id 0 reads back");
    auto one = store.get(1);
    check(one.has_value() && one->first == 20, "id 1 reads back");
    auto edge = store.get(63);
    check(edge.has_value() && edge->first == 30, "and the last bit of the first word");
}

void test_ids_across_superblock_boundaries()
{
    // The rank index is cumulative per 512-bit superblock; a query has to add
    // the whole words before its own within that superblock. An error here
    // shifts every coordinate after the first superblock onto a neighbour's --
    // which is a real position, in the wrong place, for the rest of the file.
    osm::NodeStore store;

    std::vector<std::int64_t> ids;
    for (std::int64_t id = 0; id < 5000; id += 37)
    {
        ids.push_back(id);
        store.markReferenced(id);
    }
    check(store.finalise().has_value(), "the store finalises");

    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        store.set(ids[i], static_cast<osm::Coord>(i), static_cast<osm::Coord>(i * 2));
    }

    bool allCorrect = true;
    for (std::size_t i = 0; i < ids.size(); ++i)
    {
        auto value = store.get(ids[i]);
        if (!value || value->first != static_cast<osm::Coord>(i) ||
            value->second != static_cast<osm::Coord>(i * 2))
        {
            allCorrect = false;
            break;
        }
    }
    check(allCorrect, "every id across many superblocks reads back its own coordinate");

    check(store.stats().referenced == ids.size(), "with the right count");
}

void test_a_sparse_id_space_costs_only_its_bitset()
{
    // The whole point of the design: ids scattered across a huge range cost a
    // bitset over the range and a coordinate array over the COUNT, not an array
    // over the range.
    osm::NodeStore store;
    store.markReferenced(1);
    store.markReferenced(1'000'000'000);
    check(store.finalise().has_value(), "the store finalises");

    store.set(1, 5, 6);
    store.set(1'000'000'000, 7, 8);

    auto low = store.get(1);
    auto high = store.get(1'000'000'000);
    check(low.has_value() && low->first == 5, "the low id reads");
    check(high.has_value() && high->first == 7, "and the billion-and-something one");

    const auto stats = store.stats();
    check(stats.referenced == 2, "two nodes referenced");
    // The bitset covers the range (~125 MB for 1e9 bits); the coordinates cost
    // 16 bytes. A dense array over the id space would have been 8 GB.
    check(stats.bytes < 200u * 1024 * 1024,
          "and the whole store is the bitset, not an array over the id space");
}

void test_repeated_marking_counts_once()
{
    // A node shared by twenty ways is marked twenty times; the count must not
    // be twenty, or finalise() allocates the wrong array and every rank after
    // it is wrong.
    osm::NodeStore store;
    for (int i = 0; i < 20; ++i)
    {
        store.markReferenced(500);
    }
    check(store.finalise().has_value(), "the store finalises");
    check(store.stats().referenced == 1, "a node referenced twenty times counts once");

    store.set(500, 1, 2);
    check(store.get(500).has_value(), "and still resolves");
}

void test_ordering_is_verified_while_streaming()
{
    // Sortedness is what makes the two-pass build possible, and the header flag
    // that claims it is advisory and often absent -- so it is checked here
    // instead, at the byte where it breaks.
    osm::OrderCheck ok;
    check(ok.node(1, 0).has_value(), "nodes in order are accepted");
    check(ok.node(5, 10).has_value(), "and continue to be");
    check(ok.way(100, 20).has_value(), "ways after nodes are accepted");
    check(ok.way(101, 30).has_value(), "in their own ascending order");
    check(ok.relation(7, 40).has_value(), "relations after ways are accepted");
    check(ok.relation(9, 50).has_value(), "in theirs");
}

void test_a_node_after_the_ways_is_refused()
{
    // The specific violation the two-pass build cannot survive: pass A has
    // already streamed past the nodes when it reads the ways, so a node that
    // arrives afterwards is one whose coordinate pass B will never see.
    osm::OrderCheck ok;
    check(ok.node(1, 0).has_value(), "a node is accepted");
    check(ok.way(100, 10).has_value(), "then a way");

    auto late = ok.node(2, 20);
    check(!late.has_value(), "a node AFTER the ways is refused");
    if (!late)
    {
        check(late.error().kind == osm::Error::Kind::OutOfOrder, "as OutOfOrder");
        check(late.error().offset == 20, "naming the byte where order broke");
    }
}

void test_descending_ids_within_a_type_are_refused()
{
    osm::OrderCheck ok;
    check(ok.way(100, 0).has_value(), "a way is accepted");
    auto backwards = ok.way(50, 10);
    check(!backwards.has_value(), "a way with a lower id after it is refused");
    if (!backwards)
    {
        check(backwards.error().kind == osm::Error::Kind::OutOfOrder, "as OutOfOrder");
    }
}

void test_ids_restart_between_types()
{
    // Node ids and way ids are separate spaces; way 1 after node 500 is normal.
    osm::OrderCheck ok;
    check(ok.node(500, 0).has_value(), "a high node id");
    check(ok.way(1, 10).has_value(), "then way 1 -- a different id space, not a regression");
    check(ok.relation(1, 20).has_value(), "and relation 1 after it");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_referenced_nodes_round_trip();
    test_an_unresolved_reference_is_absent_rather_than_null_island();
    test_id_zero_and_the_first_word();
    test_ids_across_superblock_boundaries();
    test_a_sparse_id_space_costs_only_its_bitset();
    test_repeated_marking_counts_once();

    test_ordering_is_verified_while_streaming();
    test_a_node_after_the_ways_is_refused();
    test_descending_ids_within_a_type_are_refused();
    test_ids_restart_between_types();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all node store checks passed");
    return 0;
}
