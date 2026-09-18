#include "display_backlight/deadband.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace display_backlight
{

namespace
{

// A reading appearing or vanishing always counts.
template <typename Moved>
bool anyMoved(const std::vector<std::optional<double>>& published, const std::vector<std::optional<double>>& current,
              Moved moved)
{
    if (published.size() != current.size())
    {
        return true;
    }
    for (std::size_t i = 0; i < current.size(); ++i)
    {
        if (moved(published[i], current[i]))
        {
            return true;
        }
    }
    return false;
}

}  // namespace

bool luxMoved(std::optional<double> published, std::optional<double> current)
{
    if (published.has_value() != current.has_value())
    {
        return true;
    }
    if (!current)
    {
        return false;
    }
    const double band = kLuxDeadbandFraction * std::max(std::fabs(*published), kLuxDeadbandFloor);
    return std::fabs(*current - *published) >= band;
}

bool temperatureMoved(std::optional<double> published, std::optional<double> current)
{
    if (published.has_value() != current.has_value())
    {
        return true;
    }
    return current && std::fabs(*current - *published) >= kTemperatureDeadbandCelsius;
}

bool movedPastDeadband(const StatusValues& published, const StatusValues& current)
{
    return published.backlight != current.backlight || anyMoved(published.lux, current.lux, luxMoved) ||
           anyMoved(published.celsius, current.celsius, temperatureMoved);
}

}  // namespace display_backlight
