// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning a track outline into a centreline.
//
// Every case here is one the real 994 files contain and that a plausible
// implementation gets wrong QUIETLY:
//
//   - the two loops are wound the SAME way as often as oppositely, and nothing
//     in the file says which. Get it wrong and the pairing is a reflection: the
//     lengths still come out right, so a length check passes, while the shape
//     is nonsense;
//   - the loops are sampled at wildly different vertex densities, so pairing by
//     index compares a hairpin against a straight;
//   - a file can hold SEVERAL loops. Splitting it at one of them yields a
//     centreline of respectable width and twice the correct length -- Monza
//     Without Chicane is exactly this, and a width check alone ships it;
//   - EVERY outline is a closed polygon, point-to-point courses included: they
//     are drawn as thin closed ribbons, up one edge and back the other. So
//     neither the feature type nor the `closed` property can say whether a
//     track is a circuit, and only the derived centreline can;
//   - the outline itself can disagree with the published lap length, which is
//     the source's problem and must not be reported as ours.
//
// The annulus is the fixture because its answers are known in closed form: a
// ring of mean radius R and width w has a centreline of exactly 2*pi*R and a
// width of exactly w everywhere.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "map_build/track_geometry.h"
#include "road_graph/geometry.h"

namespace
{

namespace track = map_build::track;

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

void checkNear(double value, double expected, double tolerance, const std::string& what)
{
    if (!(std::abs(value - expected) <= tolerance))
    {
        SPDLOG_ERROR("FAIL: {} (got {:.3f}, wanted {:.3f} +/- {:.3f})", what, value, expected,
                     tolerance);
        ++failures;
        return;
    }
}

// Somewhere with a cos(latitude) worth getting wrong. At 52 degrees a degree of
// longitude is 61 % of a degree of latitude, so an implementation that forgets
// the projection produces an "annulus" that is half as wide east-west as it is
// north-south -- and the median width still lands in a plausible range.
constexpr double kRefLat = 52.0;
constexpr double kRefLon = 6.5;
constexpr double kMetresPerDegree = std::numbers::pi * road_graph::kEarthRadiusM / 180.0;

// A point `east` metres east and `north` metres north of the reference.
void appendLocal(track::Ring& ring, double east, double north)
{
    const double lat = kRefLat + north / kMetresPerDegree;
    const double cosLat = std::cos(kRefLat * std::numbers::pi / 180.0);
    const double lon = kRefLon + east / (kMetresPerDegree * cosLat);
    ring.push_back(road_graph::fromDegrees(lat));
    ring.push_back(road_graph::fromDegrees(lon));
}

// One closed circular loop, `count` points, starting at angle zero.
//
// `clockwise` is the knob the whole first group of tests turns: the source data
// winds the inner loop either way, and the derivation is not allowed to care.
void appendCircle(track::Ring& ring, double centreEast, double centreNorth, double radius,
                  std::size_t count, bool clockwise)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        const double fraction = static_cast<double>(i) / static_cast<double>(count);
        const double angle = 2.0 * std::numbers::pi * (clockwise ? -fraction : fraction);
        appendLocal(ring, centreEast + radius * std::cos(angle),
                    centreNorth + radius * std::sin(angle));
    }
}

constexpr double kRadius = 500.0;
constexpr double kWidth = 14.0;
const double kLapLength = 2.0 * std::numbers::pi * kRadius;

// The outline as the source writes it: all the way round the outside, back to
// where it started, then all the way round the inside -- one ring, no hole.
//
// The repeated start vertex between the two loops is not decoration. It is what
// the real files carry (Monza's is exact to the centimetre) and it is what
// makes the seam findable at all, so a fixture without it would be testing an
// easier problem than the one that ships.
//
// `outerPoints` and `innerPoints` differ on purpose. Real traces sample the two
// loops independently -- Adria's inner carries four times the vertices of its
// outer -- so a fixture with matching counts would let an index-paired
// implementation pass.
track::Ring annulus(bool innerClockwise, std::size_t outerPoints = 360,
                    std::size_t innerPoints = 149)
{
    track::Ring ring;
    appendCircle(ring, 0.0, 0.0, kRadius + kWidth / 2.0, outerPoints, false);
    appendLocal(ring, kRadius + kWidth / 2.0, 0.0);
    appendCircle(ring, 0.0, 0.0, kRadius - kWidth / 2.0, innerPoints, innerClockwise);
    return ring;
}

