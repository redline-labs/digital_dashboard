// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <format>
#include <numbers>
#include <numeric>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "cli/output.h"
#include "map_build/extract.h"
#include "map_build/tiler.h"
#include "map_build/track_geometry.h"
#include "map_build/track_source.h"
#include "map_build/tracks.h"
#include "map_build/verbs.h"
#include "mbtiles/archive.h"
#include "mbtiles/writer.h"
#include "road_graph/geometry.h"
#include "track_store/store.h"

namespace map_build
{

namespace
{

namespace track = map_build::track;

// Union-find over bounding boxes.
//
// Several LAYOUTS share one venue -- Buttonwillow has fifteen, Buenos Aires
// twenty-three -- and they overlap on the ground because they are literally the
// same tarmac driven differently. Grouping them is a build-time fact about the
// geometry, so it is computed here once rather than rediscovered at runtime by
// anything that later has to ask "which of these am I on".
class DisjointSet
{
  public:
    explicit DisjointSet(std::size_t count) : mParent(count)
    {
        std::iota(mParent.begin(), mParent.end(), std::size_t { 0 });
    }

    std::size_t find(std::size_t i)
    {
        while (mParent[i] != i)
        {
            mParent[i] = mParent[mParent[i]];
            i = mParent[i];
        }
        return i;
    }

    void unite(std::size_t a, std::size_t b)
    {
        const std::size_t ra = find(a);
        const std::size_t rb = find(b);
        if (ra != rb)
        {
            mParent[rb] = ra;
        }
    }

  private:
    std::vector<std::size_t> mParent;
};

bool overlaps(const TrackBounds& a, const TrackBounds& b, double expandM)
{
    // Degrees of slack, widened east-west by latitude. A fixed degree margin
    // would be 300 m at the equator and 180 m at the Nordschleife, and the
    // clustering would then be latitude-dependent for no reason.
    constexpr double kMetresPerDegree = 111194.93;
    const double meanLat = (a.south + a.north + b.south + b.north) * 0.25;
    const double dLat = expandM / kMetresPerDegree;
    const double cosLat = std::max(0.05, std::cos(meanLat * std::numbers::pi / 180.0));
    const double dLon = expandM / (kMetresPerDegree * cosLat);

    return a.west - dLon <= b.east && b.west - dLon <= a.east && a.south - dLat <= b.north &&
           b.south - dLat <= a.north;
}

TrackBounds boundsOf(const track::Ring& ring)
{
    TrackBounds bounds;
    const std::size_t count = track::pointCount(ring);
    for (std::size_t i = 0; i < count; ++i)
    {
        const double lat = road_graph::toDegrees(ring[2 * i]);
        const double lon = road_graph::toDegrees(ring[2 * i + 1]);
        if (i == 0)
        {
            bounds = { lon, lat, lon, lat };
            continue;
        }
        bounds.west = std::min(bounds.west, lon);
        bounds.east = std::max(bounds.east, lon);
        bounds.south = std::min(bounds.south, lat);
        bounds.north = std::max(bounds.north, lat);
    }
    return bounds;
}

double area(const TrackBounds& b)
{
    return std::max(0.0, b.east - b.west) * std::max(0.0, b.north - b.south);
}

void assignVenues(std::vector<IngestedTrack>& tracks, double expandM)
{
    DisjointSet sets(tracks.size());
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        for (std::size_t j = i + 1; j < tracks.size(); ++j)
        {
            if (overlaps(tracks[i].bounds, tracks[j].bounds, expandM))
            {
                sets.unite(i, j);
            }
        }
    }

    // The venue is named after its LARGEST member, which is stable when a
    // smaller layout is added later -- the common case, since a venue gains
    // short configurations far more often than it gains a longer one than it
    // already had.
    std::map<std::size_t, std::size_t> largest;
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        const std::size_t root = sets.find(i);
        auto found = largest.find(root);
        if (found == largest.end() || area(tracks[i].bounds) > area(tracks[found->second].bounds))
        {
            largest[root] = i;
        }
    }
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        tracks[i].venueId = tracks[largest[sets.find(i)]].id;
    }
}

