// SPDX-License-Identifier: GPL-3.0-or-later
//
// Asking map_server where to drive.
//
// This is the half of the node that dogfoods: map/graph, map/route, map/nearest,
// map/track_catalog and map/track_detail had no C++ caller anywhere in the tree
// before this file. They are all query/reply, all answered by nodes/map_server,
// and all called here exactly once at startup -- the simulation itself talks to
// nothing.
//
// Two vocabularies of failure, deliberately not merged: the routing services
// answer with MapQueryStatus and the track services with MapStatus (see the
// comment at the top of schemas/map_graph.capnp for why). Both are reported to
// the user in their own words rather than flattened into "failed".

#ifndef BD992_MOCK_SOURCES_H
#define BD992_MOCK_SOURCES_H

#include <cstddef>
#include <string>

#include "node_config.h"
#include "path.h"

namespace bd992_mock
{

// What came back, alongside the path itself, so --check can report it and the
// startup log line can say what it is about to drive.
struct SourceReport
{
    std::string error;

    // Route only. The server's own door-to-door figures, which include the
    // lead-in and lead-out to the first and last junctions -- so they will not
    // exactly equal the geometry's own length.
    double serverDistanceM { 0.0 };
    double serverDurationS { 0.0 };
    std::size_t segmentCount { 0 };

    // Route only, and only when speed limits were asked for.
    std::size_t speedQueries { 0 };
    std::size_t speedMatched { 0 };
    std::size_t speedPosted { 0 };
    double speedElapsedS { 0.0 };

    // Track only.
    std::string trackId;
    std::string buildId;
    double publishedLengthM { 0.0 };
    double medianWidthM { 0.0 };
    // The catalogue's own centreline length, against which the geometry's
    // computed length is a cross-check rather than a second source of truth.
    double catalogLengthM { 0.0 };
};

struct RouteOptions
{
    double fromLatitudeDeg { 0.0 };
    double fromLongitudeDeg { 0.0 };
    double toLatitudeDeg { 0.0 };
    double toLongitudeDeg { 0.0 };

    // Which cost profile; empty means the graph's default.
    std::string profile;

    // One map/nearest query per distinct segment, to pick up posted limits.
    // Off makes the drive uniform at the cruise speed, which is faster to start
    // and much less interesting to look at.
    bool speedLimits { true };
};

struct TrackOptions
{
    // An exact track id, or a substring of a name. Exact id wins.
    std::string query;
};

// Resolve the graph to route on. Returns empty on failure, with `error` set.
//
// Asking rather than defaulting is what lets the node run with no config file:
// map/graph with an empty name lists everything the server has, and the first
// one that opened is the only sensible choice when there is usually exactly one.
std::string resolveGraph(const ServicesConfig& services, std::string& error);

// Build a path from map/route. `graph` must already be resolved.
bool buildRoutePath(const ServicesConfig& services, const std::string& graph,
                    const RouteOptions& options, Path& path, SourceReport& report);

// Build a path from a track centreline.
bool buildTrackPath(const ServicesConfig& services, const TrackOptions& options, Path& path,
                    SourceReport& report);

} // namespace bd992_mock

#endif // BD992_MOCK_SOURCES_H
