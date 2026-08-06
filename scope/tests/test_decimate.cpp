// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-pixel-column min/max decimation, against a brute-force reference.
//
// This is the single piece of the renderer most worth testing, for two reasons.
// It is what makes a plot's cost depend on the width of the widget rather than
// on how much history is retained, so it runs on every signal on every frame.
// And every way it can be wrong looks like data rather than like a bug: a
// dropped last column silently clips the newest sample off the right edge of
// every frame, and losing the min/max in favour of "every Nth sample" quietly
// deletes exactly the spikes someone opened a scope to see.
//
// The reference implementation below is deliberately stupid -- it walks every
// sample and does the arithmetic the obvious way. Comparing the fast path
// against it over random data is worth more than any number of hand-picked
// cases, because the interesting failures are at boundaries nobody thinks to
// pick.

#include "scope/decimate.h"
#include "scope/sample_ring.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// The obvious implementation: for every column, scan every sample.
std::vector<scope::ColumnStats> reference(const std::vector<scope::Sample>& samples,
                                          double t_begin,
                                          double t_end,
                                          std::size_t columns)
{
    std::vector<scope::ColumnStats> out(columns);
    if (columns == 0 || !(t_end > t_begin))
    {
        return out;
    }

    const double span = t_end - t_begin;
    for (const scope::Sample& sample : samples)
    {
        if (sample.t < t_begin || sample.t > t_end)
        {
            continue;
        }

        const double position = (sample.t - t_begin) / span * static_cast<double>(columns);
        std::size_t column = 0;
        if (position > 0.0)
        {
            column = std::min(static_cast<std::size_t>(position), columns - 1);
        }

        scope::ColumnStats& stats = out[column];
        if (!stats.has_data)
        {
            stats = {true, sample.v, sample.v, sample.v, sample.v};
            continue;
        }
        stats.min = std::min(stats.min, sample.v);
        stats.max = std::max(stats.max, sample.v);
        stats.last = sample.v;
    }
    return out;
}

bool same(const scope::ColumnStats& lhs, const scope::ColumnStats& rhs)
{
    if (lhs.has_data != rhs.has_data)
    {
        return false;
    }
    if (!lhs.has_data)
    {
        return true;
    }
    return lhs.min == rhs.min && lhs.max == rhs.max && lhs.first == rhs.first &&
           lhs.last == rhs.last;
}

scope::SampleHistory historyOf(const std::vector<scope::Sample>& samples)
{
    scope::SampleHistory history(std::max<std::size_t>(samples.size(), 1));
    for (const scope::Sample& sample : samples)
    {
        history.append(sample);
    }
    return history;
}

// --------------------------------------------------------- against a reference

void testAgainstTheReferenceOverRandomData()
{
    std::mt19937 rng(20260805);
    std::uniform_real_distribution<double> value_dist(-1000.0, 1000.0);
    std::uniform_real_distribution<double> step_dist(0.0001, 0.05);

    std::vector<scope::ColumnStats> fast;

    for (int trial = 0; trial < 200; ++trial)
    {
        const std::size_t count = 1 + (rng() % 500);
        std::vector<scope::Sample> samples;
        samples.reserve(count);

        double t = 0.0;
        for (std::size_t i = 0; i < count; ++i)
        {
            samples.push_back({t, value_dist(rng)});
            t += step_dist(rng);
        }

        const scope::SampleHistory history = historyOf(samples);

        // Windows that variously undershoot, overshoot and sit inside the data,
        // because clipping behaviour at both edges is where this goes wrong.
        const double last_t = samples.back().t;
        std::uniform_real_distribution<double> begin_dist(-1.0, last_t);
        const double t_begin = begin_dist(rng);
        const double t_end = t_begin + std::uniform_real_distribution<double>(0.01, last_t + 2.0)(rng);
        const std::size_t columns = 1 + (rng() % 400);

        const std::size_t filled = scope::decimateMinMax(history, t_begin, t_end, columns, fast);
        const std::vector<scope::ColumnStats> slow = reference(samples, t_begin, t_end, columns);

        if (fast.size() != slow.size())
        {
            expect(false, "decimation produces one entry per column");
            return;
        }

        std::size_t expected_filled = 0;
        for (std::size_t i = 0; i < slow.size(); ++i)
        {
            if (slow[i].has_data)
            {
                ++expected_filled;
            }
            if (!same(fast[i], slow[i]))
            {
                expect(false, "decimation matches the brute-force reference");
                return;
            }
        }

        if (filled != expected_filled)
        {
            expect(false, "the reported filled-column count matches the columns with data");
            return;
        }
    }

    expect(true, "decimation matches the brute-force reference over 200 random windows");
}

