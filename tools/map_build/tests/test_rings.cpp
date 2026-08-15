// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stitching multipolygon members into rings.
//
// Every case here is one that real OSM data contains and that a naive
// implementation gets wrong quietly:
//
//   - members arrive in ARBITRARY ORDER, so a loop that expects them sorted
//     joins the first two it sees and abandons the rest;
//   - members point in ARBITRARY DIRECTIONS, so half of them have to be
//     reversed, and reversing the wrong one puts a spike in the shoreline;
//   - the joint node appears in both arcs and must be written ONCE, or every
//     junction gets a zero-length edge;
//   - a relation has SEVERAL rings -- an outer coast plus islands -- and they
//     have to be separated by role, not by order;
//   - an arc set that does not close must be DROPPED. This is the one that
//     matters: a forced-shut ring paints its missing piece as water over land.

#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "map_build/rings.h"

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

// An arc from node `a` to node `b` through the given lat/lon pairs.
map_build::RingArc arc(std::int64_t a, std::int64_t b, std::vector<osm::Coord> geometry,
                       bool inner = false)
{
    map_build::RingArc out;
    out.firstNode = a;
    out.lastNode = b;
    out.geometry = std::move(geometry);
    out.inner = inner;
    return out;
}

bool hasPoint(const std::vector<osm::Coord>& ring, osm::Coord lat, osm::Coord lon)
{
    for (std::size_t i = 0; i + 1 < ring.size(); i += 2)
    {
        if (ring[i] == lat && ring[i + 1] == lon)
        {
            return true;
        }
    }
    return false;
}

void test_a_square_from_arcs_in_any_order_and_direction()
{
    // The square (0,0) (0,10) (10,10) (10,0), delivered as four arcs: shuffled,
    // and two of them pointing backwards. This is what a shoreline looks like.
    std::vector<map_build::RingArc> arcs {
        // node 3 -> 4, the top edge, forwards
        arc(3, 4, { 10, 10, 10, 0 }),
        // node 2 -> 1, the left edge, BACKWARDS
        arc(2, 1, { 0, 10, 0, 0 }),
        // node 4 -> 1, the bottom edge, backwards
        arc(4, 1, { 10, 0, 0, 0 }),
        // node 2 -> 3, the right edge, forwards
        arc(2, 3, { 0, 10, 10, 10 }),
    };

    auto rings = map_build::assembleRings(arcs);
    check(rings.outer.size() == 1, "four arcs make ONE outer ring");
    check(rings.inner.empty(), "and no inner rings");
    check(rings.abandonedArcs == 0, "with nothing left over");
    if (rings.outer.size() != 1)
    {
        return;
    }

    const auto& ring = rings.outer[0];
    // Four corners, each ONCE. Five would mean the closing point was repeated;
    // more would mean a joint node was written twice, which puts a zero-length
    // edge at every junction.
    check(ring.size() == 8, "with each corner exactly once (" +
                                std::to_string(ring.size() / 2) + " points)");
    check(hasPoint(ring, 0, 0), "the corner at 0,0");
    check(hasPoint(ring, 0, 10), "the corner at 0,10");
    check(hasPoint(ring, 10, 10), "the corner at 10,10");
    check(hasPoint(ring, 10, 0), "the corner at 10,0");
}

void test_an_island_is_its_own_ring()
{
    // A lake with an island: one outer ring, one inner. They must come back
    // separated by ROLE, because the two are the same shape to a stitcher and
    // only the relation says which is the hole.
    std::vector<map_build::RingArc> arcs {
        arc(1, 2, { 0, 0, 0, 100 }),
        arc(2, 1, { 0, 100, 100, 0 }),
        arc(10, 11, { 40, 40, 40, 60 }, true),
        arc(11, 10, { 40, 60, 60, 40 }, true),
    };

    auto rings = map_build::assembleRings(arcs);
    check(rings.outer.size() == 1, "the lake is one outer ring");
    check(rings.inner.size() == 1, "and the island is one INNER ring");
    check(rings.abandonedArcs == 0, "with nothing left over");
}

void test_an_unclosed_ring_is_dropped_not_forced_shut()
{
    // Three sides of a square. A stitcher that closes it anyway produces a
    // triangle of water lying over land -- which reads as a rendering fault,
    // not as missing data, and so never gets reported.
    std::vector<map_build::RingArc> arcs {
        arc(1, 2, { 0, 0, 0, 10 }),
        arc(2, 3, { 0, 10, 10, 10 }),
        arc(3, 4, { 10, 10, 10, 0 }),
    };

    auto rings = map_build::assembleRings(arcs);
    check(rings.outer.empty() && rings.inner.empty(),
          "an open chain of arcs yields NO ring at all");
    check(rings.abandonedArcs == 3, "and all three arcs are reported as abandoned");
}

void test_an_already_closed_way_is_a_ring_on_its_own()
{
    // The common case by count: a small pond mapped as one closed way, wrapped
    // in a relation only because it has an island.
    std::vector<map_build::RingArc> arcs {
        arc(1, 1, { 0, 0, 0, 10, 10, 10, 10, 0, 0, 0 }),
    };

    auto rings = map_build::assembleRings(arcs);
    check(rings.outer.size() == 1, "a closed way needs no stitching");
    if (rings.outer.size() == 1)
    {
        check(rings.outer[0].size() == 8,
              "and its repeated closing point is dropped, because ClosePath implies it");
    }
}

void test_two_separate_rings_both_survive()
{
    // A relation with two outer rings and nothing joining them -- two lakes
    // under one name. A stitcher that stops after the first ring loses the
    // second silently.
    std::vector<map_build::RingArc> arcs {
        arc(1, 2, { 0, 0, 0, 10 }),
        arc(2, 1, { 0, 10, 10, 0 }),
        arc(5, 6, { 100, 100, 100, 110 }),
        arc(6, 5, { 100, 110, 110, 100 }),
    };

    auto rings = map_build::assembleRings(arcs);
    check(rings.outer.size() == 2, "both rings are assembled");
    check(rings.abandonedArcs == 0, "with nothing left over");
}

void test_a_good_ring_survives_a_broken_one_beside_it()
{
    // Real relations are partly broken. One complete ring and one dangling arc
    // must yield the ring plus a count -- not nothing, and not a ring with the
    // stray arc spliced into it.
    std::vector<map_build::RingArc> arcs {
        arc(1, 2, { 0, 0, 0, 10 }),
        arc(2, 1, { 0, 10, 10, 0 }),
        arc(50, 51, { 500, 500, 500, 510 }),
    };

    auto rings = map_build::assembleRings(arcs);
    check(rings.outer.size() == 1, "the complete ring is kept");
    check(rings.abandonedArcs == 1, "and the dangling arc is counted, not spliced in");
    if (rings.outer.size() == 1)
    {
        check(!hasPoint(rings.outer[0], 500, 500), "the stray arc is nowhere in the ring");
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_square_from_arcs_in_any_order_and_direction();
    test_an_island_is_its_own_ring();
    test_an_unclosed_ring_is_dropped_not_forced_shut();
    test_an_already_closed_way_is_a_ring_on_its_own();
    test_two_separate_rings_both_survive();
    test_a_good_ring_survives_a_broken_one_beside_it();

    spdlog::set_level(spdlog::level::info);
    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all ring assembly checks passed");
    return 0;
}
