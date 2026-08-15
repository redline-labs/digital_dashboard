// SPDX-License-Identifier: GPL-3.0-or-later
//
// Timing the router, because stage 6's choice is supposed to rest on numbers.
//
// A* on a real graph is fast until it isn't, and the point where it stops being
// fast is a property of the DATA, not of the algorithm: the search expands an
// ellipse whose area grows with the square of the trip length, and the number of
// nodes in that ellipse depends on how dense the road network is inside it. No
// amount of reasoning substitutes for running it.
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <cmath>
#include <array>
#include <numbers>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "cli/output.h"
#include "map_build/verbs.h"
#include "road_graph/graph.h"
#include "road_graph/overlay.h"
#include "road_graph/search.h"

namespace map_build
{
namespace
{

constexpr double kCoordScale = 1e-7;
constexpr double kEarthRadiusM = 6371000.0;

double haversineM(road_graph::Coord aLat, road_graph::Coord aLon, road_graph::Coord bLat,
                  road_graph::Coord bLon)
{
    const double toRad = std::numbers::pi / 180.0;
    const double lat1 = static_cast<double>(aLat) * kCoordScale * toRad;
    const double lat2 = static_cast<double>(bLat) * kCoordScale * toRad;
    const double dLat = lat2 - lat1;
    const double dLon = (static_cast<double>(bLon) - static_cast<double>(aLon)) * kCoordScale * toRad;
    const double h = std::sin(dLat / 2) * std::sin(dLat / 2) +
                     std::cos(lat1) * std::cos(lat2) * std::sin(dLon / 2) * std::sin(dLon / 2);
    return 2.0 * kEarthRadiusM * std::asin(std::min(1.0, std::sqrt(h)));
}

// A deterministic sequence. Not std::mt19937 seeded from a clock: the whole
// value of a benchmark is that two runs are comparable, and a run whose
// origin-destination pairs differ each time measures the map, not the change.
struct Lcg
{
    std::uint64_t state;

    std::uint64_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state >> 16;
    }

    std::size_t below(std::size_t bound) { return bound == 0 ? 0 : next() % bound; }
};

struct Sample
{
    double straightKm { 0.0 };
    double routeKm { 0.0 };
    double minutes { 0.0 };
    double millis { 0.0 };
    // The overlay's time for the same pair, when there is an overlay.
    double overlayMillis { 0.0 };
    bool overlayFound { false };
    // Absolute difference in cost between the two routers. This is the number
    // that decides whether the overlay may be trusted; a speedup that came from
    // answering a different question is not a speedup.
    double deltaS { 0.0 };
    bool found { false };
};

void report(const std::string& label, std::vector<Sample>& samples, bool withOverlay)
{
    auto found = std::vector<Sample> {};
    for (const Sample& sample : samples)
    {
        if (sample.found)
        {
            found.push_back(sample);
        }
    }
    if (found.empty())
    {
        cli::out("  {:<12} no route found in {} attempt(s)\n", label, samples.size());
        return;
    }

    std::sort(found.begin(), found.end(),
              [](const Sample& a, const Sample& b) { return a.millis < b.millis; });

    double km = 0.0;
    for (const Sample& sample : found)
    {
        km += sample.routeKm;
    }

    // The MEDIAN and the worst case, not the mean. A router is judged by the
    // query that makes someone wait, and a mean over a handful of samples hides
    // exactly that one.
    const Sample& median = found[found.size() / 2];
    const Sample& worst = found.back();
    cli::out("  {:<12} n={:<3} route {:>6.1f} km  median {:>8.1f} ms  worst {:>8.1f} ms", label,
             found.size(), km / static_cast<double>(found.size()), median.millis, worst.millis);

    if (!withOverlay)
    {
        cli::out("\n");
        return;
    }

    std::vector<Sample> byOverlay = found;
    std::sort(byOverlay.begin(), byOverlay.end(),
              [](const Sample& a, const Sample& b) { return a.overlayMillis < b.overlayMillis; });
    double worstDelta = 0.0;
    std::size_t missing = 0;
    for (const Sample& sample : found)
    {
        worstDelta = std::max(worstDelta, sample.deltaS);
        if (!sample.overlayFound)
        {
            ++missing;
        }
    }
    cli::out("   |  overlay median {:>7.2f} ms  worst {:>7.2f} ms", byOverlay[byOverlay.size() / 2].overlayMillis,
             byOverlay.back().overlayMillis);
    if (missing != 0)
    {
        cli::out("  [{} NOT FOUND by the overlay]", missing);
    }
    if (worstDelta > 0.05)
    {
        // Loud, because it means the two routers disagree about the answer and
        // the timing above is comparing different questions.
        cli::out("  [WORST COST DIFFERENCE {:.2f} s]", worstDelta);
    }
    cli::out("\n");
}

} // namespace

