// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning a PBF into segments.
//
// The shared core of every verb: `verify` runs it and reports, `graph` runs it
// and feeds a road_graph::Builder, and the tiler will run it too. One
// extraction, one set of classification rules, many outputs -- which is the
// whole argument for owning this.
//
// THREE PASSES, and each exists for a reason:
//
//   1. Ways and relations only, marking every node id they reference. Node
//      blocks are skipped without decoding, via osm::peekDataBlock.
//   2. Ways only, counting how many routable ways use each node. A node used
//      twice is a junction, and junctions are where ways get split. This cannot
//      merge with pass 1 because the counter is indexed by the node store's
//      rank, which pass 1 is what produces.
//   3. Everything. Node blocks fill coordinates; way blocks emit segments.
//
// A fourth would be needed if the coordinate array did not fit in memory -- see
// the spill note in osm/node_store.h. It does, so there are three.
#ifndef MAP_BUILD_EXTRACT_H
#define MAP_BUILD_EXTRACT_H

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "osm/entity.h"
#include "osm/error.h"
#include "road_graph/builder.h"

namespace map_build
{

struct ExtractStats
{
    std::uint64_t blocks { 0 };
    std::uint64_t nodes { 0 };
    std::uint64_t ways { 0 };
    std::uint64_t relations { 0 };

    std::uint64_t drawnWays { 0 };
    // type=multipolygon relations seen, assembled into at least one closed
    // ring, and abandoned because their rings did not close. The last is the
    // number that matters: a relation whose rings do not close is a lake with a
    // piece missing, and it is invisible unless counted.
    std::uint64_t multipolygonsSeen { 0 };
    std::uint64_t multipolygonsAssembled { 0 };
    std::uint64_t multipolygonsUnclosed { 0 };
    std::uint64_t multipolygonRings { 0 };
    // Place labels: named nodes that become the `place` layer.
    std::uint64_t places { 0 };

    // Administrative-boundary relations, and the ways they resolved to. A
    // missing count that is non-zero at an extract edge is expected -- a
    // boundary is exactly what an extract tends to be cut along.
    std::uint64_t boundaryRelationsSeen { 0 };
    std::uint64_t boundaryRelationsUnrecognised { 0 };
    std::uint64_t boundaryWaysDrawn { 0 };
    std::uint64_t boundaryWaysMissing { 0 };

    // Features written into each label layer -- poi, housenumber, water_name
    // and the rest. Keyed by layer name rather than counted per field because
    // the set of label layers is open by design (see map_rules/labels.h), and a
    // counter per layer would have to be added alongside every new one.
    std::map<std::string, std::uint64_t> labels;
    std::uint64_t routableWays { 0 };
    std::uint64_t segments { 0 };
    std::uint64_t junctions { 0 };

    // Node ids some way asked for, and how many pass 3 actually filled.
    std::uint64_t referencedNodes { 0 };
    std::uint64_t resolvedNodes { 0 };

    // Ways dropped because a vertex did not resolve, SPLIT BY WHERE.
    //
    // The split is the whole value of the metric. A non-zero count at the edge
    // of the extract is normal -- ways are cut there. A non-zero count in the
    // INTERIOR means either the file is damaged or this reader is, and without
    // the split the two are one number that nobody can act on.
    std::uint64_t droppedAtBoundary { 0 };
    std::uint64_t droppedInInterior { 0 };

    std::uint64_t nodeStoreBytes { 0 };

    // type=restriction relations seen, and how many were in a form this build
    // understands. A via-WAY restriction ("no left turn across the whole of
    // this slip road") needs a path of segments rather than a single junction
    // and is counted rather than guessed at -- guessing would ban a turn
    // somewhere else on the same road.
    std::uint64_t restrictionsSeen { 0 };
    std::uint64_t restrictionsViaNode { 0 };
    std::uint64_t restrictionsViaWay { 0 };
    std::uint64_t restrictionsUnrecognised { 0 };

