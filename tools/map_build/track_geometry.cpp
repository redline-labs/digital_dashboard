// SPDX-License-Identifier: GPL-3.0-or-later
#include "map_build/track_geometry.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "road_graph/geometry.h"

namespace map_build::track
{

namespace
{

// Metres per degree of latitude, on the same sphere road_graph::distanceM uses.
// Keeping the constant identical matters: the QA gate compares a length
// computed here against one a consumer will recompute there, and two earth
// radii would put a systematic tenth of a percent between them.
constexpr double kMetresPerDegree = std::numbers::pi * road_graph::kEarthRadiusM / 180.0;

struct Vec2
{
    double x { 0.0 };
    double y { 0.0 };
};

// A local tangent plane. Every pairing decision below is a distance comparison,
// and doing those in degrees would weight longitude by cos(latitude) -- at the
// Nordschleife that is a 36 % error in one axis, which is more than a track is
// wide.
class Frame
{
  public:
    explicit Frame(const Ring& ring)
    {
        const std::size_t count = pointCount(ring);
        if (count == 0)
        {
            return;
        }
        double latSum = 0.0;
        double lonSum = 0.0;
        for (std::size_t i = 0; i < count; ++i)
        {
            latSum += road_graph::toDegrees(ring[2 * i]);
            lonSum += road_graph::toDegrees(ring[2 * i + 1]);
        }
        mLat0 = latSum / static_cast<double>(count);
        mLon0 = lonSum / static_cast<double>(count);
        mCosLat = std::cos(mLat0 * std::numbers::pi / 180.0);
    }

    Vec2 toLocal(osm::Coord lat, osm::Coord lon) const
    {
        return { (road_graph::toDegrees(lon) - mLon0) * kMetresPerDegree * mCosLat,
                 (road_graph::toDegrees(lat) - mLat0) * kMetresPerDegree };
    }

    void toCoord(Vec2 p, osm::Coord& lat, osm::Coord& lon) const
    {
        lat = road_graph::fromDegrees(mLat0 + p.y / kMetresPerDegree);
        lon = road_graph::fromDegrees(mLon0 + p.x / (kMetresPerDegree * mCosLat));
    }

  private:
    double mLat0 { 0.0 };
    double mLon0 { 0.0 };
    double mCosLat { 1.0 };
};

std::vector<Vec2> project(const Ring& ring, const Frame& frame)
{
    const std::size_t count = pointCount(ring);
    std::vector<Vec2> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        out.push_back(frame.toLocal(ring[2 * i], ring[2 * i + 1]));
    }
    return out;
}

double dist(Vec2 a, Vec2 b)
{
    return std::hypot(b.x - a.x, b.y - a.y);
}

double polylineLength(const std::vector<Vec2>& p, bool closed)
{
    if (p.size() < 2)
    {
        return 0.0;
    }
    double total = 0.0;
    for (std::size_t i = 1; i < p.size(); ++i)
    {
        total += dist(p[i - 1], p[i]);
    }
    if (closed)
    {
        total += dist(p.back(), p.front());
    }
    return total;
}

// Resample a CLOSED polyline to `n` evenly spaced samples, the last sample one
// step short of the first so the result can be rotated freely. Uniform in ARC
// LENGTH and not in index, which is the whole point: the two loops of a track
// outline are traced at wildly different vertex densities -- Adria's inner loop
// carries four times the vertices of its outer -- so pairing by index compares
// a hairpin against a straight.
std::vector<Vec2> resampleClosed(const std::vector<Vec2>& p, std::size_t n)
{
    std::vector<Vec2> out;
    if (p.size() < 2 || n == 0)
    {
        return out;
    }

    std::vector<double> cumulative;
    cumulative.reserve(p.size() + 1);
    cumulative.push_back(0.0);
    for (std::size_t i = 1; i < p.size(); ++i)
    {
        cumulative.push_back(cumulative.back() + dist(p[i - 1], p[i]));
    }
    cumulative.push_back(cumulative.back() + dist(p.back(), p.front()));

    const double total = cumulative.back();
    if (total <= 0.0)
    {
        return out;
    }

    out.reserve(n);
    std::size_t segment = 0;
    for (std::size_t k = 0; k < n; ++k)
    {
        const double target = total * static_cast<double>(k) / static_cast<double>(n);
        while (segment + 1 < cumulative.size() - 1 && cumulative[segment + 1] < target)
        {
            ++segment;
        }
        const double span = cumulative[segment + 1] - cumulative[segment];
        const double f = span <= 0.0 ? 0.0 : (target - cumulative[segment]) / span;
        const Vec2& a = p[segment];
        const Vec2& b = p[(segment + 1) % p.size()];
        out.push_back({ a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f });
    }
    return out;
}

// Mean squared distance between A and B rotated by `shift`.
double pairingCost(const std::vector<Vec2>& a, const std::vector<Vec2>& b, std::size_t shift)
{
    const std::size_t n = a.size();
    double total = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const Vec2& p = a[i];
        const Vec2& q = b[(i + shift) % n];
        const double dx = p.x - q.x;
        const double dy = p.y - q.y;
        total += dx * dx + dy * dy;
    }
    return total / static_cast<double>(n);
}