void writeReport(const std::filesystem::path& path, const std::vector<IngestedTrack>& tracks,
                 const std::vector<SkippedFile>& skipped)
{
    std::ofstream out(path);
    if (!out)
    {
        SPDLOG_ERROR("[tracks] cannot write report to {}", path.string());
        return;
    }

    out << "id,name,circuit,venue_id,quality,topology,gate,combo,closed,outline_points,"
           "outline_length_m,centerline_length_m,published_length_m,length_error_pct,"
           "published_disagrees,"
           "median_width_m,seam_index,seam_gap_m,seam_returns,west,south,east,north\n";
    for (const auto& t : tracks)
    {
        out << t.id << ',' << '"' << t.name << '"' << ',' << '"' << t.circuit << '"' << ','
            << t.venueId << ',' << track::to_string(t.derived.quality) << ','
            << track::to_string(t.derived.topology) << ','
            << track::to_string(t.gateResult) << ',' << (t.combo ? 1 : 0) << ','
            << (t.closed ? 1 : 0) << ',' << track::pointCount(t.outline) << ','
            << t.derived.outlineLengthM << ','
            << (t.derived.centerline ? t.derived.centerline->lengthM : 0.0) << ','
            << t.derived.publishedLengthM << ',' << t.derived.lengthErrorFraction * 100.0 << ','
            << (t.derived.publishedLengthDisagrees ? 1 : 0) << ','
            << (t.derived.centerline ? t.derived.centerline->medianWidthM : 0.0) << ','
            << t.derived.seam.index << ',' << t.derived.seam.gapM << ','
            << t.derived.seam.returns << ',' << t.bounds.west << ',' << t.bounds.south << ','
            << t.bounds.east << ',' << t.bounds.north << '\n';
    }
    // Skipped files go in the SAME report rather than only to the log. A file
    // that never became a track is the thing most likely to be missed, and a
    // reader scanning for it should not have to know to look somewhere else.
    for (const auto& s : skipped)
    {
        out << s.id << ",\"\",\"\",," << track::to_string(s.status) << ",,,,,,,,,,,,,,,,,,\n";
    }
}

// The layer names, as literals with static storage.
//
// DrawInput::layer takes a `const char*` and keeps it, because the layer names
// are a closed set and an owning string would cost bytes on every feature in
// the basemap to say nothing new. Naming them here is also what keeps
// map_rules::RenderClass out of this: RenderClass is a closed enum switched
// exhaustively in four files, and a track is not a road.
constexpr const char* kSurfaceLayer = "track";
constexpr const char* kCenterlineLayer = "track_centerline";
constexpr const char* kLabelLayer = "track_label";

DrawInput surfaceFeature(const IngestedTrack& t, std::uint8_t minZoom)
{
    DrawInput input;
    input.name = t.name;
    input.layer = kSurfaceLayer;
    input.classification.isArea = true;
    input.classification.minZoom = minZoom;
    input.classification.className = "surface";
    input.closed = true;

    // THE WHOLE REASON THE SEAM IS FOUND AT INGEST.
    //
    // The source ring is one loop with no hole, and its two halves are wound
    // the same way as often as oppositely. Handed over as a single ring it
    // tessellates into a filled blob with the infield painted over -- no error,
    // no log line, just a track that is not a track. Passing the two loops as
    // ROLES lets the tiler set the winding itself after projecting, which is
    // the one place in the pipeline that is allowed to decide it.
    //
    // The FIRST outer ring goes in `geometry` and only additional ones go in
    // `outerRings` -- the convention extract.cpp establishes for relations.
    // Putting the only ring in `outerRings` and leaving `geometry` empty
    // compiles, runs, reports success, and is silently dropped by add()'s
    // "a way whose vertices did not resolve" guard.
    if (!t.derived.outer.empty() && !t.derived.inner.empty())
    {
        input.geometry = t.derived.outer;
        input.innerRings.push_back(t.derived.inner);
    }
    else if (t.derived.loops.size() >= 2)
    {
        // A COMPOSITE: several layouts accumulated into one outline, as
        // consecutive (outer, inner) pairs. Drawn as the multipolygon it is,
        // so every layout keeps its own infield.
        //
        // An odd trailing curve is a shape that does not pair -- Charlotte's
        // roval has an oval loop, an infield loop and a connector -- and is
        // taken as another outer rather than dropped, since it is real tarmac.
        for (std::size_t i = 0; i < t.derived.loops.size(); ++i)
        {
            const bool isInner = (i % 2) == 1 && (i + 1) <= t.derived.loops.size();
            if (i == 0)
            {
                input.geometry = t.derived.loops[i];
            }
            else if (isInner)
            {
                input.innerRings.push_back(t.derived.loops[i]);
            }
            else
            {
                input.outerRings.push_back(t.derived.loops[i]);
            }
        }
    }
    else
    {
        // No seam, so no hole to cut. A point-to-point course is a solid
        // ribbon and drawing it filled is right.
        input.geometry = t.outline;
    }

    input.attributes.emplace_back("track_id", t.id);
    input.attributes.emplace_back("venue_id", t.venueId);
    input.attributes.emplace_back("quality", track::to_string(t.derived.quality));
    return input;
}

