---
title: map_rules
parent: Libraries
---

# map_rules

## Overview

What a piece of OSM data is, decided once, for everyone who asks. The tiler
and the graph builder in [map_build](../tools/map_build.html) both call
`classify()`; neither keeps rules of its own. Two rule sets drift, and the
symptom is a route down a road the map does not draw, which costs a day before
anyone suspects the cause. This library is the reason the extractor is ours
rather than tilemaker's Lua beside a C++ graph builder; the argument is laid
out in [Map build](../design/map-build.html).

Rules are C++ tables rather than a scripting language: the tree embeds no
scripting, a Lua profile is precisely what owning the extractor removes, and a
rule set that compiles is one `-Wswitch-enum` can check. There is no dependency
on [osm](osm.html) on purpose. `classify()` takes a span of string pairs, so it
is testable against a table of tags with no PBF anywhere near it, and a future
consumer that gets its tags from somewhere else can use it unchanged. Nothing
here does I/O or logs.

## Public headers

| Header | Declares |
|---|---|
| `map_rules/classification.h` | `RenderClass`, `RouteClass`, `PlaceKind`, `SpeedSource`, the `AccessMask` bits, `RoadClassification`, `TagView`, `Shape`, `classify()`, `classifyPlace()`, `parseMaxspeed()`, `isRoad()` and the `to_string()` overloads. |
| `map_rules/labels.h` | `LabelFeature`, the `hasLabelTags()` gate, and `classifyPoi()`, `classifyAeroway()`, `classifyAerodrome()`, `classifyPark()`, `classifyMountainPeak()`. |

## Using it

Link the `map_rules` target. Build a `TagView` over the entity's tag pairs,
say whether the geometry closes, and read both answers off the struct. This is
what `tools/map_build/extract.cpp` does for every way.

```cpp
const map_rules::TagView tags = scratch.view(*block, way);
const auto refs = block->refs(way);
const bool closed = refs.size() > 2 && refs.front() == refs.back();
const auto classification = map_rules::classify(tags, { closed });
if (classification.routable()) { /* it becomes a graph segment */ }
if (classification.drawn())    { /* it becomes a tile feature */ }
if (map_rules::hasLabelTags(tags)) { /* ask the label classifiers */ }
```

`road_graph::Builder::SegmentInput` carries the `RoadClassification` as-is;
the graph never re-derives it.

## Behaviour worth knowing

**One tag parse, many answers.** `classify()` returns a struct, not one enum,
because the sharing is not symmetric: the tiler is zoom-dependent and works on
simplified geometry, the graph is neither, and `highway=footway` is drawn but
only routable under a pedestrian profile. A single enum would force render-only
and access-only vocabularies into one namespace and end in two tables again.
`RenderClass` and `RouteClass` are deliberately different vocabularies; there
is no `Building` in the second and `Path` there means something a pedestrian
profile can use. `routable()` requires both a route class and a non-zero access
mask.

**The drawn and routable answers are allowed to differ.** A service road is
driveable before it is drawn; a footway is drawn and not driveable; an unknown
`highway=*` value is drawn but not routed; a ferry is routable without being a
highway. The test suite asserts the asymmetries explicitly, because a test that
only checked the two agree would pass while a car was routed down a footpath.

**Two speeds, and they are different numbers.** `postedSpeedKph` is what a sign
says and is frequently absent; roughly half of `highway=residential` has no
`maxspeed`. Presence is the `hasPosted` flag, not a sentinel, because zero is a
legal limit at exactly the barriers and gates where getting it wrong matters.
`freeFlowSpeedKph` is always defined because a router cannot cost an edge
without one, and must never be shown to a driver. `SpeedSource::Sign` is the
only source a display may present as a posted limit; `ConditionalIgnored` means
a `maxspeed:conditional` exists and was not evaluated. `parseMaxspeed()` is
exposed on its own because `mph` misread as km/h is a plausible-looking 1.6x
error.

**Closed enums on purpose.** Every `RenderClass` value is switched in the
tessellator, the style struct, the tiler and the label ranker, so adding one is
a four-file edit that the compiler enforces. The long tail lives in the access
bitmask and in `className`, a pointer into static storage because the struct is
filled nine million times per build. `PlaceKind` is ordered largest to
smallest so the ordinal is the label priority.

**Labels are the opposite shape.** `amenity`, `shop` and `tourism` carry
thousands of values, so `LabelFeature::subclass` passes OSM's own value through
as a borrowed `string_view` and only the coarse bucket is closed. `hasLabelTags()`
exists because of a silent failure: `aeroway=runway` has no render class and no
route class, so an extractor that keeps only drawn-or-routable ways discards it
before any label rule is asked, and the airport comes out with no tarmac.

{: .warning }
`LabelFeature::subclass` points into the tags that were handed in. In
`map_build` those are views into a decompressed PBF block that is dropped as
soon as the extractor moves on. Copy it before the block goes; never store the
feature.

## Tests

| Target | Labels | Proves |
|---|---|---|
| `map_rules_test_classification` | `map_rules unit` | The asymmetric cases above; access tags close a road; `oneway=-1` is not read as forward; a roundabout is oneway without saying so; `mph` is converted; posted and free-flow differ; a conditional limit is flagged; areas classify only when closed; water, landuse, bridge, tunnel and layer are carried; an empty tag set classifies as nothing. |
| `map_rules_test_labels` | `map_rules unit` | A runway is a label though nothing draws it; the gate agrees with every classifier; a nature reserve is a park and not ground cover; a city park is ground cover and not a park-layer feature; a summit needs no name; a terminal is a building, not tarmac; swimming pools are water; a class name is never borrowed from the tags. Every case was found by counting features against tilemaker on a real Southern California extract. |
