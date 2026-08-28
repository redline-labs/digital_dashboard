// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning two signals into a line on a map, and finding a point on it.
//
// EVERY CASE HERE DRAWS SOMETHING WHEN IT IS WRONG, which is why it is a unit
// test and not a screenshot:
//
//   * pair by INDEX instead of by timestamp and the track skews the moment one
//     binding is added after the other, or one sample is dropped. The line is
//     still a line, over the wrong roads.
//   * interpolate the colour signal and a corner is painted a speed nothing
//     ever published.
//   * thin wrongly and either the path costs a thousand segments per pixel or
//     it visibly corners.
//   * hit-test wrongly and clicking the track seeks somewhere else, which reads
//     as a mis-aimed mouse rather than as a bug.
//
// No window, no GPU, no bus.

#include "map_panel/track_builder.h"

#include <spdlog/spdlog.h>

#include <cstdio>
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

bool close(double a, double b, double tolerance = 1e-9)
{
    return std::abs(a - b) <= tolerance;
}

scope::SampleHistory historyOf(const std::vector<scope::Sample>& samples)
{
    scope::SampleHistory history(samples.size() + 1);
    for (const scope::Sample& sample : samples)
    {
        history.append(sample);
    }
    return history;
}

// Irvine, and a point about 300 m east of it, so screen distances at a street
// zoom are the sort of number the panel actually deals in.
constexpr double kLat = 33.6865966;
constexpr double kLon = -117.8557874;

map_render::Projection projectionAt(double zoom = 14.0)
{
    const map_render::Camera camera{map_render::Coordinate{kLat, kLon}, zoom, 0.0};
    return map_render::Projection(camera, 800.0, 600.0, 1.0);
}

// ------------------------------------------------------------------ pairing

void testLatitudeAndLongitudePairByTimestamp()
{
    const auto lat = historyOf({{1.0, kLat}, {2.0, kLat + 0.001}, {3.0, kLat + 0.002}});
    const auto lon = historyOf({{1.0, kLon}, {2.0, kLon + 0.001}, {3.0, kLon + 0.002}});

    std::vector<scope::track::Point> out;
    const auto stats = scope::track::build(lat, lon, nullptr, out);

    expect(stats.paired == 3, "three fixes pair");
    expect(stats.unpaired_latitude == 0 && stats.unpaired_longitude == 0, "and nothing is left over");
    expect(out.size() == 3, "and three points come out");
    expect(close(out[0].t, 1.0) && close(out[2].t, 3.0), "carrying their own timestamps");
}

// THE ONE THAT MATTERS. Pairing by index would happily match latitude[1] with
// longitude[1] here and produce a track through a position that never existed.
void testAMissingSampleDoesNotSkewEverythingAfterIt()
{
    // The longitude at t=2 never arrived.
    const auto lat = historyOf({{1.0, 10.0}, {2.0, 20.0}, {3.0, 30.0}});
    const auto lon = historyOf({{1.0, 11.0}, {3.0, 33.0}});

    std::vector<scope::track::Point> out;
    const auto stats = scope::track::build(lat, lon, nullptr, out);

    expect(stats.paired == 2, "only the instants that have both are paired");
    expect(stats.unpaired_latitude == 1, "and the orphaned latitude is counted");
    expect(stats.unpaired_longitude == 0, "with no orphaned longitudes");
    expect(out.size() == 2 && close(out[1].t, 3.0),
           "the surviving pair keeps ITS OWN time -- index pairing would put t=3's "
           "longitude against t=2's latitude and draw a position nothing reported");
}

// Two signals on DIFFERENT topics never share a message, so they never share a
// timestamp. This is the case that must be visible in the counters, because on
// screen it is an empty map and looks exactly like no data at all.
void testTwoTopicsThatNeverShareATimestampPairNothing()
{
    const auto lat = historyOf({{1.00, 10.0}, {1.10, 11.0}, {1.20, 12.0}});
    const auto lon = historyOf({{1.05, 20.0}, {1.15, 21.0}, {1.25, 22.0}});

    std::vector<scope::track::Point> out;
    const auto stats = scope::track::build(lat, lon, nullptr, out);

    expect(stats.paired == 0, "nothing pairs");
    expect(out.empty(), "and the track is empty");
    expect(stats.unpaired_latitude == 3 && stats.unpaired_longitude == 3,
           "but BOTH counts are non-zero, which is what says the two are not on one topic");
}