DrawInput centerlineFeature(const IngestedTrack& t, std::uint8_t minZoom)
{
    DrawInput input;
    input.name = t.name;
    input.layer = kCenterlineLayer;
    input.classification.minZoom = minZoom;
    input.classification.className = "centerline";
    input.closed = t.closed;
    input.geometry = t.derived.centerline->points;
    input.attributes.emplace_back("track_id", t.id);
    input.attributes.emplace_back("venue_id", t.venueId);
    return input;
}

DrawInput labelFeature(const IngestedTrack& t, std::uint8_t minZoom)
{
    DrawInput input;
    input.name = t.name;
    input.layer = kLabelLayer;
    input.isPoint = true;
    input.classification.minZoom = minZoom;
    input.classification.className = "track";
    // Bigger circuits win a label collision. Rank is "lower sorts first", so a
    // long track has to get a SMALL number -- the sense is easy to invert and
    // the result is a map that labels the kart circuit and hides Spa.
    //
    // On `place`, not on `classification`: the tiler reads a POINT's rank from
    // there, and a rank set on the classification is carried all the way
    // through and then not written.
    const double lengthM = t.derived.centerline ? t.derived.centerline->lengthM
                                                : t.derived.outlineLengthM * 0.5;
    input.place.labelRank =
        static_cast<std::uint8_t>(std::clamp(20.0 - lengthM / 1000.0, 0.0, 20.0));

    input.geometry = { road_graph::fromDegrees((t.bounds.south + t.bounds.north) * 0.5),
                       road_graph::fromDegrees((t.bounds.west + t.bounds.east) * 0.5) };
    input.attributes.emplace_back("track_id", t.id);
    input.attributes.emplace_back("venue_id", t.venueId);
    return input;
}

// Identifies THIS build of THIS input, and is written into both the mbtiles
// `metadata` table and the catalogue's own `track_meta`. track_store::Store
// refuses a file where the two disagree, which is the state a half-finished or
// half-copied build leaves behind.
//
// A timestamp alone would call two builds of identical data different, and a
// content hash alone would call a rebuild after a source edit the same when the
// edit did not change the file list. Both together say what a reader wants:
// which input, and which run over it.
std::string makeBuildId(const std::filesystem::path& input, std::size_t trackCount)
{
    const auto now = std::chrono::system_clock::now();
    std::string stamp = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                    std::chrono::floor<std::chrono::seconds>(now));

    // FNV-1a over the sorted file names and sizes. Not cryptographic and does
    // not need to be -- it exists to notice that two halves of one file came
    // from different runs, not to resist anybody.
    std::uint64_t hash = 1469598103934665603ULL;
    const auto mix = [&hash](std::string_view text) {
        for (const char c : text)
        {
            hash ^= static_cast<std::uint8_t>(c);
            hash *= 1099511628211ULL;
        }
    };
    for (const auto& path : track::listSourceFiles(input))
    {
        mix(path.filename().string());
        std::error_code ec;
        mix(std::to_string(std::filesystem::file_size(path, ec)));
    }
    mix(std::to_string(trackCount));

    return stamp + "-" + std::format("{:016x}", hash);
}