void expectGoodAnnulus(const track::Derived& derived, const std::string& label)
{
    check(derived.quality == track::Quality::Ok,
          label + ": accepted (got " + track::to_string(derived.quality) + ")");
    check(derived.seam.found, label + ": the seam is found");
    check(derived.seam.returns == 1, label + ": exactly one return to the start");
    check(derived.seam.index == 360, label + ": and it is where the inner loop begins (got " +
                                         std::to_string(derived.seam.index) + ")");
    check(track::pointCount(derived.outer) == 360, label + ": the outer loop is split out whole");
    check(track::pointCount(derived.inner) == 149, label + ": and the inner loop with it");

    if (!derived.centerline.has_value())
    {
        check(false, label + ": a centreline was produced");
        return;
    }
    const auto& line = *derived.centerline;
    checkNear(line.lengthM, kLapLength, kLapLength * 0.005, label + ": centreline length");
    checkNear(line.medianWidthM, kWidth, 0.2, label + ": median width");
    check(line.distanceCm.size() == track::pointCount(line.points),
          label + ": one distance per centreline point");
    check(line.halfWidthCm.size() == track::pointCount(line.points),
          label + ": one half width per centreline point");
    check(line.distanceCm.front() == 0, label + ": distance starts at zero");
    check(std::is_sorted(line.distanceCm.begin(), line.distanceCm.end()),
          label + ": and never goes backwards");
    checkNear(static_cast<double>(line.halfWidthCm[line.halfWidthCm.size() / 2]) / 100.0,
              kWidth / 2.0, 0.2, label + ": half width is half the width");
}

// ============================================================================

void test_an_annulus_gives_back_its_own_centreline()
{
    const auto derived = track::derive(annulus(true), kLapLength, false);
    expectGoodAnnulus(derived, "inner loop wound the other way");
    if (derived.centerline)
    {
        check(derived.centerline->reversed,
              "an oppositely wound inner loop is recorded as reversed");
    }
}

void test_the_same_annulus_with_both_loops_wound_the_same_way()
{
    // THE TRAP. Monza and the Nordschleife wind both loops the same way; Laguna
    // Seca winds them oppositely. An implementation that assumes either one
    // pairs the loops as a REFLECTION on half the corpus -- and a reflection
    // preserves length, so every length check still passes while the shape is
    // turned inside out.
    const auto derived = track::derive(annulus(false), kLapLength, false);
    expectGoodAnnulus(derived, "both loops wound the same way");
    if (derived.centerline)
    {
        check(!derived.centerline->reversed,
              "a same-wound inner loop is recorded as not reversed");
    }
}

void test_the_loops_may_be_sampled_at_different_densities()
{
    // 360 points against 37: pairing by index would match every tenth vertex of
    // one loop against every vertex of the other.
    const auto derived = track::derive(annulus(true, 360, 37), kLapLength, false);
    check(derived.quality == track::Quality::Ok,
          "a sparsely sampled inner loop still pairs (got " +
              std::string(track::to_string(derived.quality)) + ")");
    if (derived.centerline)
    {
        // 1.5 m rather than the 0.2 m the dense fixture gets, and the slack is
        // the FIXTURE's: a 37-gon inscribed in a 493 m circle sits about 0.9 m
        // inside it, so a correctly paired centreline here really is ~15 m
        // wide. Still far tighter than a mispairing, which misses by tens of
        // metres at least.
        checkNear(derived.centerline->medianWidthM, kWidth, 1.5,
                  "and the width is still the width");
    }
}