struct Alignment
{
    bool reversed { false };
    std::size_t shift { 0 };
    double cost { 0.0 };
    bool valid { false };
};

// Reversing must happen BEFORE resampling, never after.
//
// Reversing an array of n samples maps index i to n-1-i, so it shifts the phase
// by one sample -- and one sample is a different angle at 256 samples than at
// 2 000. Reverse after resampling and the coarse answer no longer scales to the
// fine one: on a 5 km circuit the two disagree by about 1.2 degrees, which
// walks the pairing a metre and a half sideways and quietly widens every track.
// Reversing the polyline instead makes the two resolutions resamplings of the
// same curve, and the shift scales exactly.
std::vector<Vec2> maybeReverse(const std::vector<Vec2>& points, bool reversed)
{
    if (!reversed)
    {
        return points;
    }
    return { points.rbegin(), points.rend() };
}

// Which way round the inner loop runs, and by how much it is out of phase.
//
// BOTH have to be searched. The source winding says nothing -- Monza and the
// Nordschleife wind their two loops the same way, Laguna Seca winds them
// oppositely, and there is no property in the file that distinguishes them. And
// the phase is arbitrary because the two loops were traced starting from
// wherever the tracer happened to start.
//
// Coarse then fine, rather than a full search at the working resolution: a full
// search is O(samples^2) and 2 000 samples is four million distance evaluations
// per direction per track, times 994 tracks. The coarse pass costs 1/60th of
// that and gets the shift to within one coarse step, which the fine pass then
// only has to search either side of.
Alignment align(const std::vector<Vec2>& outer, const std::vector<Vec2>& inner,
                std::size_t coarseSamples, std::size_t fineSamples)
{
    Alignment best;

    const auto coarseA = resampleClosed(outer, coarseSamples);
    if (coarseA.size() != coarseSamples)
    {
        return best;
    }

    for (int reversed = 0; reversed < 2; ++reversed)
    {
        const auto coarseB = resampleClosed(maybeReverse(inner, reversed != 0), coarseSamples);
        if (coarseB.size() != coarseSamples)
        {
            return best;
        }
        for (std::size_t shift = 0; shift < coarseSamples; ++shift)
        {
            const double cost = pairingCost(coarseA, coarseB, shift);
            if (!best.valid || cost < best.cost)
            {
                best = { reversed != 0, shift, cost, true };
            }
        }
    }

    if (!best.valid)
    {
        return best;
    }

    // Rescale the coarse shift and search either side of it. The window is one
    // coarse step plus a margin, which is the most the coarse answer can be
    // wrong by.
    const double scale = static_cast<double>(fineSamples) / static_cast<double>(coarseSamples);
    const auto centre = static_cast<std::ptrdiff_t>(std::llround(
        static_cast<double>(best.shift) * scale));
    const auto window = static_cast<std::ptrdiff_t>(std::ceil(scale)) + 4;

    const auto fineA = resampleClosed(outer, fineSamples);
    const auto fineB = resampleClosed(maybeReverse(inner, best.reversed), fineSamples);
    if (fineA.size() != fineSamples || fineB.size() != fineSamples)
    {
        return best;
    }

    const auto span = static_cast<std::ptrdiff_t>(fineSamples);
    Alignment fine { best.reversed, 0, 0.0, false };
    for (std::ptrdiff_t offset = -window; offset <= window; ++offset)
    {
        const auto shift = static_cast<std::size_t>(((centre + offset) % span + span) % span);
        const double cost = pairingCost(fineA, fineB, shift);
        if (!fine.valid || cost < fine.cost)
        {
            fine = { best.reversed, shift, cost, true };
        }
    }
    return fine.valid ? fine : best;
}

// Where a thin closed ribbon folds back on itself.
//
// A point-to-point course has no seam -- it never returns to its start -- so
// the split cannot be found the way a circuit's is. What it has instead is TWO
// FOLDS, one at each end of the course, and between them two edges running
// alongside each other in opposite directions around the same closed curve.
//
// So the pairing is ANTIPODAL rather than phase-shifted: walking out from a
// fold at sample `r`, sample `r+i` is on one edge and sample `r-i` is on the
// other, directly across the track from it. Finding the fold is a search over
// `r` for the offset that makes those pairs closest, which is the same
// coarse-then-fine shape as the circuit's phase search and for the same reason.
struct Fold
{
    std::size_t index { 0 };
    double cost { 0.0 };
    bool valid { false };
};

