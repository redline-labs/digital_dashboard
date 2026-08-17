// SPDX-License-Identifier: GPL-3.0-or-later
#include "sources.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

#include <spdlog/spdlog.h>

#include "map_common.capnp.h"
#include "map_graph.capnp.h"
#include "map_tiles.capnp.h"
#include "map_tracks.capnp.h"
#include "pub_sub/zenoh_client.h"

namespace bd992_mock
{
namespace
{

// The two status vocabularies, in their own words. -Wswitch-enum is on, so
// adding an enumerant upstream is a build error here rather than a silent
// "unknown" in a diagnostic somebody is relying on.
const char* toString(::MapQueryStatus status)
{
    switch (status)
    {
    case ::MapQueryStatus::UNKNOWN:
        return "unknown";
    case ::MapQueryStatus::OK:
        return "ok";
    case ::MapQueryStatus::NO_MATCH:
        return "noMatch (nothing within the search radius)";
    case ::MapQueryStatus::NO_SUCH_GRAPH:
        return "noSuchGraph (the server has no graph by that name)";
    case ::MapQueryStatus::OUT_OF_COVERAGE:
        return "outOfCoverage (the point is outside the graph)";
    case ::MapQueryStatus::NO_ROUTE:
        return "noRoute (both ends matched, nothing connects them)";
    case ::MapQueryStatus::BAD_REQUEST:
        return "badRequest";
    case ::MapQueryStatus::FAILED:
        return "failed";
    }
    return "unrecognised";
}

const char* toString(::MapStatus status)
{
    switch (status)
    {
    case ::MapStatus::UNKNOWN:
        return "unknown";
    case ::MapStatus::OK:
        return "ok";
    case ::MapStatus::NOT_FOUND:
        return "notFound";
    case ::MapStatus::NO_SUCH_TILESET:
        return "noSuchTileset (no trackset by that name)";
    case ::MapStatus::OUT_OF_RANGE:
        return "outOfRange";
    case ::MapStatus::BAD_REQUEST:
        return "badRequest";
    case ::MapStatus::FAILED:
        return "failed";
    }
    return "unrecognised";
}

const char* toString(::MapTrackQuality quality)
{
    switch (quality)
    {
    case ::MapTrackQuality::UNKNOWN:
        return "unknown";
    case ::MapTrackQuality::OK:
        return "ok";
    case ::MapTrackQuality::SEAM_NOT_FOUND:
        return "seamNotFound";
    case ::MapTrackQuality::MULTIPLE_LOOPS:
        return "multipleLoops";
    case ::MapTrackQuality::WIDTH_OUT_OF_RANGE:
        return "widthOutOfRange";
    case ::MapTrackQuality::LENGTH_MISMATCH:
        return "lengthMismatch";
    case ::MapTrackQuality::SOURCE_LENGTH_IMPLAUSIBLE:
        return "sourceLengthImplausible";
    case ::MapTrackQuality::DEGENERATE:
        return "degenerate";
    }
    return "unrecognised";
}

// Free-flow speed by road class, in m/s, for a segment map/nearest would not
// name.
//
// A FALLBACK, not a model. It exists so that one unmatched segment in a
// thousand does not become an unbounded straight through a residential street;
// how often it fires is counted and reported, because a fallback that is
// carrying the whole route is a bug rather than a default.
double fallbackSpeedMps(::MapRoadClass roadClass)
{
    switch (roadClass)
    {
    case ::MapRoadClass::MOTORWAY:
        return 29.0;
    case ::MapRoadClass::TRUNK:
        return 25.0;
    case ::MapRoadClass::PRIMARY:
        return 20.0;
    case ::MapRoadClass::SECONDARY:
        return 17.0;
    case ::MapRoadClass::TERTIARY:
        return 15.0;
    case ::MapRoadClass::MINOR:
        return 11.0;
    case ::MapRoadClass::SERVICE:
        return 7.0;
    case ::MapRoadClass::TRACK:
        return 7.0;
    case ::MapRoadClass::PATH:
        return 4.0;
    case ::MapRoadClass::PEDESTRIAN:
        return 4.0;
    case ::MapRoadClass::FERRY:
        return 8.0;
    case ::MapRoadClass::UNKNOWN:
        return 13.0;
    }
    return 13.0;
}

std::string lowered(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

constexpr double kKphToMps = 1000.0 / 3600.0;

} // namespace

std::string resolveGraph(const ServicesConfig& services, std::string& error)
{
    if (!services.graph.empty())
    {
        return services.graph;
    }

    pub_sub::ZenohClient<::MapGraphInfoRequest, ::MapGraphInfoResponse> client(
        services.graphInfoKey, services.requestTimeoutMs);

    // Empty asks for every graph the server has.
    client.fields().setGraph("");

    std::string chosen;
    std::string detail;

    const bool answered = client.request([&](const ::MapGraphInfoResponse::Reader& reply) {
        if (reply.getStatus() != ::MapQueryStatus::OK)
        {
            detail = std::string("map/graph answered ") + toString(reply.getStatus());
            if (reply.hasError() && reply.getError().size() > 0)
            {
                detail += ": " + std::string(reply.getError().cStr());
            }
            return;
        }

        for (const ::MapGraphInfo::Reader graph : reply.getGraphs())
        {
            if (!graph.getOpen())
            {
                SPDLOG_WARN("[mock] graph '{}' is present but did not open: {}",
                            graph.getName().cStr(),
                            graph.hasError() ? graph.getError().cStr() : "no reason given");
                continue;
            }
            if (chosen.empty())
            {
                chosen = graph.getName().cStr();
                SPDLOG_INFO("[mock] routing on graph '{}' ({} segments, {} edges)", chosen,
                            graph.getSegmentCount(), graph.getEdgeCount());
            }
            else
            {
                // Which one is wanted is then a real question, and guessing it
                // would be picking a map of a different part of the world.
                SPDLOG_WARN("[mock] the server also has graph '{}'; set services.graph to use it",
                            graph.getName().cStr());
            }
        }

        if (chosen.empty())
        {
            detail = "the server has no graph that opened; check `graphs:` in "
                     "configs/map_server.yaml";
        }
    });

    if (!answered)
    {
        error = "no reply on '" + services.graphInfoKey + "' -- is map_server running?";
        return {};
    }
    if (!detail.empty())
    {
        error = detail;
        return {};
    }
    return chosen;
}

bool buildRoutePath(const ServicesConfig& services, const std::string& graph,
                    const RouteOptions& options, Path& path, SourceReport& report)
{
    pub_sub::ZenohClient<::MapRouteRequest, ::MapRouteResponse> client(services.routeKey,
                                                                      services.requestTimeoutMs);

    ::MapRouteRequest::Builder request = client.fields();
    request.setGraph(graph);
    request.setFromLatitudeDeg(options.fromLatitudeDeg);
    request.setFromLongitudeDeg(options.fromLongitudeDeg);
    request.setToLatitudeDeg(options.toLatitudeDeg);
    request.setToLongitudeDeg(options.toLongitudeDeg);
    request.setHasFromHeading(false);
    request.setProfile(options.profile);

    // KEEP EVERY POINT. Simplification is for drawing: it drops vertices that
    // sit within a tolerance of the line through their neighbours, which is
    // exactly the geometry a corner is made of. Rounding those off would let
    // the vehicle carry motorway speed through a junction.
    request.setSimplifyToleranceM(0.0);

    std::vector<std::int32_t> geometry;
    std::vector<std::uint32_t> segmentStarts;
    std::vector<std::uint64_t> segmentIds;
    std::string detail;

    const bool answered = client.request([&](const ::MapRouteResponse::Reader& reply) {
        if (reply.getStatus() != ::MapQueryStatus::OK)
        {
            detail = std::string("map/route answered ") + toString(reply.getStatus());
            if (reply.hasError() && reply.getError().size() > 0)
            {
                detail += ": " + std::string(reply.getError().cStr());
            }
            return;
        }

        const auto points = reply.getGeometry();
        geometry.reserve(points.size());
        for (const std::int32_t value : points)
        {
            geometry.push_back(value);
        }

        const auto starts = reply.getSegmentStarts();
        segmentStarts.reserve(starts.size());
        for (const std::uint32_t value : starts)
        {
            segmentStarts.push_back(value);
        }

        const auto ids = reply.getSegmentIds();
        segmentIds.reserve(ids.size());
        for (const std::uint64_t value : ids)
        {
            segmentIds.push_back(value);
        }

        report.serverDistanceM = reply.getDistanceM();
        report.serverDurationS = reply.getDurationS();
    });

    if (!answered)
    {
        report.error = "no reply on '" + services.routeKey + "' -- is map_server running?";
        return false;
    }
    if (!detail.empty())
    {
        report.error = detail;
        return false;
    }

    std::string problem;
    if (!setGeometryFromLonLat(geometry, path, problem))
    {
        report.error = "map/route: " + problem;
        return false;
    }

    report.segmentCount = segmentIds.size();

    // segmentStarts indexes `geometry` in COORDINATE PAIRS and carries one more
    // entry than there are segments, so segment i owns points
    // [segmentStarts[i], segmentStarts[i+1]).
    const bool startsUsable = segmentStarts.size() == segmentIds.size() + 1;
    if (!startsUsable && !segmentIds.empty())
    {
        SPDLOG_WARN("[mock] segmentStarts has {} entries for {} segments; expected {}. "
                    "Driving the geometry without per-road speed limits.",
                    segmentStarts.size(), segmentIds.size(), segmentIds.size() + 1);
    }

    path.closed = false;
    path.speedCapMps.assign(path.size(), std::numeric_limits<double>::infinity());

    if (options.speedLimits && startsUsable && !segmentIds.empty())
    {
        const auto began = std::chrono::steady_clock::now();

        pub_sub::ZenohClient<::MapNearestRequest, ::MapNearestResponse> nearest(
            services.nearestKey, services.requestTimeoutMs);

        // One query per DISTINCT segment: a route doubles back through the same
        // road often enough that asking per occurrence is wasted round trips.
        std::unordered_map<std::uint64_t, double> speedById;

        for (std::size_t segment = 0; segment < segmentIds.size(); ++segment)
        {
            const std::uint64_t id = segmentIds[segment];
            if (speedById.find(id) != speedById.end())
            {
                continue;
            }

            const std::size_t first = segmentStarts[segment];
            const std::size_t last = segmentStarts[segment + 1];
            if (last <= first || last > path.size())
            {
                continue;
            }

            // HALFWAY ALONG THE SEGMENT BY LENGTH, not by vertex index, and
            // with the bearing of the leg it lands on.
            //
            // Both parts matter. Most segments here have exactly two points --
            // a junction at each end -- so an index midpoint IS a junction,
            // where four roads are equally near and the answer is a coin flip.
            // And the route's geometry REPEATS the shared vertex between
            // consecutive segments, so a bearing taken between "the midpoint"
            // and "the next index" is a bearing between two identical
            // coordinates: atan2(0, 0), which is due north. Sending that as the
            // course over ground makes the server rank every east-west road
            // last, and it was silently costing this pass half its matches.
            double segmentLengthM = 0.0;
            for (std::size_t i = first; i + 1 < last; ++i)
            {
                segmentLengthM +=
                    road_graph::distanceM(path.lat[i], path.lon[i], path.lat[i + 1], path.lon[i + 1]);
            }

            double queryLatDeg = road_graph::toDegrees(path.lat[first]);
            double queryLonDeg = road_graph::toDegrees(path.lon[first]);
            double headingDeg = 0.0;
            bool haveHeading = false;

            if (segmentLengthM > 0.0)
            {
                const double half = segmentLengthM * 0.5;
                double walked = 0.0;
                for (std::size_t i = first; i + 1 < last; ++i)
                {
                    const double leg = road_graph::distanceM(path.lat[i], path.lon[i],
                                                             path.lat[i + 1], path.lon[i + 1]);
                    if (leg <= 0.0)
                    {
                        continue;
                    }
                    if (walked + leg >= half)
                    {
                        const double t = (half - walked) / leg;
                        queryLatDeg = road_graph::toDegrees(path.lat[i]) +
                                      (t * (road_graph::toDegrees(path.lat[i + 1]) -
                                            road_graph::toDegrees(path.lat[i])));
                        queryLonDeg = road_graph::toDegrees(path.lon[i]) +
                                      (t * (road_graph::toDegrees(path.lon[i + 1]) -
                                            road_graph::toDegrees(path.lon[i])));
                        headingDeg = road_graph::bearingDeg(path.lat[i], path.lon[i],
                                                            path.lat[i + 1], path.lon[i + 1]);
                        haveHeading = true;
                        break;
                    }
                    walked += leg;
                }
            }

            ::MapNearestRequest::Builder ask = nearest.fields();
            ask.setGraph(graph);
            ask.setLatitudeDeg(queryLatDeg);
            ask.setLongitudeDeg(queryLonDeg);
            ask.setRadiusM(25.0f);
            ask.setHasHeading(haveHeading);
            ask.setHeadingDeg(static_cast<float>(headingDeg));
            // Several, because "what road is at this point" can legitimately
            // answer with the frontage road running beside this one.
            ask.setMaxCandidates(8);

            ++report.speedQueries;

            double resolved = std::numeric_limits<double>::infinity();
            bool posted = false;

            const bool replied = nearest.request([&](const ::MapNearestResponse::Reader& reply) {
                if (reply.getStatus() != ::MapQueryStatus::OK)
                {
                    return;
                }
                for (const ::MapSegmentMatch::Reader candidate : reply.getCandidates())
                {
                    // MATCH ON THE ID, not on being nearest. Taking the first
                    // candidate would silently adopt the speed limit of
                    // whichever road happened to be closest to the midpoint,
                    // which on a freeway is routinely the frontage road.
                    if (candidate.getWhere().getSegmentId() != id)
                    {
                        continue;
                    }

                    const ::MapSpeed::Reader speed = candidate.getSpeed();
                    if (speed.getHasPosted())
                    {
                        resolved = static_cast<double>(speed.getPostedKph()) * kKphToMps;
                        posted = true;
                    }
                    else if (speed.getFreeFlowKph() > 0)
                    {
                        resolved = static_cast<double>(speed.getFreeFlowKph()) * kKphToMps;
                    }
                    else
                    {
                        resolved = fallbackSpeedMps(candidate.getRoadClass());
                    }
                    ++report.speedMatched;
                    return;
                }
            });

            if (!replied)
            {
                SPDLOG_WARN("[mock] no reply on '{}'; giving up on speed limits",
                            services.nearestKey);
                break;
            }

            if (posted)
            {
                ++report.speedPosted;
            }
            speedById.emplace(id, resolved);
        }

        // Paint the resolved speeds onto the vertices they belong to.
        for (std::size_t segment = 0; segment < segmentIds.size(); ++segment)
        {
            const auto found = speedById.find(segmentIds[segment]);
            if (found == speedById.end())
            {
                continue;
            }
            const std::size_t first = segmentStarts[segment];
            const std::size_t last = std::min<std::size_t>(segmentStarts[segment + 1], path.size());
            for (std::size_t i = first; i < last; ++i)
            {
                path.speedCapMps[i] = std::min(path.speedCapMps[i], found->second);
            }
        }

        const auto ended = std::chrono::steady_clock::now();
        report.speedElapsedS =
            std::chrono::duration_cast<std::chrono::duration<double>>(ended - began).count();
    }

    // After the speed caps are painted, so a dropped duplicate takes its cap
    // with it and the arrays stay parallel.
    dropRepeatedPoints(path);
    computeDistances(path);

    path.description = "route on '" + graph + "', " + std::to_string(path.size()) + " points, " +
                       std::to_string(report.segmentCount) + " segments";
    return true;
}

bool buildTrackPath(const ServicesConfig& services, const TrackOptions& options, Path& path,
                    SourceReport& report)
{
    // ---- Find the track ----------------------------------------------------

    struct Candidate
    {
        std::string id;
        std::string name;
        bool hasCenterline { false };
        ::MapTrackQuality quality { ::MapTrackQuality::UNKNOWN };
    };

    std::vector<Candidate> exact;
    std::vector<Candidate> partial;
    std::string detail;

    {
        pub_sub::ZenohClient<::MapTrackCatalogRequest, ::MapTrackCatalogResponse> client(
            services.trackCatalogKey, services.requestTimeoutMs);

        // Empty trackset already means "the first configured one".
        client.fields().setTrackset(services.trackset);
        client.fields().setVenueId("");
        client.fields().setRadiusM(0.0);

        const std::string wanted = lowered(options.query);

        const bool answered =
            client.request([&](const ::MapTrackCatalogResponse::Reader& reply) {
                if (reply.getStatus() != ::MapStatus::OK)
                {
                    detail = std::string("map/track_catalog answered ") + toString(reply.getStatus());
                    if (reply.hasError() && reply.getError().size() > 0)
                    {
                        detail += ": " + std::string(reply.getError().cStr());
                    }
                    return;
                }

                report.buildId = reply.getBuildId().cStr();

                for (const ::MapTrackSummary::Reader track : reply.getTracks())
                {
                    Candidate candidate;
                    candidate.id = track.getId().cStr();
                    candidate.name = track.getName().cStr();
                    candidate.hasCenterline = track.getHasCenterline();
                    candidate.quality = track.getQuality();

                    if (lowered(candidate.id) == wanted || lowered(candidate.name) == wanted)
                    {
                        exact.push_back(std::move(candidate));
                    }
                    else if (lowered(candidate.name).find(wanted) != std::string::npos ||
                             lowered(candidate.id).find(wanted) != std::string::npos)
                    {
                        partial.push_back(std::move(candidate));
                    }
                }
            });

        if (!answered)
        {
            report.error = "no reply on '" + services.trackCatalogKey + "' -- is map_server "
                           "running, and does its config have a `tracksets:` entry?";
            return false;
        }
        if (!detail.empty())
        {
            report.error = detail;
            return false;
        }
    }

    const std::vector<Candidate>& matches = !exact.empty() ? exact : partial;

    if (matches.empty())
    {
        report.error = "no track matches '" + options.query + "'";
        return false;
    }
    if (matches.size() > 1)
    {
        std::string list;
        // Enough to choose from without printing all 994 when somebody types a
        // single letter.
        constexpr std::size_t kMaxListed = 20;
        for (std::size_t i = 0; i < matches.size() && i < kMaxListed; ++i)
        {
            list += "\n    " + matches[i].id + "  (" + matches[i].name + ")";
            if (!matches[i].hasCenterline)
            {
                list += "  [no centreline: ";
                list += toString(matches[i].quality);
                list += "]";
            }
        }
        if (matches.size() > kMaxListed)
        {
            list += "\n    ... and " + std::to_string(matches.size() - kMaxListed) + " more";
        }
        report.error = "'" + options.query + "' matches " + std::to_string(matches.size()) +
                       " tracks:" + list;
        return false;
    }

    const Candidate& chosen = matches.front();
    report.trackId = chosen.id;

    if (!chosen.hasCenterline)
    {
        // A real, diagnosable state rather than a fault: about a fifth of the
        // corpus has no centreline, and the catalogue says why.
        report.error = "track '" + chosen.id + "' (" + chosen.name +
                       ") has no centreline to drive: " + toString(chosen.quality) +
                       ". See docs/tracks.md.";
        return false;
    }

    // ---- Fetch its centreline ---------------------------------------------

    std::vector<std::int32_t> centerline;
    std::vector<std::uint32_t> distanceCm;
    std::vector<std::uint16_t> halfWidthCm;
    bool closed = false;
    detail.clear();

    {
        pub_sub::ZenohClient<::MapTrackDetailRequest, ::MapTrackDetailResponse> client(
            services.trackDetailKey, services.requestTimeoutMs);

        ::MapTrackDetailRequest::Builder request = client.fields();
        request.setTrackset(services.trackset);
        request.setId(chosen.id);
        // The outline is what draws the circuit; we are driving the middle of
        // it and have no use for the edges.
        request.setWantOutline(false);
        request.setWantCenterline(true);
        request.setSimplifyToleranceM(0.0f);

        const bool answered = client.request([&](const ::MapTrackDetailResponse::Reader& reply) {
            if (reply.getStatus() != ::MapStatus::OK)
            {
                detail = std::string("map/track_detail answered ") + toString(reply.getStatus());
                if (reply.hasError() && reply.getError().size() > 0)
                {
                    detail += ": " + std::string(reply.getError().cStr());
                }
                return;
            }

            const ::MapTrackSummary::Reader summary = reply.getSummary();
            closed = summary.getClosed();
            report.publishedLengthM = summary.getPublishedLengthM();
            report.medianWidthM = summary.getMedianWidthM();
            report.catalogLengthM = summary.getCenterlineLengthM();
            report.buildId = reply.getBuildId().cStr();

            const auto points = reply.getCenterline();
            centerline.reserve(points.size());
            for (const std::int32_t value : points)
            {
                centerline.push_back(value);
            }

            const auto distances = reply.getCenterlineDistanceCm();
            distanceCm.reserve(distances.size());
            for (const std::uint32_t value : distances)
            {
                distanceCm.push_back(value);
            }

            const auto widths = reply.getHalfWidthCm();
            halfWidthCm.reserve(widths.size());
            for (const std::uint16_t value : widths)
            {
                halfWidthCm.push_back(value);
            }
        });

        if (!answered)
        {
            report.error = "no reply on '" + services.trackDetailKey + "'";
            return false;
        }
        if (!detail.empty())
        {
            report.error = detail;
            return false;
        }
    }

    std::string problem;
    if (!setGeometryFromLonLat(centerline, path, problem))
    {
        report.error = "map/track_detail: " + problem;
        return false;
    }

    path.closed = closed;
    path.speedCapMps.assign(path.size(), std::numeric_limits<double>::infinity());

    dropRepeatedPoints(path);
    computeDistances(path);

    // The catalogue's own length is an INDEPENDENT measurement of the same
    // geometry, so a disagreement means one of the two is wrong -- most likely
    // that this decoded the wrong list, or decoded it lat-first. Cheap to check
    // and it fails loudly rather than putting a car in the sea.
    if (report.catalogLengthM > 0.0)
    {
        const double ours = path.lengthM();
        const double error = std::fabs(ours - report.catalogLengthM) / report.catalogLengthM;
        if (error > 0.05)
        {
            SPDLOG_WARN("[mock] centreline measures {:.0f} m here against the catalogue's "
                        "{:.0f} m ({:.1f}% apart) -- suspect the geometry decode",
                        ours, report.catalogLengthM, error * 100.0);
        }
    }

    if (!halfWidthCm.empty())
    {
        std::vector<std::uint16_t> sorted(halfWidthCm);
        std::sort(sorted.begin(), sorted.end());
        const double medianHalfWidthM = static_cast<double>(sorted[sorted.size() / 2]) / 100.0;
        SPDLOG_DEBUG("[mock] median track half width {:.1f} m (unused: driving the centreline)",
                     medianHalfWidthM);
    }

    if (distanceCm.size() != centerline.size() / 2)
    {
        SPDLOG_WARN("[mock] centerlineDistanceCm has {} entries for {} points",
                    distanceCm.size(), centerline.size() / 2);
    }

    path.description = "track '" + chosen.id + "' (" + chosen.name + "), " +
                       (path.closed ? "circuit" : "point-to-point") + ", " +
                       std::to_string(path.size()) + " points";

    if (!path.closed)
    {
        // docs/tracks.md: the two folds of an open centreline are
        // interchangeable, so index zero is not "the start of the course" --
        // it is just an end. Say which way we are going rather than implying
        // the data knows.
        SPDLOG_INFO("[mock] '{}' is a point-to-point course; its centreline has no inherent "
                    "direction, so this drives it from index zero",
                    chosen.id);
    }

    return true;
}

} // namespace bd992_mock
