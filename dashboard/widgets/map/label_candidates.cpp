// SPDX-License-Identifier: GPL-3.0-or-later
//
// Turning a decoded tile into label candidates. The DECODE WORKER's half of
// the label pass.
//
// Split from labels.cpp along the thread boundary rather than by subject:
// everything here runs once per tile, on the zenoh worker that decoded it,
// and never touches a camera, a style or a painter. Everything in labels.cpp
// runs once per frame on the GUI thread and does nothing else. Keeping them in
// one file meant one set of includes, one set of file-local helpers and no
// signal at all about which side of that line a change was on.
//
// map/label_candidates.h is deliberately QtCore-only -- it is what the tile
// cache stores -- and nothing added here may break that.
#include "map/label_candidates.h"

#include "map/labels.h"

// For roadPriority(): the ladder that decides which layer a road is drawn in
// is the same one that decides whose name survives a collision.
#include "map/tessellator.h"

#include <QString>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace map_widget
{
namespace
{

// Tiers step by TWO, not one, so that a layer can sit exactly half way between
// two of them. The track layer is the reason: it is documented as ranking
// between a town and a city, and on a unit scale there is no such number.
constexpr int kPlaceTierStep = 2;

// Point labels sit AT a coordinate; line labels sit along one. The difference
// is not cosmetic -- a road's name has to be placed from geometry that may be
// forty miles long and clipped into a dozen tiles, and the same name then turns
// up once per tile it crosses.
enum class LabelGeometry
{
    Point,
    Line,
    // Whichever the feature happens to be. `water_name` is the reason: a lake
    // gives a POINT because its name sits inside it, and a river gives the
    // LINE because its name is drawn along it. One layer, two shapes, and
    // map_build says so in as many words (extract.cpp).
    Either,
};

struct LabelLayerSpec
{
    const char* sourceLayer;
    LabelRank (*priority)(const mvt::Layer&, const mvt::Feature&);
    LabelGeometry geometry { LabelGeometry::Point };
    // Place at most one label per distinct name. Only lines need it: a place
    // name appears once, in one tile, and duplicates from a stand-in ancestor
    // land on the same pixels and lose the collision test. A road crosses every
    // tile it passes through and each one carries the whole name, so without
    // this a single street is labelled a dozen times across the viewport.
    bool oneLabelPerName { false };
    // Which per-frame gate the extracted candidates answer to.
    LabelKind kind { LabelKind::Place };
};

constexpr std::array<LabelLayerSpec, 4> kLabelLayers { {
    { "place", placeRank, LabelGeometry::Point, false, LabelKind::Place },
    { "track_label", trackRank, LabelGeometry::Point, false, LabelKind::Track },
    { "transportation_name", roadRank, LabelGeometry::Line, /*oneLabelPerName=*/true,
      LabelKind::Road },
    // Rivers and lakes. Deduped by name like roads and for the same reason: a
    // river runs through every tile it crosses and each one carries the whole
    // name.
    { "water_name", waterRank, LabelGeometry::Either, /*oneLabelPerName=*/true,
      LabelKind::Water },
} };

// How far a point may be moved to drop it from a run, in tile-local units.
//
// About half a pixel at the size a tile is drawn at when it is drawn at 1:1
// (512 px across), which is the zoom the archive built it for. Simplifying
// harder than the map is drawn would bend text off its road; simplifying not
// at all leaves the walk stepping over vertices no reader could resolve.
constexpr double kRunSimplifyTolerance = 0.5 / 512.0;

// A hard ceiling on a run's point count, applied after simplification.
//
// Simplification already handles every road in the archive; this is the
// backstop against a pathological way -- a traced coastline tagged as a
// track -- claiming a megabyte of arena on its own.
constexpr std::size_t kMaxRunPoints = 128;

// Douglas-Peucker, iteratively: keep the two ends, and recursively keep
// whichever point in between is furthest from the line joining them until
// nothing is further than the tolerance.
//
// Iterative rather than recursive because the input is untrusted: a run
// arrives from an archive, and a pathological one would otherwise recurse as
// deep as it has points.
void simplifyRun(const std::vector<LocalPoint>& in, double tolerance,
                 std::vector<LocalPoint>& out)
{
    if (in.size() <= 2)
    {
        out.assign(in.begin(), in.end());
        return;
    }

    std::vector<bool> keep(in.size(), false);
    keep.front() = true;
    keep.back() = true;

    std::vector<std::pair<std::size_t, std::size_t>> pending;
    pending.emplace_back(0, in.size() - 1);

    const double toleranceSq = tolerance * tolerance;
    while (!pending.empty())
    {
        const auto [first, last] = pending.back();
        pending.pop_back();
        if (last <= first + 1)
        {
            continue;
        }

        const double ax = double(in[first].x);
        const double ay = double(in[first].y);
        const double bx = double(in[last].x);
        const double by = double(in[last].y);
        const double dx = bx - ax;
        const double dy = by - ay;
        const double lengthSq = (dx * dx) + (dy * dy);

        double worst = -1.0;
        std::size_t worstAt = first;
        for (std::size_t i = first + 1; i < last; ++i)
        {
            const double px = double(in[i].x) - ax;
            const double py = double(in[i].y) - ay;

            // Distance to the SEGMENT, not the infinite line. A run that
            // doubles back on itself has both ends in the same place, and the
            // line through them is undefined -- there the distance is to the
            // point.
            double offX = px;
            double offY = py;
            if (lengthSq > 0.0)
            {
                const double t = std::clamp(((px * dx) + (py * dy)) / lengthSq, 0.0, 1.0);
                offX = px - (dx * t);
                offY = py - (dy * t);
            }
            const double distSq = (offX * offX) + (offY * offY);
            if (distSq > worst)
            {
                worst = distSq;
                worstAt = i;
            }
        }

        if (worst > toleranceSq)
        {
            keep[worstAt] = true;
            pending.emplace_back(first, worstAt);
            pending.emplace_back(worstAt, last);
        }
    }

    out.clear();
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        if (keep[i])
        {
            out.push_back(in[i]);
        }
    }

    // The backstop. Uniform decimation rather than a harder tolerance: this
    // only ever fires on geometry no label was going to follow sensibly
    // anyway, and a second Douglas-Peucker pass has no bound either.
    if (out.size() > kMaxRunPoints)
    {
        std::vector<LocalPoint> capped;
        capped.reserve(kMaxRunPoints);
        const double step = double(out.size() - 1) / double(kMaxRunPoints - 1);
        for (std::size_t i = 0; i < kMaxRunPoints; ++i)
        {
            capped.push_back(out[std::min(out.size() - 1, std::size_t(std::llround(double(i) * step)))]);
        }
        out.swap(capped);
    }
}

// Join the pieces MVT left behind.
//
// A way is clipped at every tile edge it crosses, so one feature arrives as
// several rings -- and a road that leaves the tile and comes back arrives as
// pieces that share an endpoint exactly. Joining them turns the longest
// PIECE into the longest RUN, which is a longer road to hang a name on and a
// better place to hang it. Exact equality is the right test: these are
// integer tile coordinates, and two pieces of one way meet on the same
// vertex or they are genuinely different roads.
void mergeRuns(std::vector<std::vector<LocalPoint>>& runs)
{
    auto same = [](const LocalPoint& a, const LocalPoint& b) {
        return a.x == b.x && a.y == b.y;
    };

    bool joined = true;
    while (joined)
    {
        joined = false;
        for (std::size_t i = 0; i < runs.size() && !joined; ++i)
        {
            if (runs[i].empty())
            {
                continue;
            }
            for (std::size_t j = i + 1; j < runs.size(); ++j)
            {
                if (runs[j].empty())
                {
                    continue;
                }
                std::vector<LocalPoint>& a = runs[i];
                std::vector<LocalPoint>& b = runs[j];

                if (same(a.back(), b.front()))
                {
                    a.insert(a.end(), b.begin() + 1, b.end());
                }
                else if (same(a.back(), b.back()))
                {
                    a.insert(a.end(), b.rbegin() + 1, b.rend());
                }
                else if (same(a.front(), b.back()))
                {
                    a.insert(a.begin(), b.begin(), b.end() - 1);
                }
                else if (same(a.front(), b.front()))
                {
                    std::reverse(a.begin(), a.end());
                    a.insert(a.end(), b.begin() + 1, b.end());
                }
                else
                {
                    continue;
                }

                b.clear();
                joined = true;
                break;
            }
        }
    }

    std::erase_if(runs, [](const std::vector<LocalPoint>& run) { return run.size() < 2; });
}

// One source layer's worth of candidates, extracted ONCE at decode time on
// the worker thread -- everything below is camera-free and never runs again
// for this tile. Gathered rather than placed even so: a label's position
// depends on which labels were already accepted, and the order tiles arrive
// in is decode order, which is not an order anybody chose.
void extractLayerLabels(const mvt::Tile& tile, const LabelLayerSpec& spec, LabelSet& out)
{
    const mvt::Layer* layer = tile.layer(spec.sourceLayer);
    if (layer == nullptr || layer->extent == 0)
    {
        return;
    }

    // Anchors leave here in [0,1] across the tile -- the same camera-free
    // domain the GPU's vertices use. The paint pass multiplies by the tile's
    // on-screen size, which is all a similarity transform needs.
    const double inv = 1.0 / double(layer->extent);

    // Reused across features rather than rebuilt per road: a dense tile holds
    // hundreds of named ways, and extraction runs on the decode worker where
    // every allocation is on the path a new tile takes to the screen.
    std::vector<std::vector<LocalPoint>> runs;
    std::vector<LocalPoint> simplified;

    for (const mvt::Feature& feature : layer->features)
    {
        const bool isPoint = feature.type == mvt::GeomType::Point;
        const bool isLine = feature.type == mvt::GeomType::LineString;
        if (feature.rings.empty() || (!isPoint && !isLine))
        {
            continue;
        }

        // A layer that takes either shape follows the feature; one that asks
        // for a shape ignores everything else it finds.
        const bool wantLine = spec.geometry == LabelGeometry::Either
                                  ? isLine
                                  : spec.geometry == LabelGeometry::Line;
        if (wantLine != isLine)
        {
            continue;
        }

        if (!wantLine)
        {
            if (feature.rings.front().empty())
            {
                continue;
            }
            const mvt::Point at = feature.rings.front().front();
            // name:latin, not name. This archive's tilemaker config emits only
            // the latin field, and reading `name` returns an empty string for
            // every place -- a map with no labels and no error anywhere.
            // map_build writes BOTH spellings for exactly this reason.
            const std::string text = layer->attributeText(feature, "name:latin");
            if (text.empty())
            {
                continue;
            }
            const LabelRank rank = spec.priority(*layer, feature);
            out.labels.push_back(LabelCandidate { QString::fromStdString(text),
                                                  double(at.x) * inv, double(at.y) * inv, 0.0,
                                                  rank.tier, rank.magnitude, spec.kind,
                                                  spec.oneLabelPerName });
            continue;
        }

        // A line label is placed ALONG the road, so what leaves here is the
        // road's shape and not just a point on it.
        //
        // Still anchored at the middle of the longest run, and still measured
        // in tile-local units: the tile-to-screen transform is a similarity,
        // so arc length, the halfway point along it, and which run is longest
        // are the same answers computed either side of it.
        runs.clear();
        for (const auto& ring : feature.rings)
        {
            if (ring.size() < 2)
            {
                continue;
            }
            std::vector<LocalPoint> run;
            run.reserve(ring.size());
            for (const mvt::Point& point : ring)
            {
                run.push_back(LocalPoint { float(double(point.x) * inv),
                                           float(double(point.y) * inv) });
            }
            runs.push_back(std::move(run));
        }

        // The pieces MVT clipped apart, put back together before the longest
        // one is chosen -- otherwise "longest" means longest fragment.
        mergeRuns(runs);

        double bestLocalLength = 0.0;
        const std::vector<LocalPoint>* best = nullptr;
        for (const std::vector<LocalPoint>& run : runs)
        {
            double length = 0.0;
            for (std::size_t i = 1; i < run.size(); ++i)
            {
                length += std::hypot(double(run[i].x) - double(run[i - 1].x),
                                     double(run[i].y) - double(run[i - 1].y));
            }
            if (length > bestLocalLength)
            {
                bestLocalLength = length;
                best = &run;
            }
        }

        if (best == nullptr || bestLocalLength <= 0.0)
        {
            continue;
        }

        std::string text = layer->attributeText(feature, "name:latin");
        if (text.empty())
        {
            // A numbered route with no name still has something to say, and
            // map_build emits it into this layer for exactly that reason.
            text = layer->attributeText(feature, "ref");
        }
        if (text.empty())
        {
            continue;
        }

        simplifyRun(*best, kRunSimplifyTolerance, simplified);
        if (simplified.size() < 2)
        {
            continue;
        }

        // Walk to the halfway point by arc length. This is where the name is
        // centred, and it is the only camera-free thing about the placement:
        // which way round the text runs, and where each character lands, are
        // decided per frame from the projected run.
        const double half = bestLocalLength / 2.0;
        double travelled = 0.0;
        LocalPoint anchor = simplified.front();
        for (std::size_t i = 1; i < simplified.size(); ++i)
        {
            const double dx = double(simplified[i].x) - double(simplified[i - 1].x);
            const double dy = double(simplified[i].y) - double(simplified[i - 1].y);
            const double segment = std::hypot(dx, dy);
            if (travelled + segment >= half)
            {
                const double t = segment > 0.0 ? (half - travelled) / segment : 0.0;
                anchor = LocalPoint { float(double(simplified[i - 1].x) + (dx * t)),
                                      float(double(simplified[i - 1].y) + (dy * t)) };
                break;
            }
            travelled += segment;
            anchor = simplified[i];
        }

        const LabelRank rank = spec.priority(*layer, feature);
        LabelCandidate candidate { QString::fromStdString(text), double(anchor.x),
                                   double(anchor.y), bestLocalLength, rank.tier,
                                   rank.magnitude, spec.kind, spec.oneLabelPerName };
        candidate.pathBegin = std::uint32_t(out.path.size());
        candidate.pathCount = std::uint32_t(simplified.size());
        out.path.insert(out.path.end(), simplified.begin(), simplified.end());
        out.labels.push_back(std::move(candidate));
    }
}

} // namespace