double antipodalCost(const std::vector<Vec2>& ring, std::size_t r)
{
    const std::size_t n = ring.size();
    const std::size_t half = n / 2;
    double total = 0.0;
    for (std::size_t i = 0; i <= half; ++i)
    {
        const Vec2& a = ring[(r + i) % n];
        const Vec2& b = ring[(r + n - (i % n)) % n];
        const double dx = a.x - b.x;
        const double dy = a.y - b.y;
        total += dx * dx + dy * dy;
    }
    return total / static_cast<double>(half + 1);
}

Fold findFold(const std::vector<Vec2>& boundary, std::size_t coarseSamples,
              std::size_t fineSamples)
{
    Fold best;

    const auto coarse = resampleClosed(boundary, coarseSamples);
    if (coarse.size() != coarseSamples)
    {
        return best;
    }
    for (std::size_t r = 0; r < coarseSamples; ++r)
    {
        const double cost = antipodalCost(coarse, r);
        if (!best.valid || cost < best.cost)
        {
            best = { r, cost, true };
        }
    }
    if (!best.valid)
    {
        return best;
    }

    const auto fine = resampleClosed(boundary, fineSamples);
    if (fine.size() != fineSamples)
    {
        return best;
    }

    const double scale = static_cast<double>(fineSamples) / static_cast<double>(coarseSamples);
    const auto centre =
        static_cast<std::ptrdiff_t>(std::llround(static_cast<double>(best.index) * scale));
    const auto window = static_cast<std::ptrdiff_t>(std::ceil(scale)) + 4;
    const auto span = static_cast<std::ptrdiff_t>(fineSamples);

    Fold refined;
    for (std::ptrdiff_t offset = -window; offset <= window; ++offset)
    {
        const auto r = static_cast<std::size_t>(((centre + offset) % span + span) % span);
        const double cost = antipodalCost(fine, r);
        if (!refined.valid || cost < refined.cost)
        {
            refined = { r, cost, true };
        }
    }
    return refined.valid ? refined : best;
}

double median(std::vector<double>& values)
{
    if (values.empty())
    {
        return 0.0;
    }
    const std::size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(middle),
                     values.end());
    return values[middle];
}

Ring toRing(const std::vector<Vec2>& points, const Frame& frame)
{
    Ring out;
    out.reserve(points.size() * 2);
    for (const Vec2& p : points)
    {
        osm::Coord lat = 0;
        osm::Coord lon = 0;
        frame.toCoord(p, lat, lon);
        out.push_back(lat);
        out.push_back(lon);
    }
    return out;
}

Ring slice(const Ring& ring, std::size_t first, std::size_t last)
{
    Ring out;
    if (last <= first)
    {
        return out;
    }
    out.reserve((last - first) * 2);
    for (std::size_t i = first; i < last; ++i)
    {
        out.push_back(ring[2 * i]);
        out.push_back(ring[2 * i + 1]);
    }
    return out;
}

// Where a ray from `origin` along `direction` first meets a closed polyline,
// as a distance. Absent when it does not within `limitM`.
std::optional<double> rayHit(const std::vector<Vec2>& ring, Vec2 origin, Vec2 direction,
                             double limitM)
{
    std::optional<double> nearest;
    const std::size_t count = ring.size();
    for (std::size_t i = 0; i < count; ++i)
    {
        const Vec2& a = ring[i];
        const Vec2& b = ring[(i + 1) % count];
        const double ex = b.x - a.x;
        const double ey = b.y - a.y;
        const double denominator = direction.x * ey - direction.y * ex;
        if (std::abs(denominator) < 1e-12)
        {
            continue;
        }
        const double px = a.x - origin.x;
        const double py = a.y - origin.y;
        const double t = (px * ey - py * ex) / denominator;
        const double u = (px * direction.y - py * direction.x) / -denominator;
        if (t < 0.0 || t > limitM || u < 0.0 || u > 1.0)
        {
            continue;
        }
        if (!nearest.has_value() || t < *nearest)
        {
            nearest = t;
        }
    }
    return nearest;
}

std::uint16_t toHalfWidthCm(double widthM)
{
    const double halfCm = std::clamp(widthM * 50.0, 0.0, 65535.0);
    return static_cast<std::uint16_t>(halfCm);
}

} // namespace

