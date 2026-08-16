// SPDX-License-Identifier: GPL-3.0-or-later
//
// The matcher and the horizon, driven over a synthetic drive.
//
// Every case here is one where the matcher is CONFIDENTLY WRONG rather than
// broken, which is the only kind of failure that matters in something whose
// output is a road name on a dash:
//
//   - a fix between a freeway and its frontage road, decided by heading;
//   - a stationary vehicle whose course over ground is noise, which must not
//     be re-matched every fix while it sits at a light;
//   - an RTK sigma of two centimetres, which must not be believed so
//     completely that no road is close enough to match at all;
//   - a jump in the stream, after which carrying the old beam forward would
//     explain the jump as a very long detour.

#include <cmath>
#include <filesystem>
#include <type_traits>
#include <atomic>
#include <thread>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "road_graph/builder.h"
#include "road_graph/graph.h"

#include "horizon.h"
#include "matcher.h"

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

constexpr road_graph::Coord kLat = 336865966;
constexpr road_graph::Coord kLon = -1178557874;
// Roughly 11 m of latitude at 1e-7 degrees.
constexpr road_graph::Coord kStep = 1000;

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

map_rules::RoadClassification roadOf(map_rules::RouteClass routeClass, std::uint16_t speedKph)
{
    map_rules::RoadClassification out;
    out.renderClass = map_rules::RenderClass::Minor;
    out.routeClass = routeClass;
    out.access = map_rules::kAccessMotorcar;
    out.freeFlowSpeedKph = speedKph;
    return out;
}

// A straight north-south road of `points` steps, starting at (lat, lon).
road_graph::Builder::SegmentInput line(std::int64_t wayId, std::uint32_t ordinal,
                                       std::int64_t fromNode, std::int64_t toNode,
                                       road_graph::Coord lat, road_graph::Coord lon, int points,
                                       const std::string& name,
                                       map_rules::RouteClass routeClass = map_rules::RouteClass::Minor)
{
    road_graph::Builder::SegmentInput input;
    input.id = road_graph::makeSegmentId(wayId, ordinal);
    input.osmWayId = wayId;
    input.fromNodeId = fromNode;
    input.toNodeId = toNode;
    for (int i = 0; i < points; ++i)
    {
        input.geometry.push_back(lat + i * kStep);
        input.geometry.push_back(lon);
    }
    input.classification = roadOf(routeClass, 50);
    input.name = name;
    return input;
}

// A graph with one long road built from three joined segments, so the horizon
// has something to follow.
std::filesystem::path buildStraightRoad(const std::string& name)
{
    const auto path = scratch(name);
    road_graph::Builder builder;
    builder.add(line(1, 0, 1, 2, kLat, kLon, 6, "Main Street"));
    builder.add(line(1, 1, 2, 3, kLat + 5 * kStep, kLon, 6, "Main Street"));
    builder.add(line(1, 2, 3, 4, kLat + 10 * kStep, kLon, 6, "Main Street"));
    builder.write(path, 0);
    return path;
}

void test_a_drive_along_a_road_stays_on_it()
{
    const auto path = buildStraightRoad("mm_straight.graph");
    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    map_match::Matcher matcher(*graph, {});

    road_graph::SegmentIndex first = road_graph::kNoSegment;
    int matched = 0;
    for (int i = 0; i < 12; ++i)
    {
        map_match::Fix fix;
        fix.lat = kLat + i * kStep;
        fix.lon = kLon;
        fix.headingDeg = 0.0;
        fix.speedMps = 12.0;
        fix.sigmaM = 0.02;  // RTK fixed

        const auto result = matcher.update(fix);
        if (result.matched)
        {
            ++matched;
            if (first == road_graph::kNoSegment)
            {
                first = result.segment;
            }
            const auto& segment = graph->segments()[result.segment];
            check(graph->nameOf(segment) == "Main Street", "every fix matches the road");
        }
    }

    check(matched == 12, "all twelve fixes match");
    check(matcher.counts().unmatched == 0, "and none is lost");

    std::filesystem::remove(path);
}