    // Render class name -> count, for eyeballing whether the rules did
    // something sane.
    std::map<std::string, std::uint64_t> renderClasses;
    std::map<std::string, std::uint64_t> routeClasses;

    // Coverage, 1e-7 degrees.
    std::int32_t west { 0 };
    std::int32_t south { 0 };
    std::int32_t east { 0 };
    std::int32_t north { 0 };
};

struct ExtractOptions
{
    std::filesystem::path input;
    // Progress every N blocks. Zero is silent.
    std::uint32_t progressEvery { 2000 };
};

// One DRAWN way, whole.
//
// Deliberately NOT split at junctions the way a routable segment is: a tile
// wants the road as one line so it can be simplified and labelled as one thing,
// and splitting it would multiply the feature count for no benefit. This is the
// same way, from the same classify() call, presented the other way round --
// which is the whole point of owning the extractor.
struct DrawInput
{
    std::int64_t osmWayId { 0 };
    // Interleaved lat/lon in 1e-7 degrees. One pair when this is a point.
    std::vector<osm::Coord> geometry;
    map_rules::RoadClassification classification;
    std::string name;
    std::string ref;
    bool closed { false };

    // Further rings, for an area assembled from a multipolygon RELATION.
    //
    // Roles are carried explicitly rather than as winding, because winding is
    // ambiguous here: Web Mercator's y grows southward, so a ring that is
    // counter-clockwise in latitude/longitude is clockwise once projected. The
    // tiler sets the winding after projecting, from these roles, and that is the
    // only place it is decided.
    std::vector<std::vector<osm::Coord>> outerRings;
    std::vector<std::vector<osm::Coord>> innerRings;

    // OSM's admin_level for a boundary line: 2 country, 4 state, 6 county,
    // 8 city. Zero means "not a boundary", which is why it is a level and not
    // an optional -- level 0 does not exist in the tagging scheme.
    std::uint8_t adminLevel { 0 };

    // A LABEL, not a line: a city, a town, a neighbourhood, carried by an OSM
    // node rather than a way.
    //
    // Points share this struct rather than getting one of their own because
    // everything downstream of classification treats them the same way -- they
    // are projected, bucketed into tiles and written into a layer exactly like
    // a road is. What differs is only that they are neither simplified nor
    // clipped, and the tiler branches on `isPoint` for that.
    bool isPoint { false };
    map_rules::PlaceClassification place;

    // WHICH LAYER, when the render class does not decide it.
    //
    // Empty is the normal case and means layerFor(renderClass) answers -- a
    // road goes in `transportation` because it is a road. The label layers are
    // different: a pharmacy, a peak and a house number are all points with a
    // name, distinguished only by which layer a style expects to find them in,
    // and there is nothing in the drawn-geometry vocabulary that separates
    // them. So they name their layer outright rather than growing RenderClass
    // with values the tessellator would have to switch on and ignore.
    //
    // Static storage, for the same reason RoadClassification::className is: the
    // layer names are a closed set of literals, and an owning string here would
    // cost bytes in every one of eleven million features to say nothing new.
    const char* layer { "" };

    // Attributes beyond the class, name and ref every feature carries.
    // Kept as strings because this is the open-vocabulary half of the schema --
    // `subclass` alone is several thousand OSM values wide.
    std::vector<std::pair<std::string, std::string>> attributes;
};

// A segment ready for road_graph::Builder.
using SegmentSink = std::function<void(road_graph::Builder::SegmentInput&&)>;
using RestrictionSink = std::function<void(const road_graph::Builder::RestrictionInput&)>;
using DrawSink = std::function<void(DrawInput&&)>;

osm::Result<ExtractStats> extract(const ExtractOptions& options, const SegmentSink& sink,
                                  const RestrictionSink& restrictions = {},
                                  const DrawSink& drawn = {});

} // namespace map_build

#endif // MAP_BUILD_EXTRACT_H