const char* to_string(Quality quality)
{
    switch (quality)
    {
        case Quality::Unknown:
            return "unknown";
        case Quality::Ok:
            return "ok";
        case Quality::SeamNotFound:
            return "seam-not-found";
        case Quality::MultipleLoops:
            return "multiple-loops";
        case Quality::WidthOutOfRange:
            return "width-out-of-range";
        case Quality::LengthMismatch:
            return "length-mismatch";
        case Quality::SourceLengthImplausible:
            return "source-length-implausible";
        case Quality::Degenerate:
            return "degenerate";
    }
    return "unknown";
}

const char* to_string(Topology topology)
{
    switch (topology)
    {
        case Topology::Unknown:
            return "unknown";
        case Topology::Annulus:
            return "annulus";
        case Topology::Ribbon:
            return "ribbon";
    }
    return "unknown";
}

const char* to_string(GateResult result)
{
    switch (result)
    {
        case GateResult::Placed:
            return "placed";
        case GateResult::NoPoint:
            return "no-point";
        case GateResult::Ambiguous:
            return "ambiguous";
        case GateResult::OffTrack:
            return "off-track";
        case GateResult::NoCenterline:
            return "no-centerline";
    }
    return "unknown";
}

double ringLengthM(const Ring& ring, bool closed)
{
    const Frame frame(ring);
    return polylineLength(project(ring, frame), closed);
}

Seam findSeam(const Ring& ring, double maxGapM)
{
    Seam seam;
    const std::size_t count = pointCount(ring);
    if (count < 20)
    {
        return seam;
    }

    const Frame frame(ring);
    const auto points = project(ring, frame);

    // The window excludes the ends. Point 0 is trivially at distance zero from
    // itself and its immediate neighbours are within a vertex spacing, so
    // without this the answer is always index 1.
    const std::size_t first = std::max<std::size_t>(5, count / 32);
    const std::size_t last = count - std::max<std::size_t>(5, count / 32);
    if (last <= first)
    {
        return seam;
    }

    // Walk the window collecting DISTINCT excursions below the threshold rather
    // than taking the global minimum. A file holding several loops -- an oval
    // and its infield, or one of the "Combo" layouts -- returns to its start
    // once per loop, and the global minimum would silently pick one of them and
    // call the result a track.
    std::vector<std::pair<std::size_t, double>> candidates;
    const std::size_t separation = std::max<std::size_t>(1, count / 50);

    std::size_t i = first;
    while (i < last)
    {
        const double gap = dist(points[i], points[0]);
        if (gap > maxGapM)
        {
            ++i;
            continue;
        }

        // Inside an excursion: take its minimum, then skip past it.
        std::size_t localIndex = i;
        double localGap = gap;
        std::size_t j = i;
        while (j < last && dist(points[j], points[0]) <= maxGapM)
        {
            const double here = dist(points[j], points[0]);
            if (here < localGap)
            {
                localGap = here;
                localIndex = j;
            }
            ++j;
        }
        candidates.emplace_back(localIndex, localGap);
        i = j + separation;
    }

    if (candidates.empty())
    {
        return seam;
    }

    const auto best = std::min_element(
        candidates.begin(), candidates.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });

    seam.index = best->first;
    seam.gapM = best->second;
    seam.found = true;

    // A RIVAL loop, not merely another close approach.
    //
    // The seam threshold is deliberately generous -- 32 of the 43 files that do
    // not close to within 2 m are ordinary circuits whose trace closes sloppily
    // -- but counting every approach inside it as a second loop is far too
    // eager: plenty of circuits pass within 25 m of their own start halfway
    // round a lap.
    //
    // What separates the two is not how CLOSE the approach is but how close it
    // is COMPARED TO THE BEST. A file holding several loops revisits the same
    // vertex each time, so every return is equally exact: Charlotte's oval and
    // its infield both come back to 0.00 m. A circuit that merely passes nearby
    // does not: Fiorano's best return is 0.00 m and its near miss is 4.42 m,
    // and Adria closes at 16.21 m while passing 26.36 m away elsewhere. Both
    // were called multi-loop by a rule with an absolute floor, and both are
    // ordinary circuits.
    const double rivalGapM = std::max(best->second * 1.5, best->second + 1.0);
    for (const auto& [index, gap] : candidates)
    {
        if (gap <= rivalGapM)
        {
            ++seam.returns;
        }
    }
    return seam;
}

namespace
{

// Turn a set of across-track sample pairs into a centreline.
//
// Shared by both topologies, because everything after the pairing is the same
// work: average each pair, measure the distance between them as the local
// width, and accumulate arc length.
Centerline buildCenterline(const std::vector<Vec2>& sideA, const std::vector<Vec2>& sideB,
                           bool closed, const Frame& frame)
{
    const std::size_t count = std::min(sideA.size(), sideB.size());

    std::vector<Vec2> centre;
    centre.reserve(count);
    std::vector<double> widths;
    widths.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        centre.push_back({ (sideA[i].x + sideB[i].x) * 0.5, (sideA[i].y + sideB[i].y) * 0.5 });
        widths.push_back(dist(sideA[i], sideB[i]));
    }

