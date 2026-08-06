#ifndef SCOPE_DECIMATE_H_
#define SCOPE_DECIMATE_H_

#include "scope/sample_ring.h"

#include <cstddef>
#include <vector>

namespace scope
{

// What one pixel column of a plot needs to know about the samples that fall in
// it.
struct ColumnStats
{
    bool has_data = false;

    // The extent of the values in this column. Drawn as a vertical segment,
    // which is what makes a decimated trace still show the spikes: a naive
    // "take every Nth sample" loses exactly the extremes you were looking for.
    double min = 0.0;
    double max = 0.0;

    // The first and last values in time order, used to join this column to its
    // neighbours. Without them a trace made only of vertical segments looks
    // like a comb wherever the signal is smooth.
    double first = 0.0;
    double last = 0.0;
};

// Reduce a window of history to one ColumnStats per pixel column.
//
// This is what keeps a plot cheap regardless of how much is retained: the work
// per frame is bounded by the width of the widget, not by the number of
// samples. A window holding a million points costs one binary search plus a
// walk of only the points actually inside it.
//
// `out` is resized to `columns` and fully overwritten, so a caller can keep one
// vector across frames and never allocate. Returns how many columns got data,
// which is what tells a caller "the window is empty" without scanning.
//
// Column i covers [t_begin + i*dt, t_begin + (i+1)*dt), except the last, which
// includes t_end so the newest sample is never dropped off the right edge.
std::size_t decimateMinMax(const SampleHistory& history,
                           double t_begin,
                           double t_end,
                           std::size_t columns,
                           std::vector<ColumnStats>& out);

}  // namespace scope

#endif  // SCOPE_DECIMATE_H_