void test_a_composite_hands_back_every_curve()
{
    // Buenos Aires F is seven layouts accumulated into one outline, Reno
    // Fernley A2 nine. Reporting only the first curve and calling the whole
    // remainder a hole punches a 52 km "infield" out of a 2 km curve -- which
    // draws, and draws as nonsense. Every curve comes back instead.
    track::Ring ring = annulus(true);
    const track::Ring again = annulus(true);
    ring.insert(ring.end(), again.begin(), again.end());

    const auto derived = track::derive(ring, kLapLength, false);
    check(derived.quality == track::Quality::MultipleLoops, "still diagnosed as multi-loop");
    check(derived.loops.size() >= 4,
          "and every boundary curve is handed back (got " +
              std::to_string(derived.loops.size()) + ")");
    check(derived.outer.empty() && derived.inner.empty(),
          "with no arbitrary outer/inner split");

    // Consecutive pairs: each (outer, inner) pair should be a real annulus, so
    // the two curves of a pair are within a few percent of each other.
    for (std::size_t i = 0; i + 1 < derived.loops.size(); i += 2)
    {
        const double a = track::ringLengthM(derived.loops[i], true);
        const double b = track::ringLengthM(derived.loops[i + 1], true);
        check(std::abs(a - b) / std::max(a, b) < 0.10,
              "curves " + std::to_string(i) + " and " + std::to_string(i + 1) +
                  " pair as one layout's two boundaries");
    }
}

void test_several_loops_in_one_file_are_diagnosed_not_averaged()
{
    // Monza Without Chicane: 798 points holding four loops, not two. Split at
    // any one of them and the result is a centreline of 26 m median width --
    // entirely plausible -- and 11 445 m against a published 5 725.
    track::Ring ring = annulus(true);
    const track::Ring again = annulus(true);
    ring.insert(ring.end(), again.begin(), again.end());

    const auto derived = track::derive(ring, kLapLength, false);
    check(derived.quality == track::Quality::MultipleLoops,
          "four loops in one ring are reported as such (got " +
              std::string(track::to_string(derived.quality)) + ")");
    check(derived.seam.returns > 1, "and the extra returns to the start are counted");
    check(!derived.loops.empty(),
          "while the curves are still handed back, because the map still draws them");
}

void test_repeated_vertices_do_not_inflate_the_return_count()
{
    // KML STUTTER. 87 of the real outlines carry runs of identical consecutive
    // vertices -- Milford Road Course has 2 346 of them -- and a revisit count
    // that treats each one as its own return reads a plain circuit as holding
    // four or ten loops, then rejects it.
    //
    // Nothing here needs to dedupe: the excursion scan takes the MINIMUM over a
    // run of near approaches and skips past it, so a stutter collapses to one
    // candidate however long it is. This pins that.
    track::Ring ring;
    appendCircle(ring, 0.0, 0.0, kRadius + kWidth / 2.0, 360, false);
    // The trace returning to its start, stuttering as it goes.
    for (int i = 0; i < 12; ++i)
    {
        appendLocal(ring, kRadius + kWidth / 2.0, 0.0);
    }
    appendCircle(ring, 0.0, 0.0, kRadius - kWidth / 2.0, 149, true);

    const auto seam = track::findSeam(ring);
    check(seam.found, "a stuttering trace still yields a seam");
    check(seam.returns == 1,
          "and twelve identical vertices are ONE return, not twelve (got " +
              std::to_string(seam.returns) + ")");

    const auto derived = track::derive(ring, kLapLength, false);
    check(derived.quality == track::Quality::Ok,
          "so the track is still accepted (got " +
              std::string(track::to_string(derived.quality)) + ")");
}