void testUnevenTailsAreCounted()
{
    const auto lat = historyOf({{1.0, 10.0}, {2.0, 20.0}, {3.0, 30.0}, {4.0, 40.0}});
    const auto lon = historyOf({{1.0, 11.0}, {2.0, 22.0}});

    std::vector<scope::track::Point> out;
    const auto stats = scope::track::build(lat, lon, nullptr, out);

    expect(stats.paired == 2, "the overlapping prefix pairs");
    expect(stats.unpaired_latitude == 2, "and the latitudes past the end are counted");
}

void testEmptyInputsProduceAnEmptyTrack()
{
    const auto empty = historyOf({});
    const auto some = historyOf({{1.0, 10.0}});

    std::vector<scope::track::Point> out;
    expect(scope::track::build(empty, some, nullptr, out).paired == 0, "no latitudes, no pairs");
    expect(out.empty(), "and no points");

    const auto stats = scope::track::build(some, empty, nullptr, out);
    expect(stats.unpaired_latitude == 1, "the lone latitude is counted as unpaired");
}

// ------------------------------------------------------------------- colour

// Zero-order hold, never interpolated: a value between two samples is a number
// nothing published, and on a corner it would paint a speed the car never did.
void testColourIsHeldNotInterpolated()
{
    const auto lat = historyOf({{1.0, 10.0}, {2.0, 11.0}, {3.0, 12.0}});
    const auto lon = historyOf({{1.0, 20.0}, {2.0, 21.0}, {3.0, 22.0}});
    // Speed changes only at t=1 and t=3.
    const auto speed = historyOf({{1.0, 100.0}, {3.0, 200.0}});

    std::vector<scope::track::Point> out;
    scope::track::build(lat, lon, &speed, out);

    expect(out.size() == 3, "three points");
    expect(out[0].has_color && close(out[0].color, 100.0), "the first takes the sample at its time");
    expect(out[1].has_color && close(out[1].color, 100.0),
           "the middle HOLDS the earlier value -- interpolating would give 150, which "
           "nothing published");
    expect(out[2].has_color && close(out[2].color, 200.0), "and the last takes the newer one");
}

void testAColourSampleAfterAPointDoesNotReachBackwards()
{
    const auto lat = historyOf({{1.0, 10.0}});
    const auto lon = historyOf({{1.0, 20.0}});
    const auto speed = historyOf({{5.0, 100.0}});

    std::vector<scope::track::Point> out;
    scope::track::build(lat, lon, &speed, out);

    expect(out.size() == 1, "one point");
    expect(!out[0].has_color,
           "with no colour: the only sample is in its future, and holding a future value "
           "backwards would colour a corner by what happened after it");
}

void testNoColourSignalLeavesEveryPointUncoloured()
{
    const auto lat = historyOf({{1.0, 10.0}});
    const auto lon = historyOf({{1.0, 20.0}});

    std::vector<scope::track::Point> out;
    scope::track::build(lat, lon, nullptr, out);
    expect(out.size() == 1 && !out[0].has_color, "no colour signal, no colour");
}

// ------------------------------------------------------------------ thinning

void testThinningDropsPointsSharingAPixelAndKeepsTheEnds()
{
    const auto projection = projectionAt(14.0);

    // A hundred fixes over about a metre of ground: at z14 they all land on the
    // same pixel.
    std::vector<scope::track::Point> dense;
    for (int i = 0; i < 100; ++i)
    {
        scope::track::Point point;
        point.t = double(i);
        point.world = map_render::worldFor(
            map_render::Coordinate{kLat + (double(i) * 1e-8), kLon});
        dense.push_back(point);
    }

    std::vector<scope::track::Point> thinned;
    scope::track::thin(dense, projection, 1.5, thinned);

    expect(thinned.size() < dense.size(), "a stationary hundred is thinned");
    expect(thinned.size() == 2, "down to just the two ends");
    expect(close(thinned.front().t, 0.0), "the first point is kept");
    expect(close(thinned.back().t, 99.0),
           "and so is the LAST -- dropping it moves where the vehicle ended up, which at "
           "a low zoom is kilometres");
}

void testThinningKeepsPointsThatAreFarApart()
{
    const auto projection = projectionAt(14.0);

    std::vector<scope::track::Point> spread;
    for (int i = 0; i < 10; ++i)
    {
        scope::track::Point point;
        point.t = double(i);
        point.world =
            map_render::worldFor(map_render::Coordinate{kLat + (double(i) * 0.001), kLon});
        spread.push_back(point);
    }

    std::vector<scope::track::Point> thinned;
    scope::track::thin(spread, projection, 1.5, thinned);
    expect(thinned.size() == spread.size(), "well-separated points all survive");
}