void test_an_rtk_sigma_is_not_believed_completely()
{
    // THE calibration case. An RTK-fixed BD992 reports two centimetres. OSM
    // centrelines are metres from the real road, so a matcher that believed the
    // receiver would find every candidate impossible and match NOTHING -- which
    // presents as "the road name is blank" rather than as a tuning problem.
    const auto path = buildStraightRoad("mm_rtk.graph");
    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    map_match::Matcher matcher(*graph, {});

    map_match::Fix fix;
    // Eight metres to the side of the road -- well within OSM's own error, and
    // four hundred sigmas away if the receiver were believed.
    fix.lat = kLat + 3 * kStep;
    fix.lon = kLon + 900;
    fix.headingDeg = 0.0;
    fix.speedMps = 12.0;
    fix.sigmaM = 0.02;

    const auto result = matcher.update(fix);
    check(result.matched, "a fix eight metres off the centreline still matches");
    check(result.sigmaUsedM >= 4.0,
          "because the emission width is floored by the MAP's accuracy, not the receiver's");

    std::filesystem::remove(path);
}

void test_a_poor_fix_widens_the_search()
{
    // The other end. A receiver coasting on a lost correction link reports
    // metres; a radius that made sense at two centimetres finds nothing.
    const auto path = buildStraightRoad("mm_poor.graph");
    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    map_match::MatcherConfig config;
    config.searchRadiusM = 10.0;
    map_match::Matcher matcher(*graph, config);

    map_match::Fix fix;
    fix.lat = kLat + 3 * kStep;
    fix.lon = kLon + 2500;  // about 23 m off
    fix.speedMps = 12.0;
    fix.headingDeg = 0.0;
    fix.sigmaM = 20.0;

    const auto result = matcher.update(fix);
    check(result.matched, "a 23 m error with a 20 m sigma still matches");
    check(result.sigmaUsedM > 10.0, "with a wide emission");

    std::filesystem::remove(path);
}

void test_heading_decides_between_parallel_roads()
{
    // A freeway and its frontage road. A fix between them is nearer whichever
    // way the noise fell, and heading is the only thing that separates them.
    const auto path = scratch("mm_parallel.graph");

    road_graph::Builder builder;
    // Northbound and southbound carriageways, 22 m apart, same class. ONEWAY,
    // as the two halves of a divided highway really are -- and that is the only
    // thing that can separate them: they are parallel, the same distance away,
    // and a heading agrees with one of them just as well in reverse.
    auto north = line(1, 0, 1, 2, kLat, kLon, 8, "Northbound");
    north.classification.onewayForward = true;
    builder.add(std::move(north));

    road_graph::Builder::SegmentInput south;
    south.id = road_graph::makeSegmentId(2, 0);
    south.osmWayId = 2;
    south.fromNodeId = 3;
    south.toNodeId = 4;
    for (int i = 7; i >= 0; --i)
    {
        south.geometry.push_back(kLat + i * kStep);
        south.geometry.push_back(kLon + 2 * kStep);
    }
    south.classification = roadOf(map_rules::RouteClass::Minor, 50);
    south.classification.onewayForward = true;
    south.name = "Southbound";
    builder.add(std::move(south));

    builder.write(path, 0);

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    map_match::Fix fix;
    fix.lat = kLat + 4 * kStep;
    fix.lon = kLon + kStep;  // exactly between them
    fix.speedMps = 25.0;
    fix.sigmaM = 0.02;

    {
        map_match::Matcher matcher(*graph, {});
        fix.headingDeg = 0.0;  // north
        const auto result = matcher.update(fix);
        check(result.matched, "travelling north matches");
        if (result.matched)
        {
            check(graph->nameOf(graph->segments()[result.segment]) == "Northbound",
                  "and picks the northbound carriageway");
        }
    }

    {
        map_match::Matcher matcher(*graph, {});
        fix.headingDeg = 180.0;  // south
        const auto result = matcher.update(fix);
        check(result.matched, "travelling south matches");
        if (result.matched)
        {
            check(graph->nameOf(graph->segments()[result.segment]) == "Southbound",
                  "and picks the OTHER carriageway, from the same position");
        }
    }

    std::filesystem::remove(path);
}