void test_passing_near_the_start_is_not_a_second_loop()
{
    // THE OTHER HALF of the multi-loop rule, and the one that costs good
    // tracks rather than admitting bad ones.
    //
    // Fiorano returns to its start at 0.00 m and passes 4.42 m away elsewhere;
    // Adria closes sloppily at 16.21 m and passes 26.36 m away. Both are
    // ordinary circuits, and a rule with an absolute "within 5 m is a rival"
    // floor called both of them multi-loop. What separates a real extra loop is
    // that it revisits the SAME VERTEX -- Charlotte's oval and infield both
    // come back to 0.00 m -- so a rival has to be as exact as the best is, not
    // merely close.
    track::Ring ring;
    appendCircle(ring, 0.0, 0.0, kRadius + kWidth / 2.0, 180, false);
    // A detour that comes back within a few metres of the start and carries on.
    appendLocal(ring, kRadius + kWidth / 2.0 + 4.5, 6.0);
    appendLocal(ring, kRadius + kWidth / 2.0 + 30.0, 40.0);
    appendCircle(ring, 0.0, 0.0, kRadius + kWidth / 2.0 + 30.0, 60, false);
    appendLocal(ring, kRadius + kWidth / 2.0, 0.0);
    appendCircle(ring, 0.0, 0.0, kRadius - kWidth / 2.0, 149, true);

    const auto seam = track::findSeam(ring);
    check(seam.found, "the seam is still found past a near miss");
    check(seam.returns == 1,
          "and a 4.5 m near miss beside a 0 m return is not a second loop (got " +
              std::to_string(seam.returns) + ")");
}

// A point-to-point course, drawn the way this database actually draws one: a
// THIN CLOSED RIBBON, out along one edge and back along the other.
//
// `courseLength` of arc, `width` wide. There is no open outline anywhere in the
// corpus -- Pikes Peak's boundary closes to within 0.1 m and its perimeter is
// twice its course -- so a fixture with an open LineString would be testing a
// shape that does not exist.
track::Ring ribbon(double radius, double sweepRadians, double width, std::size_t points)
{
    std::vector<std::pair<double, double>> left;
    std::vector<std::pair<double, double>> right;
    for (std::size_t i = 0; i < points; ++i)
    {
        const double t = sweepRadians * static_cast<double>(i) /
                         static_cast<double>(points - 1);
        // Along the arc, and the outward normal at that point.
        const double east = radius * std::cos(t);
        const double north = radius * std::sin(t);
        left.emplace_back(east * (1.0 + width / (2.0 * radius)),
                          north * (1.0 + width / (2.0 * radius)));
        right.emplace_back(east * (1.0 - width / (2.0 * radius)),
                           north * (1.0 - width / (2.0 * radius)));
    }

    track::Ring ring;
    for (const auto& [east, north] : left)
    {
        appendLocal(ring, east, north);
    }
    for (auto it = right.rbegin(); it != right.rend(); ++it)
    {
        appendLocal(ring, it->first, it->second);
    }
    return ring;
}

void test_a_point_to_point_course_gives_an_open_centreline()
{
    // THE CASE THE FEATURE TYPE CANNOT TELL YOU ABOUT. This ribbon is a closed
    // polygon exactly as a circuit's outline is, and its perimeter is twice its
    // course exactly as a circuit's is. Nothing about the boundary says which it
    // is -- only the centreline that comes out does.
    constexpr double kCourseRadius = 800.0;
    constexpr double kSweep = 2.0;
    constexpr double kRibbonWidth = 9.0;
    const double courseLength = kCourseRadius * kSweep;

    const auto derived = track::derive(ribbon(kCourseRadius, kSweep, kRibbonWidth, 400),
                                       courseLength, false);

    check(derived.quality == track::Quality::Ok,
          "a thin closed ribbon derives cleanly (got " +
              std::string(track::to_string(derived.quality)) + ")");
    check(derived.topology == track::Topology::Ribbon,
          std::string("and is read as a ribbon, not an annulus (got ") +
              track::to_string(derived.topology) + ")");

    if (!derived.centerline.has_value())
    {
        check(false, "a centreline was produced");
        return;
    }
    const auto& line = *derived.centerline;
    check(!line.closed, "its centreline is OPEN -- which is the only thing that says so");
    checkNear(line.lengthM, courseLength, courseLength * 0.02, "course length");
    // 1.5 m of slack, and it is the FIXTURE's. A constant-curvature arc makes
    // the outer edge 1.1 % longer than the inner, so pairing them by arc length
    // from the fold drifts along-track and adds that drift into the measured
    // width. A real course curves both ways and it largely cancels -- Gurston
    // Down comes out at 4.1 m and Osnabrück at 5.5 m, both correct.
    checkNear(line.medianWidthM, kRibbonWidth, 1.5, "course width");
    check(derived.outer.empty() && derived.inner.empty(),
          "and no hole is cut, because a point-to-point course has no infield");
}