// --------------------------------------------------------------- degenerate in

void testZeroColumnsProducesNothing()
{
    const scope::SampleHistory history = historyOf({{0.0, 1.0}, {1.0, 2.0}});
    std::vector<scope::ColumnStats> out;
    expect(scope::decimateMinMax(history, 0.0, 1.0, 0, out) == 0,
           "zero columns fills nothing, as during a zero-width layout pass");
    expect(out.empty(), "zero columns produces an empty output");
}

void testAnEmptyHistoryProducesNoData()
{
    scope::SampleHistory history(16);
    std::vector<scope::ColumnStats> out;
    expect(scope::decimateMinMax(history, 0.0, 10.0, 100, out) == 0,
           "an empty history fills no columns");
    expect(out.size() == 100, "an empty history still produces one entry per column");
    expect(std::none_of(out.begin(), out.end(),
                        [](const scope::ColumnStats& s) { return s.has_data; }),
           "no column claims data when there is none");
}

void testADegenerateWindowProducesNoData()
{
    const scope::SampleHistory history = historyOf({{0.0, 1.0}, {1.0, 2.0}});
    std::vector<scope::ColumnStats> out;

    expect(scope::decimateMinMax(history, 5.0, 5.0, 10, out) == 0,
           "a zero-width window fills nothing");
    expect(scope::decimateMinMax(history, 5.0, 1.0, 10, out) == 0,
           "a backwards window fills nothing rather than indexing wildly");
}

void testAWindowEntirelyBeforeOrAfterTheDataIsEmpty()
{
    const scope::SampleHistory history = historyOf({{10.0, 1.0}, {11.0, 2.0}, {12.0, 3.0}});
    std::vector<scope::ColumnStats> out;

    expect(scope::decimateMinMax(history, 0.0, 5.0, 50, out) == 0,
           "a window entirely before the data fills nothing");
    expect(scope::decimateMinMax(history, 100.0, 200.0, 50, out) == 0,
           "a window entirely after the data fills nothing");
}

// ------------------------------------------------------------------ the shape

void testFewerPointsThanColumnsLeavesGaps()
{
    // The normal state of a live plot on a slow signal: most columns are empty
    // and the renderer joins across them.
    const scope::SampleHistory history = historyOf({{0.0, 1.0}, {5.0, 2.0}, {10.0, 3.0}});
    std::vector<scope::ColumnStats> out;

    const std::size_t filled = scope::decimateMinMax(history, 0.0, 10.0, 100, out);
    expect(filled == 3, "three samples fill exactly three of a hundred columns");
    expect(out[0].has_data && out[0].first == 1.0, "the first sample lands in the first column");
    expect(out[99].has_data && out[99].last == 3.0,
           "a sample exactly at the right edge lands in the last column, not past it");
}

void testMorePointsThanColumnsKeepsTheExtremes()
{
    // The case decimation exists for, and the one a naive "every Nth sample"
    // gets wrong: the spike must survive.
    std::vector<scope::Sample> samples;
    for (int i = 0; i < 1000; ++i)
    {
        samples.push_back({static_cast<double>(i) * 0.001, 0.0});
    }
    samples[437].v = 999.0;   // A spike...
    samples[438].v = -999.0;  // ...and a matching dip, in the same column.

    const scope::SampleHistory history = historyOf(samples);
    std::vector<scope::ColumnStats> out;
    scope::decimateMinMax(history, 0.0, 1.0, 10, out);

    double overall_max = -std::numeric_limits<double>::infinity();
    double overall_min = std::numeric_limits<double>::infinity();
    for (const scope::ColumnStats& stats : out)
    {
        if (stats.has_data)
        {
            overall_max = std::max(overall_max, stats.max);
            overall_min = std::min(overall_min, stats.min);
        }
    }

    expect(overall_max == 999.0, "a spike between column boundaries survives decimation");
    expect(overall_min == -999.0, "a dip between column boundaries survives decimation");
}

