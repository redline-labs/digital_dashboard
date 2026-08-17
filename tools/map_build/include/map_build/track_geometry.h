// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning a traced track OUTLINE into a centreline.
//
// The source GeoJSONs do not contain what you would expect. A file that says
// "Monza" holds a single ring whose traced length is 11 530 m against a
// published lap of 5 762 -- exactly twice. It is not a centreline and it is not
// a racing line: it is the EDGE OF THE TARMAC, traced all the way round the
// outside and then all the way back round the inside, joined into one ring.
//
// So the ring is `[outer loop][inner loop]`, and everything here follows from
// finding where one becomes the other:
//
//   - the SEAM is the vertex where the trace returns to where it started. On
//     the great majority of files there is exactly one such vertex and it is
//     within 2 m of point 0;
//   - the two loops are then paired by arc length and averaged, which gives the
//     centreline, and the distance between each pair IS the local track width;
//   - a file with SEVERAL returns to the start is several loops concatenated
//     (Charlotte's oval plus its infield, the "Combo" layouts), and there is no
//     single centreline to extract. That is diagnosed, not averaged.
//
// THE RING HAS NO HOLE AND ITS WINDING MEANS NOTHING. Every closed file has
// exactly one entry in `coordinates`, and the two loops are wound the same way
// as often as not -- Monza and the Nordschleife agree, Laguna Seca disagrees.
// Handing the raw ring to a tessellator as one polygon therefore fills the
// infield solid: you get a blob where the track should be, with nothing logged
// and nothing thrown. Splitting it here and passing the two loops on as ROLES
// (outer, inner) is what avoids that, and it is why this runs at ingest and not
// in the renderer.
//
// EVERY OUTLINE IN THIS DATABASE IS CLOSED, and that is the thing most likely
// to be got wrong downstream. There are no open outlines at all: measured
// across all 994 files, the gap between the last vertex and the first is at
// most 3.4% of the perimeter and 0.2% at the median. A point-to-point course is
// not drawn as an open line -- Pikes Peak is a thin CLOSED ribbon whose
// perimeter, 39 745 m, is twice its published 19 915 m course, exactly as a
// circuit's is.
//
// So the boundary cannot tell you point-to-point from circuit, and neither can
// the source's Polygon/LineString type or its `closed` property. What differs
// is the TOPOLOGY:
//
//   * a CIRCUIT's surface is an annulus, so its boundary is two closed curves
//     -- an outer and an inner -- which the trace joins into one ring by
//     returning to where it started. Splitting at that seam gives two closed
//     loops and a CLOSED centreline;
//   * a POINT-TO-POINT course's surface is a long thin rectangle, so its
//     boundary is ONE closed curve with two folds, one at each end of the
//     course. Splitting at the folds gives two open edges and an OPEN
//     centreline.
//
// Both are handled, and which one applied is decided by what came out --
// Centerline::closed -- never by what the file called itself.
//
// NO I/O. No sqlite, no capnp, no GeoJSON, no mbtiles -- rings in, rings out,
// so the awkward cases can be built by hand in a test. Same split as
// map_build/rings.h against the PBF reader, and for the same reason.
#ifndef MAP_BUILD_TRACK_GEOMETRY_H
#define MAP_BUILD_TRACK_GEOMETRY_H

#include <cstdint>
#include <optional>
#include <vector>

#include "osm/entity.h"

namespace map_build::track
{

// Interleaved lat/lon in 1e-7 degrees, first point NOT repeated at the end --
// the same convention as map_build::AssembledRings and DrawInput::geometry.
using Ring = std::vector<osm::Coord>;

inline std::size_t pointCount(const Ring& ring)
{
    return ring.size() / 2;
}

// ============================================================================
// The seam
// ============================================================================

// Where the outer loop ends and the inner loop begins.
//
// `index` is a POINT index, not an offset into the flat array: the outer loop
// is points [0, index] and the inner loop is points [index, count). The seam
// point belongs to both, which is what makes them each closed.
struct Seam
{
    std::size_t index { 0 };
    // How near the trace came to its own start there. Zero on a clean file.
    double gapM { 0.0 };
    // How many DISTINCT returns to the start were found. One is the normal
    // case. More than one means several loops in one file and there is no
    // centreline to extract -- see Quality::MultipleLoops.
    std::uint32_t returns { 0 };
    bool found { false };
};

// Find it.
//
// `maxGapM` is deliberately generous (30 m by default). The clean files return
// to within 2 m, but many of those that do not are still ordinary circuits whose
// trace closes sloppily, and rejecting them for a 12 m gap would throw away good
// tracks to no purpose.
//
// NOT FINDING A SEAM IS A NORMAL ANSWER, and it is how a point-to-point course
// announces itself: its boundary is one closed curve rather than two, so it
// never returns to its start -- Pikes Peak misses by 1 104 m. derive() reads
// that as a ribbon and pairs it from its folds instead.
Seam findSeam(const Ring& ring, double maxGapM = 30.0);

// ============================================================================
// The centreline
// ============================================================================

struct Centerline
{
    Ring points;
    // Cumulative distance from the start of `points`, one entry per POINT.
    // On a CLOSED centreline placeGate() rotates this so entry 0 is the gate.
    // On an OPEN one it is left running from one end of the course, because
    // rotating an open polyline tears it rather than renumbering it.
    std::vector<std::uint32_t> distanceCm;
    // Half the distance between the two loops at each sample, one per POINT.
    // Parallel to `points`, so a consumer can ask how much road there is beside
    // it without a second lookup.
    std::vector<std::uint16_t> halfWidthCm;
    double lengthM { 0.0 };
    // The median of 2 * halfWidthCm. This is the QA signal: a real track is
    // 4-20 m wide -- a hillclimb on a public road is narrower than a circuit --
    // so a median of 500 m says the pairing is wrong however plausible the
    // result looks.
    double medianWidthM { 0.0 };
    // Which alignment won. Recorded because it is the first thing to look at
    // when a track comes out wrong, and because it is not predictable from the
    // source -- see the note about winding above.
    bool reversed { false };