void test_a_stationary_vehicle_is_not_re_matched_on_noise()
{
    // A car at a light. Its course over ground wanders through all 360 degrees,
    // and feeding that in would put it on a different road every fix -- which a
    // driver sees as the road name flickering while they sit still.
    const auto path = scratch("mm_stationary.graph");

    road_graph::Builder builder;
    builder.add(line(1, 0, 1, 2, kLat, kLon, 8, "Northbound"));
    builder.add(line(2, 0, 3, 4, kLat, kLon + 2 * kStep, 8, "Parallel"));
    builder.write(path, 0);

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    map_match::Matcher matcher(*graph, {});

    road_graph::SegmentIndex settled = road_graph::kNoSegment;
    int changes = 0;

    for (int i = 0; i < 20; ++i)
    {
        map_match::Fix fix;
        fix.lat = kLat + 4 * kStep;
        fix.lon = kLon + 200;  // sitting near the first road
        // Heading spinning through every direction, as a stationary receiver's
        // course over ground really does.
        fix.headingDeg = i * 37.0;
        fix.speedMps = 0.1;  // stopped
        fix.sigmaM = 0.02;

        const auto result = matcher.update(fix);
        if (!result.matched)
        {
            continue;
        }
        if (settled == road_graph::kNoSegment)
        {
            settled = result.segment;
        }
        else if (result.segment != settled)
        {
            ++changes;
            settled = result.segment;
        }
    }

    check(changes == 0, "a stationary vehicle stays on one road despite the heading spinning");

    std::filesystem::remove(path);
}

void test_a_gap_in_the_stream_resets_the_beam()
{
    // After a gap, carrying the beam forward explains a jump across town as a
    // very long detour, and the matcher is then confidently wrong until it
    // recovers on its own.
    const auto path = buildStraightRoad("mm_gap.graph");
    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    map_match::Matcher matcher(*graph, {});

    map_match::Fix fix;
    fix.lat = kLat;
    fix.lon = kLon;
    fix.speedMps = 12.0;
    fix.headingDeg = 0.0;
    fix.sigmaM = 0.02;
    check(matcher.update(fix).matched, "the first fix matches");

    const auto before = matcher.counts().resets;
    matcher.reset();
    check(matcher.counts().resets == before + 1, "reset is counted");

    // A fix somewhere else entirely. With a cleared beam this is simply a new
    // start rather than an impossible transition.
    fix.lat = kLat + 10 * kStep;
    const auto result = matcher.update(fix);
    check(result.matched, "and the next fix matches on its own merits");

    std::filesystem::remove(path);
}

void test_no_road_within_the_radius_is_reported_as_no_match()
{
    const auto path = buildStraightRoad("mm_nomatch.graph");
    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    map_match::Matcher matcher(*graph, {});

    map_match::Fix fix;
    fix.lat = kLat;
    fix.lon = kLon + 100 * kStep;  // a kilometre away
    fix.speedMps = 12.0;
    fix.sigmaM = 0.02;

    const auto result = matcher.update(fix);
    check(!result.matched, "a fix far from any road does NOT match");
    check(matcher.counts().unmatched == 1, "and is counted as unmatched, not as an error");

    std::filesystem::remove(path);
}

void test_the_horizon_follows_the_road_through_junctions()
{
    // Three segments in a line, joined end to end. The horizon must run through
    // all of them: stopping at the first junction would give a lookahead of one
    // segment, which on a freeway is a few hundred metres of a 2 km horizon.
    const auto path = buildStraightRoad("mm_horizon.graph");
    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    map_match::Matcher matcher(*graph, {});

    map_match::Fix fix;
    fix.lat = kLat;
    fix.lon = kLon;
    fix.headingDeg = 0.0;
    fix.speedMps = 12.0;
    fix.sigmaM = 0.02;

    const auto result = matcher.update(fix);
    check(result.matched, "the start of the road matches");
    if (!result.matched)
    {
        return;
    }

    const auto horizon = map_match::buildHorizon(*graph, result.segment, result.offsetCm,
                                                 result.forward, 200000);
    check(horizon.runs.size() >= 2, "the horizon runs past the first junction");
    check(horizon.lengthCm > 0, "and has a length");

    // Runs must tile the path with no gaps: a consumer reading a profile across
    // a gap interpolates a value that was never true.
    bool contiguous = true;
    for (std::size_t i = 1; i < horizon.runs.size(); ++i)
    {
        if (horizon.runs[i].startOffsetCm != horizon.runs[i - 1].endOffsetCm)
        {
            contiguous = false;
        }
    }
    check(contiguous, "and its runs tile the path with no gaps");

    std::filesystem::remove(path);
}