void test_a_circuit_is_read_as_closed()
{
    // The other half of the same decision, and it has to come from the geometry
    // too: this fixture's outline is a closed polygon, just like the ribbon's.
    const auto derived = track::derive(annulus(true), kLapLength, false);
    check(derived.topology == track::Topology::Annulus,
          std::string("an annulus is read as one (got ") + track::to_string(derived.topology) +
              ")");
    if (derived.centerline)
    {
        check(derived.centerline->closed, "and its centreline joins up");
    }
}

void test_a_shape_that_is_not_a_ribbon_is_rejected()
{
    // A single circle is a curve, not a traced track edge: there is no second
    // side to pair it against. Whichever gate catches it, it must not come out
    // as a track.
    track::Ring ring;
    appendCircle(ring, 0.0, 0.0, kRadius, 360, false);

    const auto derived = track::derive(ring, 0.0, false);
    check(derived.quality != track::Quality::Ok,
          "a curve with no second side is not a track (got " +
              std::string(track::to_string(derived.quality)) + ")");
}

void test_loops_that_are_not_a_ribbon_are_rejected_on_width()
{
    // Two loops that touch at a point and diverge. The seam is found -- they do
    // meet -- but there is no track between them.
    track::Ring ring;
    appendCircle(ring, 0.0, 0.0, kRadius, 360, false);
    appendCircle(ring, kRadius - 100.0, 0.0, 100.0, 149, false);

    const auto derived = track::derive(ring, 0.0, false);
    check(derived.quality == track::Quality::WidthOutOfRange,
          "a 400 m 'track' is not a track (got " +
              std::string(track::to_string(derived.quality)) + ")");
}

void test_a_wrong_published_length_warns_but_does_not_reject()
{
    // A CLEAN SINGLE-LOOP OUTLINE WITH A WRONG LABEL, which is what 40 of the
    // real files are: the outline is a KML digitisation and `length_m` comes
    // from a separate Racelogic catalogue, so a disagreement means one of two
    // independent sources is wrong -- and it is not always the geometry. Inde
    // Motorsports Ranch Full's published figure is a third of its outline;
    // rejecting it threw away a perfectly good centreline.
    //
    // Nothing measures distance-along-lap against `length_m`; it comes from the
    // geometry. So this is a warning.
    const auto derived = track::derive(annulus(true), kLapLength / 3.0, false);
    check(derived.quality == track::Quality::Ok,
          "a clean outline with a wrong published length is still usable (got " +
              std::string(track::to_string(derived.quality)) + ")");
    check(derived.publishedLengthDisagrees,
          "but the disagreement with the published length is recorded");
    check(derived.centerline.has_value(), "and a centreline is produced");
}

void test_the_source_degenerate_flag_is_honoured()
{
    // Exactly one real file carries `degenerate: true` -- 329 points on a flat
    // line at constant latitude in the Atlantic. Left to the geometric checks
    // it comes out as whichever one happens to trip first; the source already
    // knows, so ask it.
    const auto derived = track::derive(annulus(true), kLapLength, true);
    check(derived.quality == track::Quality::Degenerate,
          "a source-flagged degenerate outline is rejected as such (got " +
              std::string(track::to_string(derived.quality)) + ")");
    check(!derived.centerline.has_value(), "and no centreline is derived from it");
}