    // WHETHER THIS CENTRELINE JOINS UP. Derived from the geometry that came
    // out, and the only trustworthy answer to "is this a circuit or a
    // point-to-point course": every outline in the database is closed, so
    // neither the feature type nor the source's `closed` property can say.
    //
    // The measurement separates the two cleanly. Endpoint gap over centreline
    // length is 0.0015-0.015 for circuits (Monza 0.005, the Nordschleife
    // 0.0015) and 0.15-0.85 for point-to-point courses (Aintree Sprint 0.15,
    // Gurston Down 0.85) -- two orders of magnitude apart.
    //
    // WHEN THIS IS FALSE THE DIRECTION IS ARBITRARY. The two folds of a
    // point-to-point course are interchangeable and the search takes whichever
    // pairs better, so `points` may run either way along it. Nothing may assume
    // that index zero is the start of the course rather than the finish.
    bool closed { true };
};

// Every way this can fail, named.
//
// A track that fails still RENDERS: the outline is what draws, and it is fine
// whatever happened here. So this is the field that says why a track has no
// distance-along-lap, rather than leaving the absence to be guessed at.
enum class Quality : std::uint8_t
{
    Unknown,
    Ok,
    // Neither split worked: the outline is not a ribbon at all, under either
    // topology. Rare, and it means the source geometry is something other than
    // a traced track edge.
    SeamNotFound,
    // Several loops concatenated into one file. No single centreline exists.
    MultipleLoops,
    // The pairing produced a "track" hundreds of metres wide.
    WidthOutOfRange,
    // The centreline disagrees with the published lap length.
    LengthMismatch,
    // The outline itself is not twice a lap. A source-data problem, and worth
    // separating from our own arithmetic going wrong -- Road Atlanta's trace is
    // 3.4x its published length before this code touches it.
    SourceLengthImplausible,
    // Too few points to be a track at all.
    Degenerate,
};

const char* to_string(Quality quality);

struct DeriveOptions
{
    // Samples along each loop. The pairing is a phase search, so this is the
    // resolution of the answer as well as of the geometry.
    std::size_t samples { 2000 };
    // A track this wide is not a track.
    //
    // The floor is 3 m rather than 5 because point-to-point courses are
    // genuinely narrow: Gurston Down measures 4.1 m and Osnabrück 5.5 m, both
    // being public roads rather than circuits. A 5 m floor rejected them.
    double minWidthM { 3.0 };
    double maxWidthM { 30.0 };
    // Endpoint gap over centreline length, above which a centreline is taken to
    // be OPEN. Anything between 0.015 and 0.15 is unoccupied in this corpus.
    double closedEndGapFraction { 0.05 };
    // Fractional agreement required against the published lap length, and
    // against half the outline's own length.
    double lengthTolerance { 0.05 };
    double seamMaxGapM { 30.0 };
};

// Which topology the outline turned out to have. See the note at the top: this
// is worked out from the geometry, never from the feature type.
enum class Topology : std::uint8_t
{
    Unknown,
    // Two closed boundary curves joined at a seam. A circuit.
    Annulus,
    // One closed boundary curve with a fold at each end. A point-to-point
    // course, drawn as a thin closed ribbon like everything else here.
    Ribbon,
};

const char* to_string(Topology topology);

struct Derived
{
    Topology topology { Topology::Unknown };

    // The two loops, split at the seam. Populated for an Annulus whenever the
    // seam was found, EVEN IF the centreline was rejected -- these are what the
    // map draws, and they are what makes the infield a hole rather than a fill.
    //
    // Empty for a Ribbon: a point-to-point course's surface has no infield to
    // cut out, so the outline is drawn as one solid shape.
    Ring outer;
    Ring inner;