void test_the_horizon_stops_at_a_fork()
{
    // Guessing which way a driver will go is what branch probabilities are for.
    // A horizon that guesses wrong is worse than a short one, because a
    // consumer cannot tell the difference.
    const auto path = scratch("mm_fork.graph");

    road_graph::Builder builder;
    builder.add(line(1, 0, 1, 2, kLat, kLon, 6, "Trunk"));
    // Two roads leaving node 2.
    builder.add(line(2, 0, 2, 3, kLat + 5 * kStep, kLon, 6, "Left"));

    road_graph::Builder::SegmentInput right;
    right.id = road_graph::makeSegmentId(3, 0);
    right.osmWayId = 3;
    right.fromNodeId = 2;
    right.toNodeId = 4;
    for (int i = 0; i < 6; ++i)
    {
        right.geometry.push_back(kLat + 5 * kStep);
        right.geometry.push_back(kLon + i * kStep);
    }
    right.classification = roadOf(map_rules::RouteClass::Minor, 50);
    right.name = "Right";
    builder.add(std::move(right));

    builder.write(path, 0);

    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "graph opens");
        return;
    }

    // Find the trunk segment and build a horizon from its start.
    road_graph::SegmentIndex trunk = road_graph::kNoSegment;
    for (std::uint32_t i = 0; i < graph->header().segmentCount; ++i)
    {
        if (graph->nameOf(graph->segments()[i]) == "Trunk")
        {
            trunk = i;
        }
    }
    check(trunk != road_graph::kNoSegment, "the trunk segment is in the graph");
    if (trunk == road_graph::kNoSegment)
    {
        return;
    }

    const auto horizon = map_match::buildHorizon(*graph, trunk, 0, true, 500000);
    check(horizon.runs.size() == 1, "the horizon stops at the fork rather than guessing");

    std::filesystem::remove(path);
}

// counts() must hand back a SNAPSHOT, not a reference into live state. The
// node's status timer reads it from the main loop while update() runs on a
// zenoh RX thread; a reference there was a data race on four plain uint64s.
//
// A compile-time check, because that is the half of this that a test can pin
// deterministically -- the race itself needs a thread sanitizer to observe.
static_assert(
    std::is_same_v<decltype(std::declval<const map_match::Matcher&>().counts()),
                   map_match::Matcher::Counts>,
    "Matcher::counts() must return by value; a reference into the live counters is a race");

void test_counts_stay_monotonic_while_another_thread_reads_them()
{
    // Not a proof of thread safety -- only a sanitizer gives that -- but it
    // does catch a counter that goes backwards or lands on the wrong total,
    // which is what a torn read would look like on the status topic.
    const auto path = buildStraightRoad("mm_counts_concurrent.graph");
    auto graph = road_graph::Graph::open(path);
    if (!graph)
    {
        check(false, "the test graph opens");
        std::filesystem::remove(path);
        return;
    }

    map_match::MatcherConfig config;
    map_match::Matcher matcher(*graph, config);

    constexpr int kFixes = 2000;
    std::atomic<bool> reading { true };
    std::atomic<bool> wentBackwards { false };

    std::thread reader([&] {
        map_match::Matcher::Counts previous;
        while (reading.load(std::memory_order_relaxed))
        {
            const map_match::Matcher::Counts now = matcher.counts();
            if (now.fixes < previous.fixes || now.matched < previous.matched ||
                now.unmatched < previous.unmatched || now.resets < previous.resets)
            {
                wentBackwards.store(true, std::memory_order_relaxed);
            }
            previous = now;
        }
    });

    for (int i = 0; i < kFixes; ++i)
    {
        map_match::Fix fix;
        fix.lat = kLat;
        fix.lon = static_cast<road_graph::Coord>(kLon + i);
        matcher.update(fix);
    }

    reading.store(false, std::memory_order_relaxed);
    reader.join();

    check(!wentBackwards.load(std::memory_order_relaxed),
          "a concurrent reader never sees a counter go backwards");

    const map_match::Matcher::Counts final = matcher.counts();
    check(final.fixes == kFixes, "and every fix is counted exactly once");
    check(final.matched + final.unmatched == kFixes,
          "with each one either matched or unmatched, never both or neither");

    std::filesystem::remove(path);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_drive_along_a_road_stays_on_it();
    test_counts_stay_monotonic_while_another_thread_reads_them();
    test_an_rtk_sigma_is_not_believed_completely();
    test_a_poor_fix_widens_the_search();
    test_heading_decides_between_parallel_roads();
    test_a_stationary_vehicle_is_not_re_matched_on_noise();
    test_a_gap_in_the_stream_resets_the_beam();
    test_no_road_within_the_radius_is_reported_as_no_match();
    test_the_horizon_follows_the_road_through_junctions();
    test_the_horizon_stops_at_a_fork();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all matcher checks passed");
    return 0;
}
