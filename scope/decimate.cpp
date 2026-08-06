#include "scope/decimate.h"

#include <algorithm>
#include <cmath>

namespace scope
{

std::size_t decimateMinMax(const SampleHistory& history,
                           double t_begin,
                           double t_end,
                           std::size_t columns,
                           std::vector<ColumnStats>& out)
{
    out.assign(columns, ColumnStats{});

    if (columns == 0 || !(t_end > t_begin) || history.empty())
    {
        // A zero-width widget, a degenerate window, or nothing retained yet.
        // All three are ordinary states during startup and resizing, not
        // errors: report no data and let the caller draw an empty frame.
        return 0;
    }

    const double span = t_end - t_begin;

    // Start at the first sample inside the window rather than at the beginning
    // of the history. This is the whole reason the history supports a binary
    // search, and it is what makes the cost depend on the window rather than on
    // how much is retained.
    const std::size_t start = history.lowerBound(t_begin);

    std::size_t filled = 0;
    for (std::size_t i = start; i < history.size(); ++i)
    {
        const Sample& sample = history[i];
        if (sample.t > t_end)
        {
            // Times are non-decreasing, so the first sample past the right edge
            // means every later one is too.
            break;
        }

        // The clamp matters at both ends. A sample exactly at t_end would index
        // one past the last column, and floating-point error near a boundary
        // can land just outside either way -- dropping the newest sample off
        // the right edge of every frame is exactly the kind of fault that looks
        // like the data rather than like a bug.
        const double position = (sample.t - t_begin) / span * static_cast<double>(columns);
        std::size_t column = 0;
        if (position > 0.0)
        {
            column = static_cast<std::size_t>(position);
            column = std::min(column, columns - 1);
        }

        ColumnStats& stats = out[column];
        if (!stats.has_data)
        {
            stats.has_data = true;
            stats.min = sample.v;
            stats.max = sample.v;
            stats.first = sample.v;
            stats.last = sample.v;
            ++filled;
            continue;
        }

        stats.min = std::min(stats.min, sample.v);
        stats.max = std::max(stats.max, sample.v);
        // Samples arrive in time order, so the last one seen for a column is
        // the latest one in it.
        stats.last = sample.v;
    }

    return filled;
}

}  // namespace scope