void test_a_missing_published_length_is_not_agreement()
{
    // Two files carry no Start / Finish point and so no length_m. Zero must
    // skip the check, never satisfy it.
    const auto derived = track::derive(annulus(true), 0.0, false);
    check(derived.quality == track::Quality::Ok,
          "no published length still derives (got " +
              std::string(track::to_string(derived.quality)) + ")");
    checkNear(derived.lengthErrorFraction, 0.0, 1e-9,
              "and reports no error against a length it was never given");
}

// ============================================================================

void test_the_gate_lands_on_the_track_and_rebases_the_lap()
{
    auto derived = track::derive(annulus(true), kLapLength, false);
    if (!derived.centerline)
    {
        check(false, "gate test needs a centreline");
        return;
    }

    // Due north of the centre, on the racing surface.
    track::Ring point;
    appendLocal(point, 0.0, kRadius);

    track::Gate gate;
    const auto result = track::placeGate(*derived.centerline, derived.outer, derived.inner,
                                         point[0], point[1], gate);
    check(result == track::GateResult::Placed,
          std::string("a point on the tarmac places the gate (got ") +
              track::to_string(result) + ")");
    check(gate.present, "and the gate is marked present");
    checkNear(gate.widthM, kWidth, 1.0, "the gate spans the track");

    const auto& line = *derived.centerline;
    check(line.distanceCm.front() == 0, "the lap now starts at the gate");
    check(std::is_sorted(line.distanceCm.begin(), line.distanceCm.end()),
          "and runs forward from it without wrapping mid-array");
    check(gate.centerlineOffsetCm < 1000,
          "the residual is under a sample spacing (got " +
              std::to_string(gate.centerlineOffsetCm) + " cm)");

    // The gate centre is where the point was, not where the trace started.
    checkNear(road_graph::distanceM(gate.centreLat, gate.centreLon, point[0], point[1]), 0.0, 10.0,
              "the gate is at the point the file gave");
}

void test_a_gate_on_an_open_course_does_not_rotate_it()
{
    // A circuit's lap has no beginning, so the gate is put at sample zero and
    // the distances run forward from it. A point-to-point course HAS a
    // beginning -- the fold at one end -- and rotating an open polyline does
    // not renumber it, it TEARS IT IN HALF and joins the two ends across the
    // map. The centreline still has the right number of points and the right
    // distances, so nothing fails; it is simply the wrong shape.
    constexpr double kCourseRadius = 800.0;
    constexpr double kSweep = 2.0;
    constexpr double kRibbonWidth = 9.0;

    auto derived = track::derive(ribbon(kCourseRadius, kSweep, kRibbonWidth, 400),
                                 kCourseRadius * kSweep, false);
    if (!derived.centerline.has_value() || derived.centerline->closed)
    {
        check(false, "open-course gate test needs an open centreline");
        return;
    }

    const track::Ring before = derived.centerline->points;
    const double lengthBefore = derived.centerline->lengthM;

    // A quarter of the way along the course, on the surface.
    const double t = kSweep * 0.25;
    track::Ring point;
    appendLocal(point, kCourseRadius * std::cos(t), kCourseRadius * std::sin(t));

    track::Gate gate;
    const auto result = track::placeGate(*derived.centerline, derived.outer, derived.inner,
                                         point[0], point[1], gate);
    check(result == track::GateResult::Placed,
          std::string("a point on an open course places the gate (got ") +
              track::to_string(result) + ")");
    check(derived.centerline->points == before,
          "and the centreline is left exactly where it was, not rotated");
    checkNear(derived.centerline->lengthM, lengthBefore, 0.001,
              "so its length is unchanged");
    check(derived.centerline->distanceCm.front() == 0,
          "distances still run from the start of the course");
    check(std::is_sorted(derived.centerline->distanceCm.begin(),
                         derived.centerline->distanceCm.end()),
          "and still run forward");
    // Not a residual here: on an open course the offset is how far along the
    // course the gate actually is, which is a quarter of it.
    //
    // FROM EITHER END. An open centreline's direction is arbitrary -- the two
    // folds are interchangeable and the search takes whichever pairs better --
    // so nothing may assume which end is the start of the course. Asserting a
    // quarter rather than three quarters would pin an accident of the fixture.
    const double offset = static_cast<double>(gate.centerlineOffsetCm) / 100.0;
    checkNear(std::min(offset, lengthBefore - offset), lengthBefore * 0.25, lengthBefore * 0.03,
              "the gate offset is its distance along the course, from whichever end");
}

