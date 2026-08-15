// SPDX-License-Identifier: GPL-3.0-or-later
//
// The map_match node's YAML configuration.
//
// Same shape as nodes/map_server and nodes/bd992_bridge: plain structs with
// in-class defaults, a parse that accumulates every problem, and a
// string-taking overload so the parser is testable without a file.
#ifndef MAP_MATCH_NODE_CONFIG_H
#define MAP_MATCH_NODE_CONFIG_H

#include <cstdint>
#include <string>

namespace map_match
{

struct PositionConfig
{
    // The FUSED epoch topic, not a per-record one.
    //
    // A matcher needs position, heading and quality for the SAME instant.
    // <prefix>/gsof/lat_long_height carries a position and nothing else -- no
    // time, no velocity -- and pairing it with the velocity topic by arrival
    // order yields a heading from a different epoch, which is off by however
    // far the vehicle turned and puts it on the frontage road. See
    // schemas/gsof_epoch.capnp.
    std::string zenohKey { "nodes/bd992/epoch" };
    std::string schemaType { "GsofEpoch" };

    // Older than this and the beam is dropped rather than carried: a gap in the
    // stream means the next fix has no trustworthy predecessor, and explaining
    // a jump across town as a very long detour is worse than starting fresh.
    std::uint32_t staleAfterMs { 3000 };
};

struct MatchConfig
{
    double searchRadiusM { 50.0 };
    std::uint16_t beamWidth { 6 };

    // The emission width is taken from the receiver and clamped into this
    // range. The floor is a statement about the MAP's accuracy, not the
    // receiver's: OSM centrelines are metres off, so believing a 2 cm RTK fix
    // completely would make every candidate impossible.
    double minSigmaM { 4.0 };
    double maxSigmaM { 30.0 };
    double defaultSigmaM { 8.0 };

    double transitionBetaM { 15.0 };
    double headingValidAboveMps { 1.5 };

    // How far ahead to build the path.
    std::uint32_t lookaheadM { 2000 };
};

struct ServiceConfig
{
    std::string horizonKey { "nodes/map_match/horizon" };
    std::string statusKey { "nodes/map_match/status" };

    // Publish rate for the horizon. The matcher runs at whatever the receiver
    // sends; this is how often the result goes on the bus.
    std::uint32_t horizonIntervalMs { 100 };
    std::uint32_t statusIntervalMs { 5000 };
};

struct NodeConfig
{
    // The graph file, by path -- this node opens it directly rather than asking
    // map_server. A matcher needs a candidate set and several bounded searches
    // per fix, which is dozens of lookups at 10 Hz; round-tripping those over
    // zenoh would buy latency and a hard cross-process dependency for what is a
    // pointer dereference into an mmap. A read-only mmap is shareable, so both
    // processes hold the same file with no coordination.
    std::string graphPath;

    PositionConfig position;
    MatchConfig match;
    ServiceConfig services;
};

bool parse_node_config(const std::string& yaml, NodeConfig& out);
bool load_node_config(const std::string& path, NodeConfig& out);

} // namespace map_match

#endif // MAP_MATCH_NODE_CONFIG_H