    Centerline line;
    line.closed = closed;
    line.points = toRing(centre, frame);
    line.lengthM = polylineLength(centre, closed);

    std::vector<double> widthCopy = widths;
    line.medianWidthM = median(widthCopy);

    line.distanceCm.reserve(count);
    line.halfWidthCm.reserve(count);
    double travelled = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (i > 0)
        {
            travelled += dist(centre[i - 1], centre[i]);
        }
        line.distanceCm.push_back(static_cast<std::uint32_t>(std::llround(travelled * 100.0)));
        line.halfWidthCm.push_back(toHalfWidthCm(widths[i]));
    }
    return line;
}

// A centreline joins up when its two ends are in the same place, and that --
// not the feature type, and not the source's `closed` property -- is what says
// whether a track is a circuit or a point-to-point course. See the note at the
// top of the header for why nothing else can.
bool endsMeet(const Centerline& line, const Frame& frame, double fraction)
{
    const std::size_t count = pointCount(line.points);
    if (count < 2 || line.lengthM <= 0.0)
    {
        return true;
    }
    const auto points = project(line.points, frame);
    return dist(points.front(), points.back()) / line.lengthM <= fraction;
}

// THE CIRCUIT CASE: two closed boundary curves joined at a seam.
std::optional<Centerline> deriveAnnulus(const std::vector<Vec2>& outerLocal,
                                        const std::vector<Vec2>& innerLocal, const Frame& frame,
                                        const DeriveOptions& options)
{
    const std::size_t fine = std::max<std::size_t>(64, options.samples);
    const std::size_t coarse = std::min<std::size_t>(256, fine);

    const Alignment alignment = align(outerLocal, innerLocal, coarse, fine);
    if (!alignment.valid)
    {
        return std::nullopt;
    }

    const auto sampledOuter = resampleClosed(outerLocal, fine);
    const auto sampledInner = resampleClosed(maybeReverse(innerLocal, alignment.reversed), fine);
    if (sampledOuter.size() != fine || sampledInner.size() != fine)
    {
        return std::nullopt;
    }

    std::vector<Vec2> shifted;
    shifted.reserve(fine);
    for (std::size_t i = 0; i < fine; ++i)
    {
        shifted.push_back(sampledInner[(i + alignment.shift) % fine]);
    }

    Centerline line = buildCenterline(sampledOuter, shifted, true, frame);
    line.reversed = alignment.reversed;
    return line;
}

// THE POINT-TO-POINT CASE: one closed boundary curve with a fold at each end.
std::optional<Centerline> deriveRibbon(const std::vector<Vec2>& boundary, const Frame& frame,
                                       const DeriveOptions& options)
{
    // Even, because the pairing walks out from the fold in both directions at
    // once and the two halves have to meet.
    std::size_t fine = std::max<std::size_t>(64, options.samples);
    fine += fine % 2;
    const std::size_t coarse = std::min<std::size_t>(256, fine);

    const Fold fold = findFold(boundary, coarse, fine);
    if (!fold.valid)
    {
        return std::nullopt;
    }

    const auto sampled = resampleClosed(boundary, fine);
    if (sampled.size() != fine)
    {
        return std::nullopt;
    }

    const std::size_t half = fine / 2;
    std::vector<Vec2> sideA;
    std::vector<Vec2> sideB;
    sideA.reserve(half + 1);
    sideB.reserve(half + 1);
    for (std::size_t i = 0; i <= half; ++i)
    {
        sideA.push_back(sampled[(fold.index + i) % fine]);
        sideB.push_back(sampled[(fold.index + fine - (i % fine)) % fine]);
    }

    return buildCenterline(sideA, sideB, false, frame);
}

// How well a derivation came out, so the two topologies can be compared on
// their results rather than guessed at from the source.
double scoreOf(const Centerline& line, double publishedLengthM, const DeriveOptions& options)
{
    double score = 0.0;
    if (line.medianWidthM < options.minWidthM || line.medianWidthM > options.maxWidthM)
    {
        // Far and away the strongest signal. A mispairing does not miss by a
        // little: it produces a "track" hundreds of metres wide.
        score += 1000.0;
    }
    score += line.medianWidthM;
    if (publishedLengthM > 0.0)
    {
        score += 100.0 * std::abs(line.lengthM - publishedLengthM) / publishedLengthM;
    }
    return score;
}

