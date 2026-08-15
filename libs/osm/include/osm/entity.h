// SPDX-License-Identifier: GPL-3.0-or-later
//
// What comes out of a PBF block.
//
// Entities are FLAT AND INDEXED rather than self-contained: a Way holds a range
// into the block's shared ref array, not its own vector. A continental extract
// is around 400 million ways averaging ten refs each, and a vector per way is
// 400 million allocations plus 24 bytes of header on every one of them. The
// ranges cost nothing and the spans are handed out on demand.
//
// Every id here is the OSM id, unchanged. Nothing in this library renumbers.
#ifndef OSM_ENTITY_H
#define OSM_ENTITY_H

#include <cstdint>
#include <limits>

namespace osm
{

// A coordinate in 1e-7 degrees, the unit used everywhere in this tree.
//
// int32 holds +/-2.147e9 and the widest legal value is 1.8e9 (180 degrees), so
// the range fits with room. PBF carries nanodegrees, which is a hundred times
// finer than OSM data is ever accurate to; the conversion happens once, in
// block.cpp.
using Coord = std::int32_t;

// A coordinate that was never written.
//
// NOT zero: zero is lat 0 / lon 0, which is a real place in the Gulf of Guinea,
// and a road drawn to it stretches across the world while a routing edge to it
// is 8000 km long with a four-minute cost that a router will happily use for
// every trip. INT32_MIN is not a reachable coordinate, so reading one is a
// checkable condition rather than a plausible answer. See node_store.h.
inline constexpr Coord kNoCoord = std::numeric_limits<std::int32_t>::min();

constexpr bool hasCoord(Coord c)
{
    return c != kNoCoord;
}

enum class MemberType : std::uint8_t
{
    Node,
    Way,
    Relation,
};

// One (key, value) pair, as indices into the block's string table.
struct Tag
{
    std::uint32_t key { 0 };
    std::uint32_t value { 0 };
};

struct Member
{
    std::int64_t ref { 0 };
    std::uint32_t roleIndex { 0 };
    MemberType type { MemberType::Node };
};

struct Node
{
    std::int64_t id { 0 };
    Coord lat { kNoCoord };
    Coord lon { kNoCoord };
    std::uint32_t tagBegin { 0 };
    std::uint32_t tagCount { 0 };
};

struct Way
{
    std::int64_t id { 0 };
    std::uint32_t refBegin { 0 };
    std::uint32_t refCount { 0 };
    std::uint32_t tagBegin { 0 };
    std::uint32_t tagCount { 0 };
};

struct Relation
{
    std::int64_t id { 0 };
    std::uint32_t memberBegin { 0 };
    std::uint32_t memberCount { 0 };
    std::uint32_t tagBegin { 0 };
    std::uint32_t tagCount { 0 };
};

} // namespace osm

#endif // OSM_ENTITY_H
