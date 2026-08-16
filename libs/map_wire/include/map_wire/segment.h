// SPDX-License-Identifier: GPL-3.0-or-later
//
// A road-graph segment, on the wire.
//
// The one place a `road_graph::SegmentRecord` becomes capnp. Both
// nodes/map_server (nearest, route) and nodes/map_match (the horizon) answer
// "what road is this", and each had its own copy of the mapping -- an identical
// 30-line wireClassOf() in two files, plus the same four speed setters written
// out three times.
//
// Two copies of a vocabulary drift, and the drift is silent: map_common.capnp
// says as much about why MapRoadClass exists at all. A matcher reporting
// `minor` and a horizon reporting `secondary` for the same road look like two
// different roads to anything that switches on either.
#ifndef MAP_WIRE_SEGMENT_H
#define MAP_WIRE_SEGMENT_H

#include "map_rules/classification.h"
#include "road_graph/format.h"

#include "map_common.capnp.h"

#include <capnp/message.h>

namespace map_wire
{

// map_rules::RouteClass -> the wire vocabulary.
//
// Written out rather than cast, even though the two enumerations are parallel
// today. A cast would keep compiling after someone inserts a value into either
// one, and the result would be every road on the dash reporting the class of
// its neighbour. Spelled out, -Wswitch-enum makes that insertion a build
// failure here -- which is the entire point, and the reason this is worth a
// function rather than a static_cast at each call site.
::MapRoadClass classOf(map_rules::RouteClass value);

// The same, from the raw byte a SegmentRecord stores.
::MapRoadClass classOf(std::uint8_t routeClass);

// map_rules::SpeedSource -> the wire vocabulary, for the same reason.
//
// This one WAS a static_cast at both call sites, which is precisely the bug
// classOf() was written out to avoid: inserting a value into either enum would
// have silently relabelled every speed limit's provenance, and "sign" versus
// "guessed from the road class" is the difference between a limit a dash may
// show a driver and one it must not.
::MapSpeedSource speedSourceOf(map_rules::SpeedSource value);
::MapSpeedSource speedSourceOf(std::uint8_t postedSource);

// Fill a MapSpeed from a segment.
//
// `hasPosted` comes from the flag, never from postedKph being non-zero: zero
// is a legal limit in exactly the places (a barrier, a gate) where getting it
// wrong matters. See map_common.capnp.
void fillSpeed(::MapSpeed::Builder speed, const road_graph::SegmentRecord& segment);

} // namespace map_wire

#endif // MAP_WIRE_SEGMENT_H