// Every boundary curve of a multi-loop outline, split where the trace returns
// to its start. The seam vertex is the repeated start point and belongs to the
// curve that closes on it, so each split drops it.
std::vector<Ring> splitLoops(const Ring& ring, const Seam& seam, double maxGapM)
{
    const std::size_t count = pointCount(ring);
    const Frame frame(ring);
    const auto points = project(ring, frame);

    const std::size_t first = std::max<std::size_t>(5, count / 32);
    const std::size_t last = count - std::max<std::size_t>(5, count / 32);
    const std::size_t separation = std::max<std::size_t>(1, count / 50);
    const double rivalGapM = std::max(5.0, seam.gapM * 1.5);

    std::vector<std::size_t> cuts { 0 };
    std::size_t i = first;
    while (i < last)
    {
        if (dist(points[i], points[0]) > maxGapM)
        {
            ++i;
            continue;
        }
        std::size_t localIndex = i;
        double localGap = dist(points[i], points[0]);
        std::size_t j = i;
        while (j < last && dist(points[j], points[0]) <= maxGapM)
        {
            const double here = dist(points[j], points[0]);
            if (here < localGap)
            {
                localGap = here;
                localIndex = j;
            }
            ++j;
        }
        if (localGap <= rivalGapM)
        {
            cuts.push_back(localIndex);
        }
        i = j + separation;
    }
    cuts.push_back(count);

    std::vector<Ring> out;
    for (std::size_t k = 0; k + 1 < cuts.size(); ++k)
    {
        const std::size_t from = k == 0 ? cuts[k] : cuts[k] + 1;
        Ring curve = slice(ring, from, cuts[k + 1]);
        if (pointCount(curve) >= 8)
        {
            out.push_back(std::move(curve));
        }
    }
    return out;
}

} // namespace

Derived derive(const Ring& ring, double publishedLengthM, bool sourceSaysDegenerate,
               const DeriveOptions& options)
{
    Derived derived;
    derived.publishedLengthM = publishedLengthM;

    const std::size_t count = pointCount(ring);
    if (count < 20 || sourceSaysDegenerate)
    {
        // The source's own `degenerate` flag, honoured rather than rediscovered.
        // Exactly one file carries it, and it is 329 points on a flat line at
        // constant latitude in the Atlantic -- a shape that would otherwise be
        // diagnosed by whichever geometric check happened to trip first.
        derived.quality = Quality::Degenerate;
        return derived;
    }

    const Frame frame(ring);
    const auto points = project(ring, frame);
    derived.outlineLengthM = polylineLength(points, true);

    derived.seam = findSeam(ring, options.seamMaxGapM);

    // SEVERAL LOOPS FIRST, before the length check below.
    //
    // A file holding four loops has an outline twice as long as it should be,
    // so both diagnoses fire -- but they are not equally useful. "Several loops
    // concatenated" says what is in the file; "the outline disagrees with the
    // published length" only reports the symptom, and sends somebody measuring.
    if (derived.seam.returns > 1)
    {
        derived.quality = Quality::MultipleLoops;
        // NOT outer/inner. Splitting at the first seam and calling the whole
        // remainder a hole punches a 52 km "infield" out of a 2 km curve on
        // Buenos Aires F. The curves are handed over whole instead, for the
        // caller to pair.
        derived.loops = splitLoops(ring, derived.seam, options.seamMaxGapM);
        return derived;
    }

    // Half the outline is the mean of the two sides, which is what a centreline
    // between them must come to -- and it holds for BOTH topologies: Pikes
    // Peak's 39 745 m perimeter is twice its 19 915 m course, exactly as a
    // circuit's is.
    //
    // THIS IS THE CHECK THAT MATTERS, and it is the one that does not depend on
    // the published length at all. See the note on publishedLengthDisagrees.
    const double halfOutline = derived.outlineLengthM * 0.5;

    // ---- the circuit reading -------------------------------------------
    std::optional<Centerline> annulus;
    if (derived.seam.found)
    {
        // The seam VERTEX belongs to neither loop. It is where the trace came
        // back to where it started, so it is the outer loop's repeated closing
        // point, and every ring in this tree leaves that implied. Kept in the
        // inner loop it would put a spike from the inner boundary out to the
        // outer one -- which draws as a hairline crack across the track and
        // shortens the inner loop's arc length enough to bias the pairing.
        Ring outer = slice(ring, 0, derived.seam.index);
        Ring inner = slice(ring, derived.seam.index + 1, count);
        if (pointCount(outer) >= 8 && pointCount(inner) >= 8)
        {
            annulus = deriveAnnulus(project(outer, frame), project(inner, frame), frame, options);
            if (annulus.has_value())
            {
                derived.outer = std::move(outer);
                derived.inner = std::move(inner);
            }
        }
    }

    // ---- the point-to-point reading -------------------------------------
    const std::optional<Centerline> ribbon = deriveRibbon(points, frame, options);

    // Whichever came out better. Both are tried because the topology is not
    // knowable in advance -- every outline here is closed, so nothing in the
    // source distinguishes them.
    std::optional<Centerline> chosen;
    if (annulus.has_value() && ribbon.has_value())
    {
        chosen = scoreOf(*annulus, publishedLengthM, options) <=
                         scoreOf(*ribbon, publishedLengthM, options)
                     ? annulus
                     : ribbon;
    }
    else if (annulus.has_value())
    {
        chosen = annulus;
    }
    else if (ribbon.has_value())
    {
        chosen = ribbon;
    }

    if (!chosen.has_value())
    {
        derived.quality = Quality::SeamNotFound;
        return derived;
    }

    // The final word on open versus closed comes from the geometry that came
    // out, whichever branch produced it: a ribbon whose ends meet is a circuit
    // traced the other way round, and an annulus split whose ends do not meet
    // was not an annulus.
    chosen->closed = endsMeet(*chosen, frame, options.closedEndGapFraction);
    derived.topology = chosen->closed ? Topology::Annulus : Topology::Ribbon;

    if (derived.topology == Topology::Ribbon)
    {
        // A point-to-point course's surface has no infield, so there is no hole
        // to cut and the outline draws as one solid shape.
        derived.outer.clear();
        derived.inner.clear();
    }
    if (publishedLengthM > 0.0)
    {
        derived.lengthErrorFraction = (chosen->lengthM - publishedLengthM) / publishedLengthM;
        derived.publishedLengthDisagrees =
            std::abs(derived.lengthErrorFraction) > options.lengthTolerance;
    }

    if (chosen->medianWidthM < options.minWidthM || chosen->medianWidthM > options.maxWidthM)
    {
        derived.quality = Quality::WidthOutOfRange;
        derived.centerline = std::move(chosen);
        return derived;
    }

    if (halfOutline > 0.0 &&
        std::abs(chosen->lengthM - halfOutline) / halfOutline > options.lengthTolerance)
    {
        derived.quality = Quality::LengthMismatch;
        derived.centerline = std::move(chosen);
        return derived;
    }

    derived.quality = Quality::Ok;
    derived.centerline = std::move(chosen);
    return derived;
}

