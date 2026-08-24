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
    // Three GSOF record topics. The position is the trigger; the other two are
    // held as latest-known and paired by arrival age -- see fix_assembler.h.
    //
    // Record 38 is deliberately absent: the matcher reads the fix quality it
    // needs from record 12's RMS, and subscribing to a topic to ignore it would
    // just be another thing to keep configured.
    std::string positionKey { "nodes/bd992/gsof/lat_long_height" };
    std::string velocityKey { "nodes/bd992/gsof/velocity" };
    std::string sigmaKey { "nodes/bd992/gsof/position_sigma" };

    // How recently a velocity or accuracy record must have arrived to be used
    // with a position. Not a rate, and not tied to one: records the receiver
    // sends together arrive microseconds apart and pair with room to spare,
    // while records on a slower schedule pair when they are fresh and are
    // reported absent when they are not.
    //
    // Set it by how long a heading stays true rather than by the output rate. A
    // vehicle turning at 10 deg/s moves 2 degrees in 200 ms, which is well
    // inside the matcher's tolerance; a second would not be.
    std::uint32_t pairWithinMs { 200 };

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