void testThinningHandlesDegenerateInputs()
{
    const auto projection = projectionAt();
    std::vector<scope::track::Point> thinned;

    scope::track::thin({}, projection, 1.5, thinned);
    expect(thinned.empty(), "thinning nothing gives nothing");

    std::vector<scope::track::Point> one(1);
    one[0].world = map_render::worldFor(map_render::Coordinate{kLat, kLon});
    scope::track::thin(one, projection, 1.5, thinned);
    expect(thinned.size() == 1, "a single point survives rather than being dropped as 'the end'");
}

// ------------------------------------------------------------------ hit test

void testNearestFindsThePointUnderThePointer()
{
    const auto projection = projectionAt(14.0);

    std::vector<scope::track::Point> points;
    for (int i = 0; i < 5; ++i)
    {
        scope::track::Point point;
        point.t = double(i);
        point.world =
            map_render::worldFor(map_render::Coordinate{kLat, kLon + (double(i) * 0.002)});
        points.push_back(point);
    }

    const map_render::ScreenPoint target = projection.screenFor(points[3].world);
    const auto hit = scope::track::nearest(points, projection, target, 12.0);
    expect(hit.has_value() && *hit == 3, "a click on a point finds that point");

    // Far away in screen space.
    const auto miss = scope::track::nearest(
        points, projection, map_render::ScreenPoint{target.x + 400.0, target.y + 300.0}, 12.0);
    expect(!miss.has_value(), "a click away from the track finds nothing, so the map pans");
}

// A lap crosses itself, so two points sit under the pointer. Picking whichever
// came first in the buffer seeks to the wrong lap -- and both are on the track,
// so nothing about the result looks wrong.
void testNearestPicksTheNearestNotTheFirst()
{
    const auto projection = projectionAt(14.0);

    std::vector<scope::track::Point> points;
    // t=0 is 20 m away from the pointer; t=1 is right under it.
    scope::track::Point far_point;
    far_point.t = 0.0;
    far_point.world = map_render::worldFor(map_render::Coordinate{kLat + 0.0004, kLon});
    points.push_back(far_point);

    scope::track::Point near_point;
    near_point.t = 1.0;
    near_point.world = map_render::worldFor(map_render::Coordinate{kLat, kLon});
    points.push_back(near_point);

    const auto hit = scope::track::nearest(points, projection,
                                           projection.screenFor(near_point.world), 40.0);
    expect(hit.has_value() && *hit == 1,
           "the NEARER point wins even though the other is within the radius and came first");
}

void testNearestOnAnEmptyTrackFindsNothing()
{
    const auto projection = projectionAt();
    expect(!scope::track::nearest({}, projection, map_render::ScreenPoint{10.0, 10.0}, 12.0)
                .has_value(),
           "an empty track is not clickable");
}

// ---------------------------------------------------------------- lookup by t

void testAtFindsTheSampleAtOrBeforeAnInstant()
{
    std::vector<scope::track::Point> points(3);
    points[0].t = 1.0;
    points[1].t = 2.0;
    points[2].t = 3.0;

    expect(!scope::track::at(points, 0.5).has_value(), "before the track starts there is nothing");
    expect(scope::track::at(points, 1.0) == std::size_t{0}, "exactly on a point finds it");
    expect(scope::track::at(points, 2.5) == std::size_t{1},
           "between two points HOLDS the earlier one -- a position between two fixes is "
           "somewhere nothing reported being");
    expect(scope::track::at(points, 99.0) == std::size_t{2},
           "past the end holds the last known position rather than losing the marker");
    expect(!scope::track::at({}, 1.0).has_value(), "an empty track has no marker");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testLatitudeAndLongitudePairByTimestamp();
    testAMissingSampleDoesNotSkewEverythingAfterIt();
    testTwoTopicsThatNeverShareATimestampPairNothing();
    testUnevenTailsAreCounted();
    testEmptyInputsProduceAnEmptyTrack();

    testColourIsHeldNotInterpolated();
    testAColourSampleAfterAPointDoesNotReachBackwards();
    testNoColourSignalLeavesEveryPointUncoloured();

    testThinningDropsPointsSharingAPixelAndKeepsTheEnds();
    testThinningKeepsPointsThatAreFarApart();
    testThinningHandlesDegenerateInputs();

    testNearestFindsThePointUnderThePointer();
    testNearestPicksTheNearestNotTheFirst();
    testNearestOnAnEmptyTrackFindsNothing();

    testAtFindsTheSampleAtOrBeforeAnInstant();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