GateResult placeGate(Centerline& centerline, const Ring& outer, const Ring& inner,
                     osm::Coord pointLat, osm::Coord pointLon, Gate& gate)
{
    const std::size_t count = pointCount(centerline.points);
    if (count < 8 || centerline.distanceCm.size() != count)
    {
        return GateResult::NoCenterline;
    }

    const Frame frame(centerline.points);
    const auto line = project(centerline.points, frame);
    const auto outerLocal = project(outer, frame);
    const auto innerLocal = project(inner, frame);
    const Vec2 target = frame.toLocal(pointLat, pointLon);

    std::size_t nearest = 0;
    double nearestDistance = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const double d = dist(line[i], target);
        if (i == 0 || d < nearestDistance)
        {
            nearestDistance = d;
            nearest = i;
        }
    }

    // On the tarmac, not merely near it -- but measured against the CENTRELINE
    // and its local width, not by a crossing test against the two loops.
    //
    // The crossing test is the obvious implementation and it is wrong here for
    // a reason the data makes plain: these start/finish points sit essentially
    // ON the outline, 1-10 m from the nearest boundary vertex and usually
    // equidistant from two of them, i.e. on an edge. Even-odd parity for a
    // point on an edge is a coin flip, and it rejected 310 of 844 perfectly
    // good gates. Distance from the centreline, allowed out to the half width
    // plus slack, asks the question that was actually meant.
    const double halfWidthM = static_cast<double>(centerline.halfWidthCm[nearest]) / 100.0;
    const double allowance = std::max(3.0, halfWidthM * 0.5);
    if (nearestDistance > halfWidthM + allowance)
    {
        return GateResult::OffTrack;
    }

    // ROTATION IS ONLY MEANINGFUL FOR A CLOSED CENTRELINE.
    //
    // On a circuit the lap has no natural beginning, so putting the gate at
    // sample zero is what makes the distances run forward from it -- rebasing
    // them in place instead would leave them wrapping mid-array and every
    // consumer would have to special-case the discontinuity.
    //
    // A point-to-point course is the opposite: it HAS a beginning, the fold at
    // one end, and rotating an open polyline does not renumber it -- it tears it
    // in half and joins the two ends across the map. So the distances are left
    // running from the start of the course and the gate records where along it
    // it falls.
    std::size_t gateIndex = nearest;
    if (centerline.closed)
    {
        std::rotate(centerline.points.begin(),
                    centerline.points.begin() + static_cast<std::ptrdiff_t>(2 * nearest),
                    centerline.points.end());
        std::rotate(centerline.halfWidthCm.begin(),
                    centerline.halfWidthCm.begin() + static_cast<std::ptrdiff_t>(nearest),
                    centerline.halfWidthCm.end());
        gateIndex = 0;

        auto rotated = project(centerline.points, frame);
        double travelled = 0.0;
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i > 0)
            {
                travelled += dist(rotated[i - 1], rotated[i]);
            }
            centerline.distanceCm[i] =
                static_cast<std::uint32_t>(std::llround(travelled * 100.0));
        }
    }

    const auto rotated = project(centerline.points, frame);

    // The tangent at the gate, and the perpendicular the gate lies along. On an
    // open centreline the neighbours are clamped rather than wrapped: sample
    // zero's predecessor is the far end of the course, and a tangent taken
    // across that would point sideways.
    const Vec2 ahead = rotated[centerline.closed ? (gateIndex + 1) % count
                                                 : std::min(gateIndex + 1, count - 1)];
    const Vec2 behind = rotated[centerline.closed ? (gateIndex + count - 1) % count
                                                  : (gateIndex > 0 ? gateIndex - 1 : 0)];
    double tx = ahead.x - behind.x;
    double ty = ahead.y - behind.y;
    const double norm = std::hypot(tx, ty);
    if (norm <= 0.0)
    {
        return GateResult::NoCenterline;
    }
    tx /= norm;
    ty /= norm;
    const Vec2 left { -ty, tx };
    const Vec2 right { ty, -tx };

    constexpr double kCastLimitM = 200.0;
    const double fallback = static_cast<double>(centerline.halfWidthCm[gateIndex]) / 100.0;
    const auto leftHit = rayHit(outerLocal, rotated[gateIndex], left, kCastLimitM);
    const auto rightHit = rayHit(innerLocal, rotated[gateIndex], right, kCastLimitM);
    // The cast can miss on either side -- the gate can fall where the outline
    // is locally concave, or where a pit entry breaks the ribbon. The paired
    // half width is the answer the derivation already computed, so falling back
    // to it costs accuracy on one side and never fails.
    const double leftM = leftHit.value_or(fallback);
    const double rightM = rightHit.value_or(fallback);

    const Vec2 at = rotated[gateIndex];
    frame.toCoord(at, gate.centreLat, gate.centreLon);
    frame.toCoord({ at.x + left.x * leftM, at.y + left.y * leftM }, gate.leftLat, gate.leftLon);
    frame.toCoord({ at.x + right.x * rightM, at.y + right.y * rightM }, gate.rightLat,
                  gate.rightLon);
    gate.widthM = leftM + rightM;
    // The residual: how far sample zero sits from the point the file gave. Less
    // than one sample spacing by construction, and worth carrying so a consumer
    // knows the lap does not start at a rounded-off place by accident.
    // On a circuit this is the residual -- how far sample zero sits from the
    // point the file gave, under one sample spacing by construction. On an open
    // course it is the real thing: how far along the course the gate is.
    gate.centerlineOffsetCm =
        centerline.closed
            ? static_cast<std::uint32_t>(std::llround(dist(at, target) * 100.0))
            : centerline.distanceCm[gateIndex];
    gate.present = true;
    return GateResult::Placed;
}