LabelSet extractLabels(const mvt::Tile& tile)
{
    LabelSet out;
    // All three layers, unconditionally: the show_* toggles and zoom floors
    // are the CAMERA's business and are applied per frame in paintLabels().
    // Extracting everything keeps the worker style-free, so a style edit
    // never needs a re-extract.
    for (const LabelLayerSpec& spec : kLabelLayers)
    {
        extractLayerLabels(tile, spec, out);
    }
    return out;
}

// map_rules writes a `rank` on every label point, LOW meaning important:
// country 0, state 1, city 2, town 3, village 4, hamlet 5, suburb 6,
// neighbourhood 7, locality 8 (libs/map_rules/src/classification.cpp).
//
// Preferred over the class name because it is the tiler's own ordering and
// stays right when a class is added upstream. The class is the fallback, for
// archives built before rank was written -- the bench archive is one, which is
// why this cannot simply require the attribute.
constexpr std::int64_t kMaxPlaceRank = 8;

int tierForRank(std::int64_t rank)
{
    return kPlaceTierStep * int(kMaxPlaceRank - std::clamp(rank, std::int64_t { 0 }, kMaxPlaceRank));
}

LabelRank placeRank(const mvt::Layer& layer, const mvt::Feature& feature)
{
    LabelRank out;

    const std::optional<double> rank = attributeNumber(layer, feature, "rank");
    out.tier = rank.has_value() ? tierForRank(std::int64_t(*rank))
                                : placePriority(layer.attributeText(feature, "class"));

    const std::optional<double> population = attributeNumber(layer, feature, "population");
    if (population.has_value() && *population > 0.0)
    {
        out.magnitude = std::uint32_t(std::clamp(*population, 0.0, 1.0e8));

        // The city/town line is drawn by local convention and moves by an order
        // of magnitude between countries, so a large town must be able to
        // outrank a small city rather than lose to it on the tag alone.
        //
        // The SAME thresholds map_rules already uses to promote a place's
        // minZoom (classification.cpp). Promoting the zoom but not the rank is
        // what produced a 400 000-strong town drawn from z6 and then labelled
        // beneath a city of 3 000.
        //
        // Upward only, for map_rules' reason: a city tagged with a small
        // population is far more often a stale tag than a tiny city.
        const int cityTier = tierForRank(2);
        const int townTier = tierForRank(3);
        if (out.magnitude >= 200'000)
        {
            out.tier = std::max(out.tier, cityTier);
        }
        else if (out.magnitude >= 50'000)
        {
            out.tier = std::max(out.tier, townTier);
        }
    }

    return out;
}

LabelRank trackRank(const mvt::Layer& layer, const mvt::Feature& feature)
{
    // Between a town and a city, which is the HALF step the doubled tier scale
    // exists to express. A circuit is a landmark worth seeing from a distance,
    // and it is the reason the driver is looking at this part of the map -- but
    // it must not push a city name off a country view.
    LabelRank out;
    out.tier = tierForRank(3) + 1;

    // map_build writes a length-derived rank on every circuit: 0 for the
    // longest, 20 for the shortest (tools/map_build/tracks.cpp). Inverted here
    // because magnitude sorts high-first, and used rather than ignored so that
    // where two circuits collide the bigger one keeps its name -- the sense is
    // easy to invert and the result is a map that labels the kart track and
    // hides Spa.
    constexpr double kMaxTrackRank = 20.0;
    const std::optional<double> rank = attributeNumber(layer, feature, "rank");
    out.magnitude =
        std::uint32_t(kMaxTrackRank - std::clamp(rank.value_or(kMaxTrackRank), 0.0, kMaxTrackRank));
    return out;
}

LabelRank roadRank(const mvt::Layer& layer, const mvt::Feature& feature)
{
    // Between a neighbourhood and a locality.
    //
    // Below every settlement worth the name, because a street name must not
    // push a town off the map -- and above `locality`, because at the zooms a
    // road label appears at, the street you are on is worth more than the name
    // of a road junction three miles away.
    LabelRank out;
    out.tier = tierForRank(kMaxPlaceRank) + 1;

    // Among roads, the bigger road wins the collision. roadPriority() is the
    // tessellator's own ladder -- motorway 4, trunk/primary 3, secondary 2,
    // minor 1, rail 5 -- reused rather than restated so the layer a road is
    // DRAWN in and the weight its name carries cannot drift apart.
    out.magnitude = std::uint32_t(roadPriority(layer.attributeText(feature, "class")));
    return out;
}

// The fallback for an archive whose label points carry no `rank`. Kept in step
// with map_rules' own table (classification.cpp) so that the two agree about
// which is the bigger place, and expressed through tierForRank() rather than as
// its own ladder of literals so they cannot drift apart.
//
// Returns a value on the same doubled scale placeLayerPriority() uses; only the
// ORDER is meaningful, never the number.
// How water bodies rank among themselves. Bigger, more permanent water first:
// a river is worth more than the ditch beside it, a lake more than a pond.
int waterPriority(std::string_view className)
{
    if (className == "ocean" || className == "sea")
    {
        return 6;
    }
    if (className == "lake")
    {
        return 5;
    }
    if (className == "river")
    {
        return 4;
    }
    if (className == "canal")
    {
        return 3;
    }
    if (className == "stream")
    {
        return 2;
    }
    // drain, ditch, and anything the archive has that we do not.
    return 1;
}

LabelRank waterRank(const mvt::Layer& layer, const mvt::Feature& feature)
{
    // One step below a road, and below a locality with it. See the note in
    // map/labels.h: on a map for driving, the street you are on outranks the
    // river you are crossing. Tiers step by two, so there is room underneath.
    LabelRank out;
    out.tier = tierForRank(kMaxPlaceRank) - 1;
    out.magnitude = std::uint32_t(waterPriority(layer.attributeText(feature, "class")));
    return out;
}

int placePriority(std::string_view className)
{
    if (className == "country")
    {
        return tierForRank(0);
    }
    if (className == "state" || className == "province")
    {
        return tierForRank(1);
    }
    if (className == "city")
    {
        return tierForRank(2);
    }
    if (className == "town")
    {
        return tierForRank(3);
    }
    if (className == "village")
    {
        return tierForRank(4);
    }
    if (className == "hamlet")
    {
        return tierForRank(5);
    }
    if (className == "suburb" || className == "quarter")
    {
        return tierForRank(6);
    }
    if (className == "neighbourhood")
    {
        return tierForRank(7);
    }
    // Everything unlisted, `locality` included, sorts last rather than
    // vanishing: an unknown class is a place we have no opinion about, not a
    // place that is not there.
    return tierForRank(kMaxPlaceRank);
}


} // namespace map_widget
