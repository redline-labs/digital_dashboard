// SPDX-License-Identifier: GPL-3.0-or-later
#include "tracksets.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <spdlog/spdlog.h>

namespace map_server
{

namespace
{

constexpr double kCoordScale = 1e-7;
constexpr double kEarthRadiusM = 6371008.8;
constexpr double kMetresPerDegree = std::numbers::pi * kEarthRadiusM / 180.0;

// A floor on the tolerance, in metres.
//
// The centreline is sampled at a few metres and the outlines at rather less, so
// a tolerance below this removes nothing while still costing a full pass. Zero
// from a client means "everything", and everything for Milford Road Course is
// 55 000 points.
constexpr double kMinToleranceM = 0.25;

struct Point
{
    double x { 0.0 };
    double y { 0.0 };
};

// Perpendicular distance from `p` to the segment `a`-`b`, in metres.
double distanceToSegment(Point p, Point a, Point b)
{
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double lengthSquared = dx * dx + dy * dy;
    if (lengthSquared <= 0.0)
    {
        return std::hypot(p.x - a.x, p.y - a.y);
    }
    double t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0, 1.0);
    return std::hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy));
}

// Iterative Douglas-Peucker. Iterative rather than recursive because the input
// is caller-supplied and can be 55 000 points: a recursive implementation on a
// pathological input is a stack overflow in a server.
void simplifyRange(const std::vector<Point>& points, std::size_t first, std::size_t last,
                   double toleranceM, std::vector<bool>& keep)
{
    std::vector<std::pair<std::size_t, std::size_t>> pending { { first, last } };
    while (!pending.empty())
    {
        const auto [lo, hi] = pending.back();
        pending.pop_back();
        if (hi <= lo + 1)
        {
            continue;
        }

        double worst = 0.0;
        std::size_t worstIndex = lo;
        for (std::size_t i = lo + 1; i < hi; ++i)
        {
            const double d = distanceToSegment(points[i], points[lo], points[hi]);
            if (d > worst)
            {
                worst = d;
                worstIndex = i;
            }
        }

        if (worst > toleranceM)
        {
            keep[worstIndex] = true;
            pending.emplace_back(lo, worstIndex);
            pending.emplace_back(worstIndex, hi);
        }
    }
}

} // namespace

TracksetRegistry::TracksetRegistry(const std::vector<TracksetConfig>& configured)
{
    for (const auto& entry : configured)
    {
        auto trackset = std::make_unique<Trackset>();
        trackset->name = entry.name;
        trackset->path = entry.path;

        auto store = track_store::Store::open(entry.path);
        if (store)
        {
            SPDLOG_INFO("[trackset] '{}': {} tracks from {} (build {})", entry.name,
                        store->tracks().size(), entry.path, store->buildId());
            trackset->store = std::make_unique<track_store::Store>(std::move(*store));
        }
        else
        {
            // Kept, not dropped. See the note in the header: a name that is not
            // configured and a file that cannot be read want different answers.
            trackset->error = track_store::to_string(store.error());
            SPDLOG_WARN("[trackset] '{}': {}", entry.name, trackset->error);
        }

        mTracksets.push_back(std::move(trackset));
    }
}

Trackset* TracksetRegistry::find(std::string_view name)
{
    if (name.empty())
    {
        return mTracksets.empty() ? nullptr : mTracksets.front().get();
    }
    for (auto& trackset : mTracksets)
    {
        if (trackset->name == name)
        {
            return trackset.get();
        }
    }
    return nullptr;
}

const Trackset* TracksetRegistry::find(std::string_view name) const
{
    return const_cast<TracksetRegistry*>(this)->find(name);
}

std::size_t TracksetRegistry::openCount() const
{
    return static_cast<std::size_t>(
        std::count_if(mTracksets.begin(), mTracksets.end(),
                      [](const auto& trackset) { return trackset->store != nullptr; }));
}

std::vector<std::int32_t> simplifyCoords(const std::vector<std::int32_t>& lonLatE7,
                                         double toleranceM, bool closed)
{
    const std::size_t count = lonLatE7.size() / 2;
    if (count < 3)
    {
        return lonLatE7;
    }

    const double tolerance = std::max(kMinToleranceM, toleranceM);

    // Projected to metres about the geometry's own latitude before measuring.
    // A tolerance in degrees would be 30 % tighter east-west at the
    // Nordschleife than at the equator, and the same request would then return
    // different amounts of detail depending on where the track is.
    double latSum = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        latSum += static_cast<double>(lonLatE7[2 * i + 1]) * kCoordScale;
    }
    const double cosLat = std::cos(latSum / static_cast<double>(count) * std::numbers::pi / 180.0);

    std::vector<Point> points;
    points.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        points.push_back({ static_cast<double>(lonLatE7[2 * i]) * kCoordScale * kMetresPerDegree *
                               cosLat,
                           static_cast<double>(lonLatE7[2 * i + 1]) * kCoordScale *
                               kMetresPerDegree });
    }

    std::vector<bool> keep(count, false);
    keep.front() = true;
    keep.back() = true;
    if (closed)
    {
        // A closed ring has no natural endpoints, so the standard algorithm's
        // "keep the two ends" would pin an arbitrary vertex and leave the
        // opposite side of the loop as one long chord. Splitting at the point
        // farthest from the first gives two halves with real endpoints.
        std::size_t opposite = 0;
        double worst = -1.0;
        for (std::size_t i = 1; i < count; ++i)
        {
            const double d = std::hypot(points[i].x - points[0].x, points[i].y - points[0].y);
            if (d > worst)
            {
                worst = d;
                opposite = i;
            }
        }
        keep[opposite] = true;
        simplifyRange(points, 0, opposite, tolerance, keep);
        simplifyRange(points, opposite, count - 1, tolerance, keep);
    }
    else
    {
        simplifyRange(points, 0, count - 1, tolerance, keep);
    }

    std::vector<std::int32_t> out;
    out.reserve(lonLatE7.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        if (keep[i])
        {
            out.push_back(lonLatE7[2 * i]);
            out.push_back(lonLatE7[2 * i + 1]);
        }
    }
    return out;
}

} // namespace map_server