    // Every boundary curve, in trace order, when the outline holds more than
    // one. Populated only for Quality::MultipleLoops.
    //
    // These come in consecutive (outer, inner) PAIRS, one pair per layout the
    // file accumulated -- Buenos Aires F holds seven, EuroSpeedway Combo seven,
    // Reno Fernley A2 nine. Carried so the renderer can draw a composite as the
    // multipolygon it actually is, rather than as one curve with everything
    // else punched out of it as a hole.
    std::vector<Ring> loops;

    std::optional<Centerline> centerline;
    Quality quality { Quality::Unknown };

    Seam seam;
    double outlineLengthM { 0.0 };
    double publishedLengthM { 0.0 };
    // Signed: positive when the derived centreline is longer than published.
    double lengthErrorFraction { 0.0 };

    // The derived centreline disagrees with `length_m` beyond tolerance.
    //
    // A WARNING, NOT A REJECTION, and the distinction rests on where the two
    // numbers come from. The outline is a KML digitisation; `length_m` is the
    // `length` attribute of a separate Racelogic StartFinish catalogue. They are
    // independent sources, so a disagreement says one of the two is wrong -- and
    // it is not always the geometry. On 40 tracks the outline is a clean single
    // loop and the published figure is for a different layout entirely (Inde
    // Motorsports Ranch Full is 3.00x, Desert North Palm 1.97x).
    //
    // Rejecting those threw away 40 usable centrelines to protect a label that
    // nothing measures anything against: distance-along-lap comes from the
    // geometry, never from `length_m`. So the gate is now the two checks that
    // are internal to the geometry -- median width, and the centreline against
    // half its own outline -- and this flag records the cross-check for anyone
    // who wants it.
    //
    // What the published figure USED to catch -- a file holding several loops,
    // where the derivation is self-consistently wrong by a factor of two -- is
    // caught independently by Seam::returns, which is what makes dropping it
    // safe.
    bool publishedLengthDisagrees { false };
};

// Split, pair, average, and judge.
//
// `publishedLengthM` is the `length_m` from the file's Start / Finish point.
// Zero means the file did not say, and the length check is then SKIPPED rather
// than silently passed -- a missing number must not read as agreement.
//
// The gate is all three of: median width in range, the centreline within
// tolerance of the published length, and the centreline within tolerance of
// half the outline's own length. Any one alone is not enough. Monza Without
// Chicane is the case that settles it: its median width is a perfectly
// respectable 26 m while its "centreline" is 11 445 m against a published
// 5 725, because the file holds four loops and the seam split it in the wrong
// place. The width gate alone would have shipped it.
Derived derive(const Ring& ring, double publishedLengthM, bool sourceSaysDegenerate,
               const DeriveOptions& options = {});

// ============================================================================
// The start/finish gate
// ============================================================================

struct Gate
{
    bool present { false };

    osm::Coord centreLat { 0 };
    osm::Coord centreLon { 0 };
    // The two ends, cast perpendicular to the centreline out to each loop. A
    // gate is a LINE the vehicle crosses; a crossing test against the centre
    // point alone is a distance threshold, and at 250 km/h a 10 Hz fix moves
    // 7 m between samples, so a threshold either fires late or fires twice.
    osm::Coord leftLat { 0 };
    osm::Coord leftLon { 0 };
    osm::Coord rightLat { 0 };
    osm::Coord rightLon { 0 };

    // Where s == 0 falls along the centreline, before rebasing.
    std::uint32_t centerlineOffsetCm { 0 };

    double widthM { 0.0 };
};

enum class GateResult : std::uint8_t
{
    Placed,
    // The file had no Start / Finish point.
    NoPoint,
    // More than one. Guessing which is meant is worse than declining.
    Ambiguous,
    // The point is not on the track. Kept as its own outcome rather than
    // folded into NoPoint: a gate 40 m into the infield is worse than no gate,
    // and it means the data is wrong in a way somebody should hear about.
    OffTrack,
    // There is no centreline to place it against.
    NoCenterline,
};

const char* to_string(GateResult result);

// Project the Start / Finish point onto the centreline, cast the gate line
// across the track, and REBASE `centerline.distanceCm` so that zero is the
// gate. Mutates `centerline` on success and leaves it untouched otherwise.
GateResult placeGate(Centerline& centerline, const Ring& outer, const Ring& inner,
                     osm::Coord pointLat, osm::Coord pointLon, Gate& gate);

// ============================================================================
// Small shared geometry
// ============================================================================

// The long axis, degrees clockwise from north, from the covariance of the
// points. Cheap here and awkward later: a viewer that wants to rotate a circuit
// to fill a wide screen needs it, and it is a property of the geometry rather
// than of the view.
double principalAxisDeg(const Ring& ring);

// Length of a ring in metres. `closed` adds the implied closing edge.
double ringLengthM(const Ring& ring, bool closed);

} // namespace map_build::track

#endif // MAP_BUILD_TRACK_GEOMETRY_H