track_store::Quality toStoreQuality(track::Quality quality)
{
    // Written out rather than static_cast, so -Wswitch-enum catches an
    // enumerant added on one side and not the other. The two enums are
    // deliberately separate: one is the ingest's vocabulary, the other is a
    // PERSISTED integer, and letting them be the same type would make
    // reordering the first silently rewrite every catalogue ever built.
    switch (quality)
    {
        case track::Quality::Unknown:
            return track_store::Quality::Unknown;
        case track::Quality::Ok:
            return track_store::Quality::Ok;
        case track::Quality::SeamNotFound:
            return track_store::Quality::SeamNotFound;
        case track::Quality::MultipleLoops:
            return track_store::Quality::MultipleLoops;
        case track::Quality::WidthOutOfRange:
            return track_store::Quality::WidthOutOfRange;
        case track::Quality::LengthMismatch:
            return track_store::Quality::LengthMismatch;
        case track::Quality::SourceLengthImplausible:
            return track_store::Quality::SourceLengthImplausible;
        case track::Quality::Degenerate:
            return track_store::Quality::Degenerate;
    }
    return track_store::Quality::Unknown;
}

track_store::Result<void> writeCatalogue(const std::filesystem::path& path,
                                         const std::string& buildId,
                                         const std::vector<IngestedTrack>& tracks)
{
    auto writer = track_store::Writer::append(path, buildId);
    if (!writer)
    {
        return std::unexpected(writer.error());
    }

    for (const auto& t : tracks)
    {
        track_store::TrackRecord record;
        record.id = t.id;
        record.name = t.name;
        record.circuit = t.circuit;
        record.venueId = t.venueId;
        record.west = t.bounds.west;
        record.south = t.bounds.south;
        record.east = t.bounds.east;
        record.north = t.bounds.north;
        record.publishedLengthM = t.derived.publishedLengthM;
        record.principalAxisDeg = t.principalAxisDeg;
        record.closed = t.closed;
        record.combo = t.combo;
        record.quality = toStoreQuality(t.derived.quality);
        record.outlinePoints = static_cast<std::uint32_t>(track::pointCount(t.outline));

        // hasCenterline means USABLE, not merely present. A rejected track
        // keeps its centreline in memory so the report can say how wrong it
        // was, but nothing downstream may measure a lap against it.
        const bool usable =
            t.derived.quality == track::Quality::Ok && t.derived.centerline.has_value();
        record.hasCenterline = usable;
        if (t.derived.centerline.has_value())
        {
            record.centerlineLengthM = t.derived.centerline->lengthM;
            record.medianWidthM = t.derived.centerline->medianWidthM;
        }

        if (t.gate.present && usable)
        {
            record.gate.source = track_store::GateSource::DataDrop;
            record.gate.centreLatE7 = t.gate.centreLat;
            record.gate.centreLonE7 = t.gate.centreLon;
            record.gate.leftLatE7 = t.gate.leftLat;
            record.gate.leftLonE7 = t.gate.leftLon;
            record.gate.rightLatE7 = t.gate.rightLat;
            record.gate.rightLonE7 = t.gate.rightLon;
            record.gate.centerlineOffsetCm = t.gate.centerlineOffsetCm;
            record.gate.widthM = t.gate.widthM;
        }

        if (auto ok = writer->put(record); !ok)
        {
            return ok;
        }

        // The outline goes in whatever the verdict was: it is what draws, and a
        // rejected track is still a track on the map.
        if (!t.derived.outer.empty())
        {
            if (auto ok = writer->putGeometry(t.id, track_store::GeometryKind::OuterRing,
                                              t.derived.outer);
                !ok)
            {
                return ok;
            }
        }
        else if (auto ok =
                     writer->putGeometry(t.id, track_store::GeometryKind::OuterRing, t.outline);
                 !ok)
        {
            return ok;
        }

        if (!t.derived.inner.empty())
        {
            if (auto ok = writer->putGeometry(t.id, track_store::GeometryKind::InnerRing,
                                              t.derived.inner);
                !ok)
            {
                return ok;
            }
        }

        if (usable)
        {
            const auto& line = *t.derived.centerline;
            if (auto ok = writer->putGeometry(t.id, track_store::GeometryKind::Centerline,
                                              line.points);
                !ok)
            {
                return ok;
            }
            if (auto ok = writer->putGeometry(
                    t.id, track_store::GeometryKind::CenterlineDistanceCm, line.distanceCm);
                !ok)
            {
                return ok;
            }
            if (auto ok = writer->putGeometry(t.id, track_store::GeometryKind::HalfWidthCm,
                                              line.halfWidthCm);
                !ok)
            {
                return ok;
            }
        }
    }

    return writer->finish();
}

} // namespace

