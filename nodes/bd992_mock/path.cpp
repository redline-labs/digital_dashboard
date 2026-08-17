// SPDX-License-Identifier: GPL-3.0-or-later
#include "path.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace bd992_mock
{
namespace
{

// The closing leg of a closed path, from the last point back to the first.
double closingLegM(const Path& path)
{
    const std::size_t n = path.size();
    if (n < kMinPathPoints)
    {
        return 0.0;
    }
    return road_graph::distanceM(path.lat[n - 1], path.lon[n - 1], path.lat[0], path.lon[0]);
}

// Interpolate between two vertices at fraction t, in fixed point.
//
// Rounded rather than truncated: truncation biases every interpolated position
// toward the equator and the prime meridian by half a unit, which is millimetres
// and harmless, but the bias is systematic and free to avoid.
road_graph::Coord lerpCoord(road_graph::Coord a, road_graph::Coord b, double t)
{
    const double value = static_cast<double>(a) + t * (static_cast<double>(b) - static_cast<double>(a));
    return static_cast<road_graph::Coord>(std::llround(value));
}

// Step outward from `index` along the path, accumulating distance, until at
// least `wantM` has been covered or the path runs out.
//
// Wraps on a closed path and stops at the ends of an open one, and never
// revisits the vertex it started from -- so a short circuit cannot fold back on
// itself and report a baseline it does not have.
void walkOut(const Path& path, std::size_t index, bool forward, double wantM,
             std::size_t& outIndex, double& outDistance)
{
    const std::size_t n = path.size();
    std::size_t current = index;
    double accumulated = 0.0;

    for (std::size_t step = 0; step + 1 < n; ++step)
    {
        std::size_t next = current;
        if (forward)
        {
            if (current + 1 < n)
            {
                next = current + 1;
            }
            else if (path.closed)
            {
                next = 0;
            }
            else
            {
                break;
            }
        }
        else
        {
            if (current > 0)
            {
                next = current - 1;
            }
            else if (path.closed)
            {
                next = n - 1;
            }
            else
            {
                break;
            }
        }

        accumulated += road_graph::distanceM(path.lat[current], path.lon[current], path.lat[next],
                                             path.lon[next]);
        current = next;

        if (accumulated >= wantM)
        {
            break;
        }
    }

    outIndex = current;
    outDistance = accumulated;
}

// Position at `s`, with `s` already normalised into range by the caller.
//
// Linear search would be O(n) per tick against a 40 km route's thousands of
// points; distanceM is sorted, so this is a bisection.
std::size_t vertexBefore(const Path& path, double s)
{
    const auto it = std::upper_bound(path.distanceM.begin(), path.distanceM.end(), s);
    if (it == path.distanceM.begin())
    {
        return 0;
    }
    return static_cast<std::size_t>(std::distance(path.distanceM.begin(), it) - 1);
}

// Normalise a distance into the path: wrapped for a closed path, clamped for an
// open one.
double normalise(const Path& path, double s)
{
    const double length = path.lengthM();
    if (length <= 0.0)
    {
        return 0.0;
    }

    if (!path.closed)
    {
        return std::clamp(s, 0.0, length);
    }

    double wrapped = std::fmod(s, length);
    if (wrapped < 0.0)
    {
        wrapped += length;
    }
    return wrapped;
}

// The point at `s`, without a heading.
void positionAt(const Path& path, double s, road_graph::Coord& lat, road_graph::Coord& lon)
{
    const std::size_t n = path.size();
    const double normalised = normalise(path, s);

    // Past the last vertex, which for a closed path means on the closing leg
    // back to index 0.
    if (normalised >= path.distanceM[n - 1])
    {
        if (!path.closed)
        {
            lat = path.lat[n - 1];
            lon = path.lon[n - 1];
            return;
        }

        const double leg = closingLegM(path);
        const double along = normalised - path.distanceM[n - 1];
        const double t = leg > 0.0 ? std::clamp(along / leg, 0.0, 1.0) : 0.0;
        lat = lerpCoord(path.lat[n - 1], path.lat[0], t);
        lon = lerpCoord(path.lon[n - 1], path.lon[0], t);
        return;
    }

    const std::size_t i = vertexBefore(path, normalised);
    const std::size_t j = i + 1;
    const double span = path.distanceM[j] - path.distanceM[i];
    const double t = span > 0.0 ? (normalised - path.distanceM[i]) / span : 0.0;

    lat = lerpCoord(path.lat[i], path.lat[j], t);
    lon = lerpCoord(path.lon[i], path.lon[j], t);
}

} // namespace

double Path::lengthM() const
{
    if (size() < kMinPathPoints)
    {
        return 0.0;
    }
    const double open = distanceM.back();
    return closed ? open + closingLegM(*this) : open;
}

bool setGeometryFromLonLat(std::span<const std::int32_t> lonLat, Path& out, std::string& problem)
{
    if ((lonLat.size() % 2) != 0)
    {
        problem = "geometry has an odd number of values, so it is not interleaved lon/lat";
        return false;
    }

    const std::size_t points = lonLat.size() / 2;
    if (points < kMinPathPoints)
    {
        problem = "geometry has " + std::to_string(points) + " point(s); at least " +
                  std::to_string(kMinPathPoints) + " are needed to drive it";
        return false;
    }

    out.lat.clear();
    out.lon.clear();
    out.lat.reserve(points);
    out.lon.reserve(points);

    for (std::size_t i = 0; i < points; ++i)
    {
        // LON FIRST. See the header.
        out.lon.push_back(static_cast<road_graph::Coord>(lonLat[(i * 2) + 0]));
        out.lat.push_back(static_cast<road_graph::Coord>(lonLat[(i * 2) + 1]));
    }

    return true;
}

void computeDistances(Path& path)
{
    const std::size_t n = path.size();
    path.distanceM.assign(n, 0.0);
    for (std::size_t i = 1; i < n; ++i)
    {
        path.distanceM[i] =
            path.distanceM[i - 1] +
            road_graph::distanceM(path.lat[i - 1], path.lon[i - 1], path.lat[i], path.lon[i]);
    }
}

void dropRepeatedPoints(Path& path)
{
    const std::size_t n = path.size();
    if (n == 0)
    {
        return;
    }

    std::vector<road_graph::Coord> lat;
    std::vector<road_graph::Coord> lon;
    std::vector<double> caps;
    lat.reserve(n);
    lon.reserve(n);

    const bool haveCaps = path.speedCapMps.size() == n;

    for (std::size_t i = 0; i < n; ++i)
    {
        if (!lat.empty() && path.lat[i] == lat.back() && path.lon[i] == lon.back())
        {
            continue;
        }
        lat.push_back(path.lat[i]);
        lon.push_back(path.lon[i]);
        if (haveCaps)
        {
            caps.push_back(path.speedCapMps[i]);
        }
    }

    path.lat = std::move(lat);
    path.lon = std::move(lon);
    if (haveCaps)
    {
        path.speedCapMps = std::move(caps);
    }
}

double curvatureRadiusM(const Path& path, std::size_t index, double baselineM)
{
    const double infinity = std::numeric_limits<double>::infinity();
    const std::size_t n = path.size();
    if (n < 3 || index >= n)
    {
        return infinity;
    }

    const double wanted = std::max(baselineM, 0.0);
    // Half a baseline still gives a sagitta comfortably above the coordinate
    // grid; less than that and the answer is rounding.
    const double minimum = wanted * 0.5;

    std::size_t before = index;
    std::size_t after = index;
    double reachedBack = 0.0;
    double reachedForward = 0.0;

    walkOut(path, index, false, wanted, before, reachedBack);
    walkOut(path, index, true, wanted, after, reachedForward);

    if (reachedBack < minimum || reachedForward < minimum)
    {
        // Near the end of an open path there is not enough road either side to
        // measure over. No answer beats a noisy one.
        return infinity;
    }
    if (before == after || before == index || after == index)
    {
        return infinity;
    }

    // The three side lengths of the triangle, then the circumradius
    // R = abc / (4 * area). Area comes from Heron's formula, which is where a
    // degenerate triple shows up as a zero.
    const double a = road_graph::distanceM(path.lat[before], path.lon[before], path.lat[index],
                                           path.lon[index]);
    const double b =
        road_graph::distanceM(path.lat[index], path.lon[index], path.lat[after], path.lon[after]);
    const double c = road_graph::distanceM(path.lat[before], path.lon[before], path.lat[after],
                                           path.lon[after]);

    if (a <= 0.0 || b <= 0.0 || c <= 0.0)
    {
        return infinity;
    }

    const double s = (a + b + c) * 0.5;
    const double squared = s * (s - a) * (s - b) * (s - c);
    if (squared <= 0.0)
    {
        // Collinear, or so nearly so that the subtraction lost it. Either way,
        // straight.
        return infinity;
    }

    const double area = std::sqrt(squared);
    if (area <= 0.0)
    {
        return infinity;
    }

    return (a * b * c) / (4.0 * area);
}

std::vector<double> buildSpeedProfile(const Path& path, const VehicleConfig& vehicle)
{
    const std::size_t n = path.size();
    std::vector<double> profile(n, vehicle.cruiseSpeedMps);
    if (n < kMinPathPoints)
    {
        return profile;
    }

    const bool haveCaps = path.speedCapMps.size() == n;

    // What each vertex's own curvature allows.
    std::vector<double> byCurvature(n, vehicle.cruiseSpeedMps);
    for (std::size_t i = 0; i < n; ++i)
    {
        const double radius = curvatureRadiusM(path, i, vehicle.curvatureBaselineM);
        if (std::isfinite(radius))
        {
            byCurvature[i] = std::min(vehicle.cruiseSpeedMps,
                                      std::sqrt(vehicle.lateralAccelMps2 * radius));
        }
    }

    // Then the SLOWEST such speed within a baseline either side.
    //
    // Necessary because the measurement above is optimistic exactly where a
    // corner begins: a baseline spanning the last of the straight and the first
    // of the corner fits a circle far larger than the corner's own, so the
    // entry vertices are permitted a speed the apex cannot hold -- and the
    // vehicle is already committed by the time the cap tightens. Taking the
    // window minimum makes a corner's limit start before the corner does, which
    // is both what a driver does and what keeps the lateral acceleration inside
    // the configured bound.
    for (std::size_t i = 0; i < n; ++i)
    {
        double slowest = byCurvature[i];

        for (int direction = 0; direction < 2; ++direction)
        {
            const bool forward = direction == 1;
            std::size_t current = i;
            double travelled = 0.0;

            for (std::size_t step = 0; step + 1 < n; ++step)
            {
                std::size_t next = current;
                if (forward)
                {
                    if (current + 1 < n)
                    {
                        next = current + 1;
                    }
                    else if (path.closed)
                    {
                        next = 0;
                    }
                    else
                    {
                        break;
                    }
                }
                else
                {
                    if (current > 0)
                    {
                        next = current - 1;
                    }
                    else if (path.closed)
                    {
                        next = n - 1;
                    }
                    else
                    {
                        break;
                    }
                }

                travelled += road_graph::distanceM(path.lat[current], path.lon[current],
                                                   path.lat[next], path.lon[next]);
                current = next;
                slowest = std::min(slowest, byCurvature[current]);

                if (travelled >= vehicle.curvatureBaselineM)
                {
                    break;
                }
            }
        }

        double cap = slowest;
        if (haveCaps && std::isfinite(path.speedCapMps[i]))
        {
            cap = std::min(cap, path.speedCapMps[i]);
        }

        profile[i] = cap;
    }

    // An open path ends at rest -- there is no more road.
    if (!path.closed)
    {
        profile[n - 1] = 0.0;
    }

    // Relax backwards: a vertex may be no faster than what braking from it can
    // still make the next one's limit. On a closed path the constraint travels
    // round the seam, so the pass repeats until it stops changing anything --
    // one pass would leave the vertices just before index 0 unbraked.
    constexpr int kMaxPasses = 8;
    constexpr double kSettled = 1e-6;

    for (int pass = 0; pass < kMaxPasses; ++pass)
    {
        double largestChange = 0.0;

        if (path.closed)
        {
            const double leg = closingLegM(path);
            const double reachable =
                std::sqrt((profile[0] * profile[0]) + (2.0 * vehicle.brakeMps2 * leg));
            if (reachable < profile[n - 1])
            {
                largestChange = std::max(largestChange, profile[n - 1] - reachable);
                profile[n - 1] = reachable;
            }
        }

        for (std::size_t i = n - 1; i > 0; --i)
        {
            const double ds = path.distanceM[i] - path.distanceM[i - 1];
            const double reachable =
                std::sqrt((profile[i] * profile[i]) + (2.0 * vehicle.brakeMps2 * ds));
            if (reachable < profile[i - 1])
            {
                largestChange = std::max(largestChange, profile[i - 1] - reachable);
                profile[i - 1] = reachable;
            }
        }

        if (largestChange < kSettled)
        {
            break;
        }
    }

    return profile;
}

PathPoint samplePath(const Path& path, double s, double headingLookaheadM)
{
    PathPoint out;
    const std::size_t n = path.size();
    if (n == 0)
    {
        return out;
    }
    if (n < kMinPathPoints)
    {
        out.lat = path.lat[0];
        out.lon = path.lon[0];
        return out;
    }

    const double length = path.lengthM();
    const double here = normalise(path, s);
    positionAt(path, here, out.lat, out.lon);

    // Heading is measured over a chord to a point further along, not to the
    // next vertex. Look backwards instead when there is no road left ahead, so
    // that a vehicle stopped at the end of a route still points along it rather
    // than reporting a bearing of zero (due north, in the middle of California).
    const double lookahead = std::max(headingLookaheadM, 0.0);
    road_graph::Coord aheadLat = 0;
    road_graph::Coord aheadLon = 0;

    if (path.closed || (here + lookahead) <= length)
    {
        positionAt(path, here + lookahead, aheadLat, aheadLon);
        out.headingDeg = road_graph::bearingDeg(out.lat, out.lon, aheadLat, aheadLon);
    }
    else
    {
        const double back = std::max(here - lookahead, 0.0);
        positionAt(path, back, aheadLat, aheadLon);
        out.headingDeg = road_graph::bearingDeg(aheadLat, aheadLon, out.lat, out.lon);
    }

    return out;
}

double speedCapAt(const Path& path, const std::vector<double>& profile, double s)
{
    const std::size_t n = path.size();
    if (n == 0 || profile.size() != n)
    {
        return 0.0;
    }
    if (n < kMinPathPoints)
    {
        return profile[0];
    }

    const double normalised = normalise(path, s);

    if (normalised >= path.distanceM[n - 1])
    {
        if (!path.closed)
        {
            return profile[n - 1];
        }
        const double leg = closingLegM(path);
        const double along = normalised - path.distanceM[n - 1];
        const double t = leg > 0.0 ? std::clamp(along / leg, 0.0, 1.0) : 0.0;
        return profile[n - 1] + t * (profile[0] - profile[n - 1]);
    }

    const std::size_t i = vertexBefore(path, normalised);
    const std::size_t j = i + 1;
    const double span = path.distanceM[j] - path.distanceM[i];
    const double t = span > 0.0 ? (normalised - path.distanceM[i]) / span : 0.0;
    return profile[i] + t * (profile[j] - profile[i]);
}

} // namespace bd992_mock
