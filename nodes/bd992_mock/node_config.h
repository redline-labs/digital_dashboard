// SPDX-License-Identifier: GPL-3.0-or-later
//
// The bd992_mock node's YAML configuration.
//
// Same shape as nodes/map_match, nodes/map_server and nodes/bd992_bridge: plain
// structs with in-class defaults, a parse that accumulates every problem, and a
// string-taking overload so the parser is testable without a file.
//
// UNLIKE those nodes, --config is OPTIONAL here. Every default below is already
// correct for this tree -- the service keys are map_server's own defaults, the
// publish prefix is bd992_bridge's, and the graph and trackset names are
// discovered from the server rather than guessed. A mock that needs a config
// file before it will say anything is a mock nobody reaches for.
#ifndef BD992_MOCK_NODE_CONFIG_H
#define BD992_MOCK_NODE_CONFIG_H

#include <cstdint>
#include <string>

namespace bd992_mock
{

// Where map_server is listening. These are map_server's own defaults from
// nodes/map_server/node_config.h; if you moved them there, move them here.
struct ServicesConfig
{
    std::string graphInfoKey { "map/graph" };
    std::string routeKey { "map/route" };
    std::string nearestKey { "map/nearest" };
    std::string trackCatalogKey { "map/track_catalog" };
    std::string trackDetailKey { "map/track_detail" };

    // The track catalogue is 994 entries, so it is the slowest of these by some
    // margin. One timeout covers all of them.
    std::uint32_t requestTimeoutMs { 5000 };

    // EMPTY MEANS ASK THE SERVER. The graph name is resolved by querying
    // map/graph and taking the first one that opened; the trackset name is left
    // empty on the wire, which map_tracks.capnp already defines as "the first
    // configured one". Naming either here pins it.
    std::string graph;
    std::string trackset;
};

// The vehicle, as a point mass on a path.
//
// Nothing here is a claim about a real car. It is the smallest set of numbers
// that makes position, speed and heading agree with each other: a speed that is
// reachable, a corner that is taken slowly enough to be survivable, and braking
// that starts before the corner rather than at it.
struct VehicleConfig
{
    // The ceiling, before posted limits and curvature take their bite. About
    // 60 mph.
    double cruiseSpeedMps { 27.0 };

    double accelMps2 { 2.0 };
    double brakeMps2 { 3.5 };

    // What sets corner speed: v = sqrt(lateral_accel * radius). 3 m/s^2 is a
    // brisk road car; a circuit lap wants 8-12.
    double lateralAccelMps2 { 3.0 };

    // Heading is measured to a point this far ahead rather than to the next
    // vertex. Road geometry is quantised to 1e-7 degrees (about 11 mm) and
    // vertices can be a metre apart, so an adjacent-vertex bearing on a straight
    // road jitters by tens of degrees -- which reads downstream as a vehicle
    // weaving, and map_match weights heading.
    double headingLookaheadM { 5.0 };

    // How far either side of a vertex the curvature circle is measured over.
    //
    // THE SAME QUANTISATION PROBLEM AS headingLookaheadM, and much worse.
    // Curvature comes from the sagitta -- how far the middle point bows off the
    // chord -- and on a 1.2 m chord around a 30 m corner that bow is 6 mm,
    // which is SMALLER THAN THE 11 mm COORDINATE GRID. Measured between
    // adjacent vertices, a dense arc's radius comes out anywhere from half to
    // double the truth, and the speed profile inherits the noise as phantom
    // corners on a straight road. Widening the baseline is what makes it a
    // measurement rather than a rounding artefact.
    //
    // The cost is that a corner shorter than this gets averaged with the
    // straights either side of it. 8 m is well under the length of any real
    // corner and about 700x the grid.
    double curvatureBaselineM { 8.0 };
};

struct PublishConfig
{
    // The REAL prefix, so every existing consumer works unchanged. See the
    // warning in main.cpp about running this alongside bd992_bridge.
    std::string topicPrefix { "nodes/bd992" };

    std::uint32_t rateHz { 10 };

    // There is NO ELEVATION in either source -- route geometry and track
    // centrelines are both lon/lat only -- so this is a constant rather than an
    // invented terrain model. Above the WGS-84 ellipsoid, not sea level.
    double ellipsoidHeightM { 50.0 };

    // Zero puts the vehicle exactly on the road, which is what you want when
    // looking at the map. Raise it to make map_match work for its living; the
    // seed is here so a run that misbehaves can be repeated.
    double positionNoiseM { 0.0 };
    std::uint32_t noiseSeed { 1 };
};

struct NodeConfig
{
    ServicesConfig services;
    VehicleConfig vehicle;
    PublishConfig publish;
};

bool parse_node_config(const std::string& yaml, NodeConfig& out);
bool load_node_config(const std::string& path, NodeConfig& out);

} // namespace bd992_mock

#endif // BD992_MOCK_NODE_CONFIG_H