std::vector<IngestedTrack> ingestTracks(const std::filesystem::path& directory,
                                        const IngestOptions& options,
                                        std::vector<SkippedFile>& skipped)
{
    std::vector<IngestedTrack> tracks;

    const auto files = track::listSourceFiles(directory);
    tracks.reserve(files.size());

    for (const auto& path : files)
    {
        auto loaded = track::loadSourceFile(path);
        if (loaded.status != track::LoadStatus::Ok)
        {
            SPDLOG_WARN("[tracks] {}: {} ({})", path.filename().string(),
                        track::to_string(loaded.status), loaded.error);
            skipped.push_back({ path.stem().string(), loaded.status, loaded.error });
            continue;
        }

        IngestedTrack ingested;
        ingested.id = loaded.file.id;
        ingested.name = loaded.file.name;
        ingested.circuit = loaded.file.circuit;
        ingested.combo = loaded.file.combo;
        ingested.outline = std::move(loaded.file.outline);
        ingested.bounds = boundsOf(ingested.outline);
        ingested.gatePoint = loaded.file.gatePoint;

        ingested.derived =
            track::derive(ingested.outline, loaded.file.publishedLengthM,
                          loaded.file.degenerate, options.derive);

        // FROM THE DERIVATION, not from the source. Every outline in this
        // database is a closed polygon -- the `closed` property is true on all
        // 994 and the feature type is Polygon on all 994 -- so neither can say
        // whether a track is a circuit or a point-to-point course. Only the
        // centreline that came out can.
        ingested.closed = ingested.derived.centerline.has_value()
                              ? ingested.derived.centerline->closed
                              : loaded.file.closed;
        ingested.principalAxisDeg = track::principalAxisDeg(ingested.outline);

        if (loaded.file.gatePoint != track::GatePointStatus::Present)
        {
            ingested.gateResult = loaded.file.gatePoint == track::GatePointStatus::Several
                                      ? track::GateResult::Ambiguous
                                      : track::GateResult::NoPoint;
        }
        else if (!ingested.derived.centerline.has_value())
        {
            ingested.gateResult = track::GateResult::NoCenterline;
        }
        else
        {
            ingested.gateResult = track::placeGate(
                *ingested.derived.centerline, ingested.derived.outer, ingested.derived.inner,
                loaded.file.gateLat, loaded.file.gateLon, ingested.gate);
        }

        tracks.push_back(std::move(ingested));
    }

    assignVenues(tracks, options.venueExpandM);
    return tracks;
}

void addTracksOptions(cxxopts::Options& options)
{
    options.add_options()("i,input", "Directory of track GeoJSON files.",
                          cxxopts::value<std::string>())(
        "o,output", "mbtiles archive to write.", cxxopts::value<std::string>())(
        "name", "Tileset name, written into the metadata.",
        cxxopts::value<std::string>()->default_value("tracks"))(
        "min-zoom", "Lowest zoom to build.", cxxopts::value<std::uint64_t>()->default_value("8"))(
        "max-zoom", "Highest zoom to build.", cxxopts::value<std::uint64_t>()->default_value("14"))(
        "samples", "Centreline samples per track.",
        cxxopts::value<std::uint64_t>()->default_value("2000"))(
        "venue-expand-m", "How far apart two layouts may be and still share a venue.",
        cxxopts::value<double>()->default_value("300"))(
        "length-tolerance", "Fractional agreement required against the published lap length.",
        cxxopts::value<double>()->default_value("0.05"))(
        "width-min", "Narrowest median track width to accept, metres. Point-to-point "
                     "courses are public roads and genuinely narrow -- Gurston Down is 4.2 m.",
        cxxopts::value<double>()->default_value("3"))(
        "width-max", "Widest median track width to accept, metres.",
        cxxopts::value<double>()->default_value("30"))(
        "report", "Write a per-track CSV of what happened.", cxxopts::value<std::string>())(
        "quiet", "No progress lines.", cxxopts::value<bool>()->default_value("false"));
}

