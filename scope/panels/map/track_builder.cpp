#include "map_panel/track_builder.h"

#include <algorithm>
#include <cmath>

namespace scope::track
{
namespace
{

// The newest sample at or before `t`, or nothing. Zero-order hold.
std::optional<double> holdAt(const SampleHistory& history, double t)
{
    if (history.size() == 0)
    {
        return std::nullopt;
    }

    // lowerBound gives the first sample with time >= t; the one we want is the
    // one before it, unless that first sample is exactly at t.
    const std::size_t bound = history.lowerBound(t);
    if (bound < history.size() && history[bound].t <= t)
    {
        return history[bound].v;
    }
    if (bound == 0)
    {
        return std::nullopt;
    }
    return history[bound - 1].v;
}

}  // namespace

BuildStats build(const SampleHistory& latitude, const SampleHistory& longitude,
                 const SampleHistory* color, std::vector<Point>& out)
{
    BuildStats stats;

    out.clear();
    out.reserve(std::min(latitude.size(), longitude.size()));

    std::size_t i = 0;
    std::size_t j = 0;
    while (i < latitude.size() && j < longitude.size())
    {
        const Sample& lat = latitude[i];
        const Sample& lon = longitude[j];
        const double delta = lat.t - lon.t;

        if (std::abs(delta) <= kPairToleranceSeconds)
        {
            Point point;
            point.t = lat.t;
            // Clamped inside worldFor: past the Mercator limit tan() runs to
            // infinity and every later arithmetic yields NaN, which paints
            // nothing at all and says nothing about why.
            point.world = map_render::worldFor(
                map_render::Coordinate{map_render::clampLatitude(lat.v),
                                       map_render::wrapLongitude(lon.v)});

            if (color != nullptr)
            {
                if (const std::optional<double> held = holdAt(*color, point.t))
                {
                    point.color = *held;
                    point.has_color = true;
                }
            }

            out.push_back(point);
            ++stats.paired;
            ++i;
            ++j;
        }
        else if (delta < 0.0)
        {
            // This latitude has no longitude at the same instant.
            ++stats.unpaired_latitude;
            ++i;
        }
        else
        {
            ++stats.unpaired_longitude;
            ++j;
        }
    }

    // Whatever is left over never had a partner either.
    stats.unpaired_latitude += latitude.size() - i;
    stats.unpaired_longitude += longitude.size() - j;

    return stats;
}

void thin(const std::vector<Point>& in, const map_render::Projection& projection, double min_px,
          std::vector<Point>& out)
{
    out.clear();
    if (in.empty())
    {
        return;
    }
    out.reserve(in.size());

    const double min_squared = min_px * min_px;

    map_render::ScreenPoint last = projection.screenFor(in.front().world);
    out.push_back(in.front());

    // The last point is handled after the loop rather than inside it, so the
    // decision "is this far enough from the previous KEPT point" stays one rule.
    for (std::size_t index = 1; index + 1 < in.size(); ++index)
    {
        const map_render::ScreenPoint here = projection.screenFor(in[index].world);
        const double dx = here.x - last.x;
        const double dy = here.y - last.y;
        if ((dx * dx) + (dy * dy) < min_squared)
        {
            continue;
        }
        out.push_back(in[index]);
        last = here;
    }

    if (in.size() > 1)
    {
        // ALWAYS. It is where the vehicle ended up, and dropping it shortens the
        // track by up to min_px -- which at a low zoom is kilometres.
        out.push_back(in.back());
    }
}

std::optional<std::size_t> nearest(const std::vector<Point>& points,
                                   const map_render::Projection& projection,
                                   const map_render::ScreenPoint& screen, double radius_px)
{
    std::optional<std::size_t> best;
    double best_squared = radius_px * radius_px;

    for (std::size_t index = 0; index < points.size(); ++index)
    {
        const map_render::ScreenPoint here = projection.screenFor(points[index].world);
        const double dx = here.x - screen.x;
        const double dy = here.y - screen.y;
        const double squared = (dx * dx) + (dy * dy);

        // Strictly less, so the FIRST of two equidistant points wins and the
        // result is stable rather than depending on iteration order.
        if (squared < best_squared)
        {
            best_squared = squared;
            best = index;
        }
    }

    return best;
}

std::optional<std::size_t> at(const std::vector<Point>& points, double t)
{
    if (points.empty() || t < points.front().t)
    {
        return std::nullopt;
    }

    // First point strictly after t; the one we want is the one before it.
    const auto after = std::upper_bound(
        points.begin(), points.end(), t,
        [](double value, const Point& point) { return value < point.t; });

    return static_cast<std::size_t>(std::distance(points.begin(), after)) - 1;
}

}  // namespace scope::track
