---
title: map_wire
parent: Libraries
---

# map_wire

## Overview

A road-graph segment on the wire, derived once for everyone who publishes one.
It is the same argument as [map_rules](map_rules.html) one layer down:
[map_server](../nodes/map_server.html) (`map/nearest`, `map/route`) and
`nodes/map_match` (the horizon) both answer "what road is this", and each had
its own copy of the enum mapping, an identical thirty-line `wireClassOf()` in
two files plus the same four speed setters written out three times.
`map_common.capnp` exists so the two nodes name a road class the same way; this
library exists so they derive it the same way.

There is no zenoh here, only the generated capnp types, so it stays testable
without a bus. It links `map_rules`, [road_graph](road_graph.html) and
`schemas` publicly, because those are the types it maps between.

## Public headers

| Header | Declares |
|---|---|
| `map_wire/segment.h` | `classOf()` from `map_rules::RouteClass` or the raw byte a `SegmentRecord` stores, `speedSourceOf()` likewise, `fillSpeed()` from a `road_graph::SegmentRecord` into a `MapSpeed::Builder`, and `osmWayIdOf()` for the schema's unsigned way id. |

## Using it

Link the `map_wire` target. Both nodes call it the same way when they fill a
candidate or a horizon profile; this is `nodes/map_server/services.cpp`.

```cpp
const road_graph::SegmentRecord& segment = graph.segments()[match.segment];
auto out = list[i];
out.setName(std::string(graph.nameOf(segment)));
out.setRef(std::string(graph.refOf(segment)));
out.setRoadClass(map_wire::classOf(segment.routeClass));
map_wire::fillSpeed(out.initSpeed(), segment);
```

## Behaviour worth knowing

The mappings are written out as exhaustive switches rather than casts, even
though the two enumerations are parallel to their wire counterparts today. A
`static_cast` keeps compiling after someone inserts a value into either side,
and the result is every road on the dash reporting the class of its neighbour.
Spelled out, `-Wswitch-enum` makes that insertion a build failure here.
`speedSourceOf()` was a `static_cast` at both call sites before this library
existed, and "sign" versus "guessed from the road class" is the difference
between a limit a dash may show a driver and one it must not.

`fillSpeed()` takes `hasPosted` from the segment's flag, never from
`postedKph` being non-zero: zero is a legal limit in exactly the places where
getting it wrong matters.

## Tests

| Target | Labels | Proves |
|---|---|---|
| `map_wire_test_segment` | `map_wire unit` | Every `RouteClass` and every `SpeedSource` maps to its own wire value, checked value by value rather than for a couple of classes; a speed without the posted flag says so on the wire; a way id past 2^31 reaches the wire unchanged. |

The switch makes adding a value a build failure; this test makes reordering one
a test failure.
