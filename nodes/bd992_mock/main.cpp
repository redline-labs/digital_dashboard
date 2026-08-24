// SPDX-License-Identifier: GPL-3.0-or-later
//
// A BD992 with no receiver behind it, driving a road the map really has.
//
//   bd992_mock --route 33.6866,-117.8558 --to 33.7701,-118.1937
//   bd992_mock --track "Willow Springs"
//
// It asks nodes/map_server where to drive -- map/route for a road route,
// map/track_catalog and map/track_detail for a circuit centreline -- then runs a
// point-mass vehicle along the answer and publishes the GNSS topics the real
// bridge publishes. Position, speed and heading are all derived from one pair of
// numbers, so they cannot disagree with each other.
//
// IT PUBLISHES ON THE REAL PREFIX, `nodes/bd992`, which is what makes every
// existing consumer work unchanged and is also the one thing to be careful of:
// running this alongside a real bd992_bridge puts two publishers on one key, and
// a consumer takes whichever sample arrives first. See the startup check below.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include "pub_sub/node_identity.h"
#include "pub_sub/topic_discovery.h"

#include "node_config.h"
#include "path.h"
#include "publishers.h"
#include "sources.h"
#include "vehicle.h"

namespace
{

std::atomic<bool> gRunning { true };

void handleSignal(int)
{
    gRunning.store(false);
}

// "33.6866,-117.8558" -> two doubles. Returns false on anything else.
bool parseLatLon(const std::string& text, double& latitudeDeg, double& longitudeDeg)
{
    const std::size_t comma = text.find(',');
    if (comma == std::string::npos)
    {
        return false;
    }

    try
    {
        std::size_t consumedLat = 0;
        std::size_t consumedLon = 0;
        const std::string latPart = text.substr(0, comma);
        const std::string lonPart = text.substr(comma + 1);

        const double lat = std::stod(latPart, &consumedLat);
        const double lon = std::stod(lonPart, &consumedLon);

        // Trailing rubbish means the user meant something else.
        if (consumedLat != latPart.size() || consumedLon != lonPart.size())
        {
            return false;
        }
        if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0)
        {
            return false;
        }

        latitudeDeg = lat;
        longitudeDeg = lon;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// Warn if something is already publishing the epoch topic.
//
// Two publishers on one key is the trap documented for map_server in
// docs/map_build.md: nothing errors, the consumer takes whichever sample
// arrives, and a screenshot "proving" the mock works may be showing the real
// receiver. Best-effort -- observeTopics can only report what published during
// its window -- so this warns rather than refusing.
void warnIfAlreadyPublished(const std::string& key)
{
    constexpr int kWindowMs = 700;
    const std::vector<pub_sub::TopicObservation> seen = pub_sub::observeTopics(key, kWindowMs);
    for (const pub_sub::TopicObservation& topic : seen)
    {
        if (topic.key == key && topic.count > 0)
        {
            SPDLOG_WARN("[mock] SOMETHING IS ALREADY PUBLISHING {} at {:.1f} Hz -- probably a real "
                        "bd992_bridge. Two publishers on one key interleave, and consumers take "
                        "whichever sample arrives first. Stop one of them.",
                        key, topic.hz);
        }
    }
}

void reportPath(const bd992_mock::Path& path, const bd992_mock::SourceReport& report,
                const bd992_mock::Vehicle& vehicle)
{
    SPDLOG_INFO("[mock] {}", path.description);
    SPDLOG_INFO("[mock]   {:.0f} m of geometry, {}", path.lengthM(),
                path.closed ? "closed (laps)" : "open (drives once)");

    if (report.serverDistanceM > 0.0)
    {
        // The server's figure is door-to-door: it includes the lead-in from the
        // requested start to the first junction, which the geometry does not.
        SPDLOG_INFO("[mock]   the server called it {:.0f} m / {:.0f} s door to door",
                    report.serverDistanceM, report.serverDurationS);
    }
    if (report.segmentCount > 0)
    {
        SPDLOG_INFO("[mock]   {} segments", report.segmentCount);
    }
    if (report.speedQueries > 0)
    {
        SPDLOG_INFO("[mock]   speed limits: {} of {} segments matched, {} of those posted, "
                    "in {:.2f} s",
                    report.speedMatched, report.speedQueries, report.speedPosted,
                    report.speedElapsedS);
        if (report.speedMatched * 2 < report.speedQueries)
        {
            SPDLOG_WARN("[mock]   more than half the segments went to a road-class default; "
                        "the drive will be less faithful than it looks");
        }
    }
    if (!report.trackId.empty())
    {
        SPDLOG_INFO("[mock]   catalogue: {:.0f} m centreline, {:.0f} m published, {:.1f} m wide "
                    "(build {})",
                    report.catalogLengthM, report.publishedLengthM, report.medianWidthM,
                    report.buildId);
    }

    SPDLOG_INFO("[mock]   about {:.0f} s per traverse at the profiled speeds",
                vehicle.estimatedDurationS());
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v");

    std::string configPath;
    std::string routeFrom;
    std::string routeTo;
    std::string trackQuery;
    std::string profile;
    bool loop = false;
    bool noSpeedLimits = false;
    bool check = false;
    bool debug = false;

    try
    {
        cxxopts::Options options("bd992_mock",
                                 "Simulate a BD992 driving a route or a track from map_server");
        options.add_options()                                                              //
            ("c,config", "YAML configuration file. Optional: the defaults are this tree's.",
             cxxopts::value<std::string>(configPath))                                      //
            ("route", "Start of a road route, as 'lat,lon'.",
             cxxopts::value<std::string>(routeFrom))                                       //
            ("to", "Destination of a road route, as 'lat,lon'.",
             cxxopts::value<std::string>(routeTo))                                         //
            ("track", "Drive a race track's centreline, by id or name.",
             cxxopts::value<std::string>(trackQuery))                                      //
            ("profile", "Routing cost profile. Empty means the graph's default.",
             cxxopts::value<std::string>(profile))                                         //
            ("loop", "Start again on reaching the end. Circuits always lap.",
             cxxopts::value<bool>(loop))                                                   //
            ("no-speed-limits", "Skip the map/nearest pass; drive at the cruise speed.",
             cxxopts::value<bool>(noSpeedLimits))                                          //
            ("check", "Resolve the path, report it, and exit without publishing.",
             cxxopts::value<bool>(check))                                                  //
            ("d,debug", "Verbose logging.", cxxopts::value<bool>(debug))                   //
            ("h,help", "Print usage.");

        const auto args = options.parse(argc, argv);
        if (args.count("help") != 0)
        {
            SPDLOG_INFO("{}", options.help());
            return 0;
        }
    }
    catch (const std::exception& e)
    {
        SPDLOG_ERROR("{}", e.what());
        return 2;
    }

    if (debug)
    {
        spdlog::set_level(spdlog::level::debug);
    }

    const bool wantRoute = !routeFrom.empty() || !routeTo.empty();
    const bool wantTrack = !trackQuery.empty();

    if (wantRoute == wantTrack)
    {
        SPDLOG_ERROR("choose exactly one of --route/--to and --track");
        return 2;
    }

    bd992_mock::NodeConfig config;
    if (!configPath.empty() && !bd992_mock::load_node_config(configPath, config))
    {
        return 1;
    }

    bd992_mock::RouteOptions routeOptions;
    bd992_mock::TrackOptions trackOptions;

    if (wantRoute)
    {
        if (routeFrom.empty() || routeTo.empty())
        {
            SPDLOG_ERROR("--route and --to are both required for a road route");
            return 2;
        }
        if (!parseLatLon(routeFrom, routeOptions.fromLatitudeDeg, routeOptions.fromLongitudeDeg))
        {
            SPDLOG_ERROR("--route '{}' is not a 'lat,lon' pair", routeFrom);
            return 2;
        }
        if (!parseLatLon(routeTo, routeOptions.toLatitudeDeg, routeOptions.toLongitudeDeg))
        {
            SPDLOG_ERROR("--to '{}' is not a 'lat,lon' pair", routeTo);
            return 2;
        }
        routeOptions.profile = profile;
        routeOptions.speedLimits = !noSpeedLimits;
    }
    else
    {
        trackOptions.query = trackQuery;
    }

    // Before any publisher, so a tool watching the bus sees the node appear
    // before the topics it offers -- and sees that they belong to a mock.
    pub_sub::NodeIdentity identity("bd992_mock");

    bd992_mock::Path path;
    bd992_mock::SourceReport report;
    bool built = false;

    if (wantRoute)
    {
        std::string graphError;
        const std::string graph = bd992_mock::resolveGraph(config.services, graphError);
        if (graph.empty())
        {
            SPDLOG_ERROR("[mock] {}", graphError);
            return 1;
        }
        built = bd992_mock::buildRoutePath(config.services, graph, routeOptions, path, report);
    }
    else
    {
        built = bd992_mock::buildTrackPath(config.services, trackOptions, path, report);
    }

    if (!built)
    {
        SPDLOG_ERROR("[mock] {}", report.error);
        return 1;
    }

    if (path.size() < bd992_mock::kMinPathPoints || path.lengthM() <= 0.0)
    {
        SPDLOG_ERROR("[mock] the path has no length to drive");
        return 1;
    }

    // A circuit always laps; --loop is what makes an open path repeat. Built
    // before the --check return so that what --check reports is the profile the
    // run would actually drive, rather than a second one computed the same way.
    bd992_mock::Vehicle vehicle(path, config.vehicle, loop);
    reportPath(path, report, vehicle);

    if (check)
    {
        return 0;
    }

    warnIfAlreadyPublished(config.publish.topicPrefix + "/gsof/lat_long_height");

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    bd992_mock::Publishers publishers(config.publish);

    const double dt = 1.0 / static_cast<double>(config.publish.rateHz);
    const auto tick = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(dt));

    // Wall-clock time is read ONCE. Everything after it advances by exactly dt
    // per tick, so the GPS timestamps and the distance travelled describe the
    // same vehicle even when the loop is late -- which it will be, because this
    // sleeps rather than running on a timer.
    const double startUnixS =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    double simulatedS = 0.0;

    auto nextTick = std::chrono::steady_clock::now();
    auto nextLog = std::chrono::steady_clock::now();

    SPDLOG_INFO("[mock] driving at {} Hz on {}", config.publish.rateHz,
                config.publish.topicPrefix);

    std::uint32_t lastLap = 0;

    while (gRunning.load())
    {
        vehicle.step(dt);
        simulatedS += dt;

        const bd992_mock::VehicleState& state = vehicle.state();
        publishers.publish(state, bd992_mock::gpsTimeFromUnix(startUnixS + simulatedS));

        if (state.lap != lastLap)
        {
            SPDLOG_INFO("[mock] lap {} at {:.0f} s", state.lap, simulatedS);
            lastLap = state.lap;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextLog)
        {
            SPDLOG_DEBUG("[mock] {:.0f} m along, {:.1f} m/s, heading {:.0f} deg", state.alongM,
                         state.speedMps, state.headingDeg);
            nextLog = now + std::chrono::seconds(5);
        }

        nextTick += tick;
        if (nextTick > now)
        {
            std::this_thread::sleep_for(nextTick - now);
        }
        else
        {
            // Fell behind -- do not try to catch up by spinning, or a stall
            // becomes a burst of samples at the wrong rate.
            nextTick = now;
        }
    }

    SPDLOG_INFO("[mock] shutting down after {} epochs", publishers.sequence());
    return 0;
}