void test_a_gate_in_the_infield_is_refused()
{
    // A gate 40 m off is worse than no gate: it would still project onto the
    // centreline perfectly happily and start the lap where nobody drives.
    auto derived = track::derive(annulus(true), kLapLength, false);
    if (!derived.centerline)
    {
        check(false, "gate test needs a centreline");
        return;
    }

    track::Ring point;
    appendLocal(point, 0.0, kRadius - 200.0);

    track::Gate gate;
    const auto result = track::placeGate(*derived.centerline, derived.outer, derived.inner,
                                         point[0], point[1], gate);
    check(result == track::GateResult::OffTrack,
          std::string("a point in the infield is refused (got ") + track::to_string(result) + ")");
    check(!gate.present, "and no gate is produced");
}

// ============================================================================

void test_the_principal_axis_is_an_axis_not_a_direction()
{
    // A long thin rectangle at 30 degrees. 30 and 210 describe the same line,
    // and a viewer rotating a circuit to fill a wide screen must get the same
    // picture from either.
    constexpr double kBearing = 30.0;
    const double radians = kBearing * std::numbers::pi / 180.0;
    const double alongEast = std::sin(radians);
    const double alongNorth = std::cos(radians);

    track::Ring ring;
    for (int i = -50; i <= 50; ++i)
    {
        const double t = static_cast<double>(i) * 20.0;
        appendLocal(ring, alongEast * t + alongNorth * 40.0, alongNorth * t - alongEast * 40.0);
    }
    for (int i = 50; i >= -50; --i)
    {
        const double t = static_cast<double>(i) * 20.0;
        appendLocal(ring, alongEast * t - alongNorth * 40.0, alongNorth * t + alongEast * 40.0);
    }

    checkNear(track::principalAxisDeg(ring), kBearing, 2.0,
              "the long axis of a rotated rectangle");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_an_annulus_gives_back_its_own_centreline();
    test_the_same_annulus_with_both_loops_wound_the_same_way();
    test_the_loops_may_be_sampled_at_different_densities();
    test_several_loops_in_one_file_are_diagnosed_not_averaged();
    test_a_composite_hands_back_every_curve();
    test_repeated_vertices_do_not_inflate_the_return_count();
    test_passing_near_the_start_is_not_a_second_loop();
    test_a_point_to_point_course_gives_an_open_centreline();
    test_a_circuit_is_read_as_closed();
    test_a_shape_that_is_not_a_ribbon_is_rejected();
    test_loops_that_are_not_a_ribbon_are_rejected_on_width();
    test_a_wrong_published_length_warns_but_does_not_reject();
    test_the_source_degenerate_flag_is_honoured();
    test_a_missing_published_length_is_not_agreement();
    test_the_gate_lands_on_the_track_and_rebases_the_lap();
    test_a_gate_on_an_open_course_does_not_rotate_it();
    test_a_gate_in_the_infield_is_refused();
    test_the_principal_axis_is_an_axis_not_a_direction();

    spdlog::set_level(spdlog::level::info);
    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all track geometry checks passed");
    return 0;
}