void addRouteOptions(cxxopts::Options& options)
{
    options.add_options()("g,graph", "Graph file to read.", cxxopts::value<std::string>())(
        "samples", "Origin/destination pairs per distance band.",
        cxxopts::value<std::uint64_t>()->default_value("12"))(
        "seed", "Seed for the pair generator; the same seed gives the same pairs.",
        cxxopts::value<std::uint64_t>()->default_value("1"))(
        "overlay", "Overlay to compare against. Defaults to <graph>.overlay when present.",
        cxxopts::value<std::string>());
}

int runRoute(cli::Context& context)
{
    auto path = context.requireString("graph");
    if (!path)
    {
        return cli::kUsage;
    }

    auto graph = road_graph::Graph::open(*path);
    if (!graph)
    {
        SPDLOG_ERROR("{}", road_graph::to_string(graph.error()));
        return cli::kFailure;
    }

    const auto nodes = graph->nodes();
    if (nodes.empty())
    {
        SPDLOG_ERROR("the graph has no junctions");
        return cli::kFailure;
    }

    cli::out("graph      {}\n", *path);
    cli::out("junctions  {}\n", nodes.size());
    cli::out("segments   {}\n", graph->segments().size());
    cli::out("edges      {}\n", graph->edges().size());
    cli::out("\n");

    // Bands, in straight-line kilometres. The interesting question is not "how
    // fast is one query" but "where does the curve bend", which is why this
    // sweeps rather than timing a single pair.
    struct Band
    {
        const char* label;
        double lowKm;
        double highKm;
    };
    constexpr std::array<Band, 6> kBands { {
        { "0-2 km", 0.0, 2.0 },
        { "2-10 km", 2.0, 10.0 },
        { "10-30 km", 10.0, 30.0 },
        { "30-80 km", 30.0, 80.0 },
        { "80-200 km", 80.0, 200.0 },
        { "200+ km", 200.0, 100000.0 },
    } };

    const auto samplesPerBand = static_cast<std::size_t>(context.uintOr("samples", 12));
    Lcg lcg { context.uintOr("seed", 1) };

    // The overlay, when there is one. Compared rather than trusted: every pair
    // is routed both ways and the COSTS are checked, because a hierarchy that
    // returns a different route quickly is not an improvement.
    std::filesystem::path overlayPath = context.stringOr("overlay", "");
    if (overlayPath.empty())
    {
        overlayPath = std::filesystem::path(*path);
        overlayPath += ".overlay";
    }
    std::optional<road_graph::Overlay> overlay;
    if (std::filesystem::exists(overlayPath))
    {
        auto opened = road_graph::Overlay::open(overlayPath, *graph);
        if (!opened)
        {
            SPDLOG_ERROR("overlay at {}: {}", overlayPath.string(),
                         road_graph::to_string(opened.error()));
            return cli::kFailure;
        }
        overlay = std::move(*opened);
        cli::out("overlay    {} ({} shortcuts over {} transitions)\n", overlayPath.string(),
                 overlay->header().shortcutCount, overlay->header().originalArcCount);
        cli::out("\n");
    }

    cli::out("timings by straight-line distance:\n");

    for (const Band& band : kBands)
    {
        std::vector<Sample> samples;
        // Bounded tries, so a band with no qualifying pairs (a 200 km trip
        // inside a city-sized extract) reports that rather than spinning.
        const std::size_t maxTries = samplesPerBand * 400;
        for (std::size_t tries = 0; tries < maxTries && samples.size() < samplesPerBand; ++tries)
        {
            const auto from = static_cast<road_graph::NodeIndex>(lcg.below(nodes.size()));
            const auto to = static_cast<road_graph::NodeIndex>(lcg.below(nodes.size()));
            if (from == to)
            {
                continue;
            }
            const double straightKm =
                haversineM(nodes[from].lat, nodes[from].lon, nodes[to].lat, nodes[to].lon) / 1000.0;
            if (straightKm < band.lowKm || straightKm >= band.highKm)
            {
                continue;
            }

            Sample sample;
            sample.straightKm = straightKm;

            const auto started = std::chrono::steady_clock::now();
            auto route = road_graph::findRoute(*graph, from, to);
            const auto elapsed = std::chrono::steady_clock::now() - started;

            sample.millis =
                std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() / 1000.0;
            if (route)
            {
                sample.found = true;
                sample.routeKm = route->distanceM / 1000.0;
                sample.minutes = route->durationS / 60.0;
            }
            if (overlay)
            {
                const auto overlayStarted = std::chrono::steady_clock::now();
                auto viaOverlay = road_graph::findRouteVia(*graph, *overlay, from, to);
                const auto overlayElapsed = std::chrono::steady_clock::now() - overlayStarted;
                sample.overlayMillis =
                    std::chrono::duration_cast<std::chrono::microseconds>(overlayElapsed).count() /
                    1000.0;
                sample.overlayFound = viaOverlay.has_value();
                if (route && viaOverlay)
                {
                    sample.deltaS = std::abs(route->durationS - viaOverlay->durationS);
                }
            }

            samples.push_back(sample);
        }

        if (samples.empty())
        {
            cli::out("  {:<12} no pairs in this band\n", band.label);
            continue;
        }
        report(band.label, samples, overlay.has_value());
    }

    return cli::kOk;
}

} // namespace map_build