void testAllEqualValues()
{
    std::vector<scope::Sample> samples;
    for (int i = 0; i < 100; ++i)
    {
        samples.push_back({static_cast<double>(i) * 0.01, 7.0});
    }

    const scope::SampleHistory history = historyOf(samples);
    std::vector<scope::ColumnStats> out;
    scope::decimateMinMax(history, 0.0, 1.0, 20, out);

    bool all_seven = true;
    for (const scope::ColumnStats& stats : out)
    {
        if (stats.has_data && (stats.min != 7.0 || stats.max != 7.0))
        {
            all_seven = false;
        }
    }
    expect(all_seven, "a constant signal decimates to a constant, with min == max");
}

void testASinglePoint()
{
    const scope::SampleHistory history = historyOf({{5.0, 42.0}});
    std::vector<scope::ColumnStats> out;

    expect(scope::decimateMinMax(history, 0.0, 10.0, 10, out) == 1,
           "a single sample fills exactly one column");
    expect(out[5].has_data && out[5].min == 42.0 && out[5].max == 42.0 && out[5].first == 42.0 &&
               out[5].last == 42.0,
           "a single sample reports the same value as min, max, first and last");
}

void testFirstAndLastFollowTimeOrder()
{
    // first/last are what join a column to its neighbours, so getting them
    // backwards draws the trace with a zigzag that is not in the data.
    const scope::SampleHistory history =
        historyOf({{0.0, 1.0}, {0.1, 5.0}, {0.2, 3.0}, {0.3, 9.0}});
    std::vector<scope::ColumnStats> out;
    scope::decimateMinMax(history, 0.0, 1.0, 1, out);

    expect(out[0].has_data, "a single column collects everything in the window");
    expect(out[0].first == 1.0, "first is the earliest value in the column");
    expect(out[0].last == 9.0, "last is the latest value in the column");
    expect(out[0].min == 1.0 && out[0].max == 9.0, "min and max span the column's values");
}

void testTheOutputVectorIsFullyOverwritten()
{
    // The caller keeps one vector across frames so a 30 Hz redraw does not
    // allocate. Leftovers from the previous frame would draw stale data.
    const scope::SampleHistory history = historyOf({{0.0, 1.0}});
    std::vector<scope::ColumnStats> out;

    scope::decimateMinMax(history, 0.0, 1.0, 10, out);
    expect(out[0].has_data, "the first pass fills a column");

    scope::SampleHistory empty(4);
    scope::decimateMinMax(empty, 0.0, 1.0, 10, out);
    expect(std::none_of(out.begin(), out.end(),
                        [](const scope::ColumnStats& s) { return s.has_data; }),
           "a reused output vector carries nothing over from the previous frame");
}

void testWorksOnAWrappedHistory()
{
    // The history is a ring, so a long-running plot always has a wrapped one.
    scope::SampleHistory history(4);
    for (int i = 0; i < 20; ++i)
    {
        history.append({static_cast<double>(i), static_cast<double>(i)});
    }
    // Retains t = 16..19.

    std::vector<scope::ColumnStats> out;
    const std::size_t filled = scope::decimateMinMax(history, 16.0, 19.0, 4, out);

    expect(filled == 4, "every retained sample is found in a wrapped history");
    expect(out[0].first == 16.0, "the oldest retained sample lands in the first column");
    expect(out[3].last == 19.0, "the newest retained sample lands in the last column");
}

}  // namespace

int main()
{
    testAgainstTheReferenceOverRandomData();

    testZeroColumnsProducesNothing();
    testAnEmptyHistoryProducesNoData();
    testADegenerateWindowProducesNoData();
    testAWindowEntirelyBeforeOrAfterTheDataIsEmpty();

    testFewerPointsThanColumnsLeavesGaps();
    testMorePointsThanColumnsKeepsTheExtremes();
    testAllEqualValues();
    testASinglePoint();
    testFirstAndLastFollowTimeOrder();
    testTheOutputVectorIsFullyOverwritten();
    testWorksOnAWrappedHistory();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
