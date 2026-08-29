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

    // MULTIPLY, never divide, in the per-sample path. This loop is the single
    // hottest thing in the app -- profiled at ~10% of a core with eight 2.5 kHz
    // traces in a 30 s window -- and a double division per sample was most of
    // it: the original `(t - t_begin) / span * columns` cost ~4x this form.
    const double scale = static_cast<double>(columns) / span;

    // Start at the first sample inside the window rather than at the beginning
    // of the history. This is the whole reason the history supports a binary
    // search, and it is what makes the cost depend on the window rather than on
    // how much is retained.
    const std::size_t start = history.lowerBound(t_begin);

    // One column is accumulated IN REGISTERS and written out once, when the
    // samples cross its right edge. Times are non-decreasing, so columns only
    // ever advance; per-sample work is then a compare against the edge and two
    // register min/max updates, instead of recomputing the column index and
    // read-modify-writing out[column] for every sample. At telemetry rates a
    // column holds ~100+ samples, so the flush is rare.
    std::size_t filled = 0;
    std::size_t column = 0;
    double next_edge = 0.0;
    double cmin = 0.0;
    double cmax = 0.0;
    double cfirst = 0.0;
    double clast = 0.0;
    bool open = false;

    const auto flush = [&]() {
        if (open)
        {
            out[column] = ColumnStats{true, cmin, cmax, cfirst, clast};
            ++filled;
            open = false;
        }
    };

    // The clamp matters at both ends, exactly as before: a sample at t_end
    // would index one past the last column, and floating-point error near a
    // boundary can land just outside either way -- dropping the newest sample
    // off the right edge of every frame is exactly the kind of fault that looks
    // like the data rather than like a bug.
    const auto openColumn = [&](double t, double v) {
        const double position = (t - t_begin) * scale;
        column = 0;
        if (position > 0.0)
        {
            column = std::min(static_cast<std::size_t>(position), columns - 1);
        }
        next_edge =
            t_begin + static_cast<double>(column + 1) * span / static_cast<double>(columns);
        cmin = cmax = cfirst = clast = v;
        open = true;
    };

    for (std::size_t i = start; i < history.size(); ++i)
    {
        const Sample& sample = history[i];
        const double t = sample.t;
        if (t > t_end)
        {
            // Times are non-decreasing, so the first sample past the right edge
            // means every later one is too.
            break;
        }

        if (!open)
        {
            openColumn(t, sample.v);
            continue;
        }

        // `column + 1 < columns` keeps the LAST column open past its edge, so a
        // sample landing exactly on t_end stays in it -- the closed-at-the-top
        // rule the header documents.
        if (t >= next_edge && column + 1 < columns)
        {
            flush();
            openColumn(t, sample.v);
            continue;
        }

        const double v = sample.v;
        if (v < cmin)
        {
            cmin = v;
        }
        if (v > cmax)
        {
            cmax = v;
        }
        // Samples arrive in time order, so the last one seen for a column is
        // the latest one in it.
        clast = v;
    }
    flush();

    return filled;
}

}  // namespace scope
