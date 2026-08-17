// SPDX-License-Identifier: GPL-3.0-or-later
//
// The line the vehicle drives, and the speed it may drive it at.
//
// THIS IS THE SEAM THAT MAKES ONE SIMULATOR SERVE BOTH SCENARIOS. A route from
// map/route and a track centreline from map/track_detail arrive as different
// schemas answering different questions, and both reduce to the same thing: an
// ordered list of points, a distance along it, and whether it closes. Nothing
// below this line knows which it came from, which is why the vehicle model is
// written once and why a lap and a commute cannot drift apart in behaviour.
//
// Deliberately free of zenoh, capnp and yaml so the geometry can be tested on
// its own against shapes with closed-form answers -- see tests/test_path.cpp,
// which drives a circular arc whose radius is known exactly.

#ifndef BD992_MOCK_PATH_H
#define BD992_MOCK_PATH_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "road_graph/geometry.h"

#include "node_config.h"

namespace bd992_mock
{

// A path with fewer than two points cannot be driven or even pointed along.
constexpr std::size_t kMinPathPoints = 2;

struct Path
{
    // Parallel arrays in 1e-7 degrees, the fixed-point vocabulary the rest of
    // this tree uses (road_graph::Coord).
    std::vector<road_graph::Coord> lat;
    std::vector<road_graph::Coord> lon;

    // Cumulative metres from index 0. Same length as lat/lon; distanceM[0] is
    // zero. For a closed path this does NOT include the leg from the last point
    // back to the first -- that is what lengthM() adds.
    std::vector<double> distanceM;

    // Per-vertex ceiling from something other than geometry: a posted speed
    // limit on a route. Infinity where nothing said otherwise, so a min() with
    // it is always safe.
    std::vector<double> speedCapMps;

    // A circuit, or a route driven as a loop. The vehicle wraps rather than
    // stopping, and a lap is counted at the wrap.
    bool closed { false };

    // For --check and the startup log line.
    std::string description;

    std::size_t size() const { return lat.size(); }

    // Total drivable length: for a closed path, including the closing leg back
    // to index 0.
    double lengthM() const;
};

// Decode interleaved LON, LAT in 1e-7 degrees into a path's geometry.
//
// BOTH MAP SOURCES ARE LON-FIRST -- MapRouteResponse.geometry and
// MapTrackDetailResponse.centerline, matching MapTileset.bounds -- while every
// GSOF field on the bus is lat-first. THE SWAP HAPPENS HERE, ONCE, and nowhere
// else. Getting it wrong produces two perfectly valid numbers describing a
// point in the ocean, which is why tests/test_path.cpp pins a known coordinate
// through this function rather than trusting it to review.
//
// Returns false with `problem` set for an odd-length list or too few points.
bool setGeometryFromLonLat(std::span<const std::int32_t> lonLat, Path& out, std::string& problem);

// Fill distanceM from the geometry. Called after setGeometryFromLonLat and
// after any deduplication.
void computeDistances(Path& path);

// Drop consecutive duplicate points.
//
// Not cosmetic: a zero-length leg makes the three-point curvature circle
// degenerate and would report a corner where the road is straight.
// docs/tracks.md records 87 of the 994 track outlines carrying runs of
// identical consecutive vertices, one of them 2 346 long.
void dropRepeatedPoints(Path& path);

// Radius in metres of the circle through `index` and the two points roughly
// `baselineM` either side of it. This is the whole cornering model:
// v = sqrt(a_lat * R).
//
// NOT the immediately adjacent vertices. On geometry sampled every metre or so
// -- which track centrelines are, and route geometry is around corners -- the
// bow of the middle point off the chord is millimetres, and the coordinates are
// quantised to 11 mm. The circle through three such points is fitted mostly to
// rounding, and comes out anywhere from half to double the real radius. See
// VehicleConfig::curvatureBaselineM.
//
// Infinity for a straight line, for any degenerate triple, and near the ends of
// an open path where there is not enough road either side to measure over --
// which is deliberate: reporting a noisy radius there would be worse than
// reporting none.
double curvatureRadiusM(const Path& path, std::size_t index, double baselineM);

// The per-vertex speed ceiling: curvature, posted limit and cruise speed,
// relaxed backwards so that braking for a corner starts before the corner.
//
// WITHOUT THE BACKWARD RELAXATION the vehicle arrives at a hairpin at 27 m/s
// and drops to 8 in one tick, which is a 190 m/s^2 deceleration and reads
// downstream as a GNSS glitch rather than as driving. That pass is the only
// interesting thing in this file, and tests/test_vehicle.cpp exists mainly to
// pin it.
std::vector<double> buildSpeedProfile(const Path& path, const VehicleConfig& vehicle);

// Where the vehicle is, and which way it points, at `s` metres along.
struct PathPoint
{
    road_graph::Coord lat { 0 };
    road_graph::Coord lon { 0 };
    // Degrees clockwise from true north.
    double headingDeg { 0.0 };
};

// `s` is clamped to [0, lengthM()] for an open path and wrapped for a closed
// one. Heading is measured to a point `headingLookaheadM` further on rather
// than to the next vertex -- see VehicleConfig::headingLookaheadM for why.
PathPoint samplePath(const Path& path, double s, double headingLookaheadM);

// The speed ceiling at `s`, interpolated between the vertices either side.
double speedCapAt(const Path& path, const std::vector<double>& profile, double s);

} // namespace bd992_mock

#endif // BD992_MOCK_PATH_H