double principalAxisDeg(const Ring& ring)
{
    const std::size_t count = pointCount(ring);
    if (count < 3)
    {
        return 0.0;
    }

    const Frame frame(ring);
    const auto points = project(ring, frame);

    double meanX = 0.0;
    double meanY = 0.0;
    for (const Vec2& p : points)
    {
        meanX += p.x;
        meanY += p.y;
    }
    meanX /= static_cast<double>(points.size());
    meanY /= static_cast<double>(points.size());

    double sxx = 0.0;
    double syy = 0.0;
    double sxy = 0.0;
    for (const Vec2& p : points)
    {
        const double dx = p.x - meanX;
        const double dy = p.y - meanY;
        sxx += dx * dx;
        syy += dy * dy;
        sxy += dx * dy;
    }

    // The dominant eigenvector of the 2x2 covariance, as an angle. The closed
    // form is cheaper and steadier than an iteration for a matrix this size.
    const double angle = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    const double eastComponent = std::cos(angle);
    const double northComponent = std::sin(angle);

    double degrees = std::atan2(eastComponent, northComponent) * 180.0 / std::numbers::pi;
    // An axis, not a direction: 190 degrees and 10 degrees are the same line,
    // and a consumer rotating a view by one or the other must get the same
    // picture.
    while (degrees < 0.0)
    {
        degrees += 180.0;
    }
    while (degrees >= 180.0)
    {
        degrees -= 180.0;
    }
    return degrees;
}

} // namespace map_build::track
