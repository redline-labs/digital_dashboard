// SPDX-License-Identifier: GPL-3.0-or-later
//
// Distances, bearings and the Hilbert curve.
//
// Small enough to be header-only and worth keeping that way: the builder calls
// these tens of millions of times and the matcher calls them per candidate per
// fix.
#ifndef ROAD_GRAPH_GEOMETRY_H
#define ROAD_GRAPH_GEOMETRY_H

#include <cmath>
#include <cstdint>
#include <numbers>

namespace road_graph
{

// 1e-7 degrees, as everywhere else in this tree.
using Coord = std::int32_t;

constexpr double kCoordScale = 1e-7;
constexpr double kEarthRadiusM = 6371008.8;

constexpr double toDegrees(Coord value)
{
    return static_cast<double>(value) * kCoordScale;
}

constexpr Coord fromDegrees(double value)
{
    return static_cast<Coord>(value < 0 ? value * 1e7 - 0.5 : value * 1e7 + 0.5);
}

// Metres between two points, on an equirectangular approximation about their
// own latitude.
//
// Not haversine: over the distances a road segment spans -- tens of metres to a
// few kilometres -- the difference is well under a centimetre, and this runs in
// a builder loop that executes a hundred million times. The approximation
// breaks near the poles, where there are no roads.
inline double distanceM(Coord lat1, Coord lon1, Coord lat2, Coord lon2)
{
    const double rad = std::numbers::pi / 180.0;
    const double meanLat = (toDegrees(lat1) + toDegrees(lat2)) * 0.5 * rad;
    const double dLat = (toDegrees(lat2) - toDegrees(lat1)) * rad;
    const double dLon = (toDegrees(lon2) - toDegrees(lon1)) * rad * std::cos(meanLat);
    return std::sqrt(dLat * dLat + dLon * dLon) * kEarthRadiusM;
}

// Bearing in degrees clockwise from true north.
inline double bearingDeg(Coord lat1, Coord lon1, Coord lat2, Coord lon2)
{
    const double rad = std::numbers::pi / 180.0;
    const double meanLat = (toDegrees(lat1) + toDegrees(lat2)) * 0.5 * rad;
    const double north = (toDegrees(lat2) - toDegrees(lat1));
    const double east = (toDegrees(lon2) - toDegrees(lon1)) * std::cos(meanLat);
    double degrees = std::atan2(east, north) * 180.0 / std::numbers::pi;
    if (degrees < 0.0)
    {
        degrees += 360.0;
    }
    return degrees;
}

// Smallest absolute difference between two bearings, 0..180.
inline double bearingDeltaDeg(double a, double b)
{
    double delta = std::fmod(std::fabs(a - b), 360.0);
    return delta > 180.0 ? 360.0 - delta : delta;
}

// The point on segment (a,b) closest to p, as a fraction along it, plus the
// distance to it. Everything the matcher needs from one candidate.
struct Projection
{
    // 0 at a, 1 at b.
    double t { 0.0 };
    Coord lat { 0 };
    Coord lon { 0 };
    double distanceM { 0.0 };
};

inline Projection projectOnto(Coord pLat, Coord pLon, Coord aLat, Coord aLon, Coord bLat,
                              Coord bLon)
{
    // Worked in a local planar frame scaled by cos(lat) so that a degree of
    // longitude is the same length as a degree of latitude. Doing it in raw
    // degrees would bias every projection eastward at this latitude by a factor
    // of 1/cos(34 deg) -- about 1.2, which is metres on a road width and is
    // exactly enough to pick the frontage road.
    const double rad = std::numbers::pi / 180.0;
    const double scale = std::cos(toDegrees(pLat) * rad);

    const double ax = toDegrees(aLon) * scale;
    const double ay = toDegrees(aLat);
    const double bx = toDegrees(bLon) * scale;
    const double by = toDegrees(bLat);
    const double px = toDegrees(pLon) * scale;
    const double py = toDegrees(pLat);

    const double dx = bx - ax;
    const double dy = by - ay;
    const double lengthSq = dx * dx + dy * dy;

    double t = 0.0;
    if (lengthSq > 0.0)
    {
        t = ((px - ax) * dx + (py - ay) * dy) / lengthSq;
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
    }

    Projection out;
    out.t = t;
    out.lat = static_cast<Coord>(std::llround(static_cast<double>(aLat) +
                                              t * (static_cast<double>(bLat) - aLat)));
    out.lon = static_cast<Coord>(std::llround(static_cast<double>(aLon) +
                                              t * (static_cast<double>(bLon) - aLon)));
    out.distanceM = distanceM(pLat, pLon, out.lat, out.lon);
    return out;
}

// Hilbert index of a point on a 2^order grid.
//
// Used to order nodes and segments so that things near each other in the world
// are near each other in the file. That is decision 5 in format.h, and it is
// the difference between an A* expansion touching a few hundred pages and a few
// hundred thousand.
inline std::uint64_t hilbertIndex(std::uint32_t x, std::uint32_t y, std::uint32_t order = 16)
{
    std::uint64_t index = 0;
    for (std::uint32_t s = 1u << (order - 1); s > 0; s >>= 1)
    {
        const std::uint32_t rx = (x & s) > 0 ? 1 : 0;
        const std::uint32_t ry = (y & s) > 0 ? 1 : 0;
        index += static_cast<std::uint64_t>(s) * s * ((3 * rx) ^ ry);

        // Rotate the quadrant.
        if (ry == 0)
        {
            if (rx == 1)
            {
                x = s - 1 - x;
                y = s - 1 - y;
            }
            std::uint32_t swap = x;
            x = y;
            y = swap;
        }
    }
    return index;
}

// Map a coordinate onto the Hilbert grid used for ordering.
inline std::uint64_t hilbertOf(Coord lat, Coord lon)
{
    // The full world, folded into 16 bits per axis: about 600 m at the equator,
    // which is finer than the page granularity the ordering exists to serve.
    const double x = (toDegrees(lon) + 180.0) / 360.0;
    const double y = (toDegrees(lat) + 90.0) / 180.0;
    const auto gx = static_cast<std::uint32_t>(x * 65535.0);
    const auto gy = static_cast<std::uint32_t>(y * 65535.0);
    return hilbertIndex(gx, gy);
}

} // namespace road_graph

#endif // ROAD_GRAPH_GEOMETRY_H