int runTracks(cli::Context& context)
{
    auto input = context.requireString("input");
    if (!input)
    {
        return cli::kUsage;
    }

    IngestOptions options;
    options.derive.samples = static_cast<std::size_t>(context.uintOr("samples", 2000));
    options.derive.minWidthM = context.doubleOr("width-min", 3.0);
    options.derive.maxWidthM = context.doubleOr("width-max", 30.0);
    options.derive.lengthTolerance = context.doubleOr("length-tolerance", 0.05);
    options.venueExpandM = context.doubleOr("venue-expand-m", 300.0);

    const auto started = std::chrono::steady_clock::now();

    std::vector<SkippedFile> skipped;
    auto tracks = ingestTracks(*input, options, skipped);
    if (tracks.empty())
    {
        SPDLOG_ERROR("[tracks] no usable track files in {}", *input);
        return cli::kFailure;
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    std::map<std::string, std::size_t> byQuality;
    std::map<std::string, std::size_t> byGate;
    std::map<std::string, std::size_t> venues;
    for (const auto& t : tracks)
    {
        ++byQuality[track::to_string(t.derived.quality)];
        ++byGate[track::to_string(t.gateResult)];
        ++venues[t.venueId];
    }

    cli::out("read in {:.1f} s\n", elapsed.count() / 1000.0);
    cli::out("tracks     {}\n", tracks.size());
    cli::out("skipped    {}\n", skipped.size());
    cli::out("venues     {}\n", venues.size());
    cli::out("\ncentreline quality:\n");
    for (const auto& [name, count] : byQuality)
    {
        cli::out("  {:<26} {}\n", name, count);
    }
    cli::out("\nstart/finish gate:\n");
    for (const auto& [name, count] : byGate)
    {
        cli::out("  {:<26} {}\n", name, count);
    }

    if (context.has("report"))
    {
        const std::string path = context.stringOr("report", "");
        writeReport(path, tracks, skipped);
        cli::out("\nreport     {}\n", path);
    }

    auto output = context.requireString("output");
    if (!output)
    {
        return cli::kUsage;
    }

    TileOptions tileOptions;
    tileOptions.minZoom = static_cast<std::uint8_t>(context.uintOr("min-zoom", 8));
    tileOptions.maxZoom = static_cast<std::uint8_t>(context.uintOr("max-zoom", 14));
    tileOptions.progressEvery = context.flag("quiet") ? 0 : 5000;
    // NEVER merge. Merging folds features that share every attribute into one
    // line below a zoom, which is right for ten million road segments and wrong
    // here: `track_id` is the identity a catalogue entry is joined on, and a
    // merged feature has lost it.
    tileOptions.mergeBelowZoom = 0;

    Tiler tiler;
    const std::uint8_t surfaceMinZoom = 11;
    const std::uint8_t centerlineMinZoom = 12;
    const std::uint8_t labelMinZoom = 8;

    TrackBounds all { 180.0, 90.0, -180.0, -90.0 };
    std::size_t suppressed = 0;
    for (const auto& t : tracks)
    {
        // A COMPOSITE IS NOT DRAWN AS THE TRACK IT NAMES.
        //
        // Everywhere else in this verb a rejected track still renders, because
        // the outline is sound and only the centreline derived from it failed.
        // That premise does not hold here: these outlines are OTHER layouts
        // concatenated, and the named layout is absent from its own file --
        // Road Atlanta's outline is the Short and Combo layouts, not Road
        // Atlanta. Drawing it would overdraw the two real siblings and hang the
        // wrong name on them.
        //
        // The catalogue row is kept, with its quality verdict, so the track is
        // diagnosable rather than silently missing. Corruption confirmed
        // upstream in the source vectors; see docs/tracks.md.
        if (t.derived.quality == track::Quality::MultipleLoops)
        {
            ++suppressed;
            continue;
        }

        tiler.add(surfaceFeature(t, surfaceMinZoom));
        if (t.derived.centerline.has_value())
        {
            tiler.add(centerlineFeature(t, centerlineMinZoom));
        }
        tiler.add(labelFeature(t, labelMinZoom));

        all.west = std::min(all.west, t.bounds.west);
        all.south = std::min(all.south, t.bounds.south);
        all.east = std::max(all.east, t.bounds.east);
        all.north = std::max(all.north, t.bounds.north);
    }

    // Scoped, and that is load-bearing. mbtiles::Writer::finish() commits but
    // does NOT close -- only the destructor does -- and track_store::Writer
    // opens the same file for writing straight afterwards. Two write handles on
    // one SQLite file is how a half-written archive happens, and the symptom is
    // an archive that opens and is missing rows.
    TileStats tiled;
    {
        auto writer = mbtiles::Writer::create(*output);
        if (!writer)
        {
            SPDLOG_ERROR("{}", mbtiles::to_string(writer.error()));
            return cli::kFailure;
        }

    // The bounds are the union of the TRACKS, and nothing else. There is no
    // extract here whose bbox could be borrowed -- that is the point of the
    // verb -- and an archive claiming a basemap's coverage would tell a client
    // it has tiles across a continent when it has a few hundred worldwide.
    //
    // In 1e-7 DEGREES, not degrees. Tiler::write takes osm::Coord like
    // everything else in this tree; handing it a double in degrees truncates
    // to zero and the archive then advertises a bounding box a few
    // micro-degrees across, centred on Null Island. Nothing fails -- the tiles
    // are all correct -- and a client that trusts `bounds` simply believes the
    // archive covers nothing.
        auto written =
            tiler.write(*writer, tileOptions, context.stringOr("name", "tracks"),
                        road_graph::fromDegrees(all.west), road_graph::fromDegrees(all.south),
                        road_graph::fromDegrees(all.east), road_graph::fromDegrees(all.north));
        if (!written)
        {
            SPDLOG_ERROR("{}", mbtiles::to_string(written.error()));
            return cli::kFailure;
        }
        tiled = *written;

        if (auto ok = writer->finish(); !ok)
        {
            SPDLOG_ERROR("{}", mbtiles::to_string(ok.error()));
            return cli::kFailure;
        }
    }

    // The catalogue, into the SAME file, beside the tiles. See
    // libs/track_store/include/track_store/store.h for why it is not a sidecar.
    const std::string buildId = makeBuildId(*input, tracks.size());
    if (auto ok = writeCatalogue(*output, buildId, tracks); !ok)
    {
        SPDLOG_ERROR("{}", track_store::to_string(ok.error()));
        return cli::kFailure;
    }

    // Reopened through BOTH readers rather than trusted. An archive this tool
    // can write and Archive cannot read is the one failure that would reach the
    // dashboard as a blank map; a catalogue Store cannot read is the one that
    // would reach it as a track with no lap. Proving otherwise costs a second.
    auto catalogue = track_store::Store::open(*output);
    if (!catalogue)
    {
        SPDLOG_ERROR("wrote a catalogue that will not open: {}",
                     track_store::to_string(catalogue.error()));
        return cli::kFailure;
    }
    auto archive = mbtiles::Archive::open(*output);
    if (!archive)
    {
        SPDLOG_ERROR("wrote an archive that will not open: {}",
                     mbtiles::to_string(archive.error()));
        return cli::kFailure;
    }

    cli::out("\nsuppressed {} composite outline(s), not drawn as the track they name\n",
             suppressed);
    cli::out("features   {}\n", tiled.features);
    cli::out("tiles      {}\n", tiled.tiles);
    cli::out("dropped    {} features too small or too spread out for their zoom\n",
             tiled.droppedTooSmall);
    cli::out("bounds     {:.4f},{:.4f},{:.4f},{:.4f}\n", all.west, all.south, all.east, all.north);
    cli::out("build id   {}\n", catalogue->buildId());
    cli::out("catalogue  {} tracks, {} with a centreline\n", catalogue->tracks().size(),
             std::count_if(catalogue->tracks().begin(), catalogue->tracks().end(),
                           [](const auto& t) { return t.hasCenterline; }));
    cli::out("\ntiles per zoom:\n");
    for (const auto& [zoom, count] : tiled.tilesPerZoom)
    {
        cli::out("  z{:<3} {}\n", zoom, count);
    }

    return cli::kOk;
}

} // namespace map_build
