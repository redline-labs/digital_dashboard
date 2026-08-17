// SPDX-License-Identifier: GPL-3.0-or-later
//
// The YAML surface. Same shape as nodes/map_match/tests/test_config.cpp.

#include <string>

#include <spdlog/spdlog.h>

#include "node_config.h"

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

// An empty document is legal here, unlike every other node in this tree: the
// mock is meant to run with no config at all.
void test_an_empty_config_is_all_defaults()
{
    bd992_mock::NodeConfig config;
    check(bd992_mock::parse_node_config("services: {}\n", config),
          "a config with nothing in it parses");

    check(config.services.routeKey == "map/route", "the route key defaults to map_server's");
    check(config.services.trackDetailKey == "map/track_detail", "so does the track detail key");
    check(config.services.graph.empty(), "the graph name is empty, meaning ask the server");
    check(config.services.trackset.empty(), "and so is the trackset");
    check(config.publish.topicPrefix == "nodes/bd992", "publishing on the real bd992 prefix");
    check(config.publish.rateHz == 10, "at 10 Hz");
    check(config.publish.publishRecords, "with the per-record topics on");
    check(config.publish.positionNoiseM == 0.0, "and no noise, so the car is on the road");
}

void test_everything_can_be_overridden()
{
    const std::string yaml = R"(
services:
  graph_info_key: map/graph2
  route_key: map/route2
  nearest_key: map/nearest2
  track_catalog_key: map/tc2
  track_detail_key: map/td2
  request_timeout_ms: 9000
  graph: socal
  trackset: tracks
vehicle:
  cruise_speed_mps: 44.0
  accel_mps2: 3.5
  brake_mps2: 9.0
  lateral_accel_mps2: 12.0
  heading_lookahead_m: 2.0
publish:
  topic_prefix: nodes/fake_bd992
  publish_records: false
  rate_hz: 20
  ellipsoid_height_m: 120.0
  position_noise_m: 0.5
  noise_seed: 42
)";

    bd992_mock::NodeConfig config;
    check(bd992_mock::parse_node_config(yaml, config), "a fully specified config parses");

    check(config.services.graph == "socal", "the graph name is kept");
    check(config.services.trackset == "tracks", "the trackset name is kept");
    check(config.services.requestTimeoutMs == 9000, "the timeout is kept");
    check(config.vehicle.cruiseSpeedMps == 44.0, "the cruise speed is kept");
    check(config.vehicle.lateralAccelMps2 == 12.0, "the cornering limit is kept");
    check(config.publish.topicPrefix == "nodes/fake_bd992", "the prefix is kept");
    check(!config.publish.publishRecords, "the per-record topics can be turned off");
    check(config.publish.rateHz == 20, "the rate is kept");
    check(config.publish.positionNoiseM == 0.5, "the noise is kept");
    check(config.publish.noiseSeed == 42, "and its seed, so a bad run can be repeated");
}

void test_a_bad_zenoh_key_is_refused()
{
    bd992_mock::NodeConfig config;
    check(!bd992_mock::parse_node_config("services:\n  route_key: 'map/route?'\n", config),
          "a key outside the allowed charset is refused rather than silently unreachable");
}

void test_two_services_on_one_key_are_refused()
{
    const std::string yaml = R"(
services:
  route_key: map/same
  nearest_key: map/same
)";
    bd992_mock::NodeConfig config;
    check(!bd992_mock::parse_node_config(yaml, config),
          "two services sharing a key is refused: a request would go to whichever answers first");
}

void test_impossible_vehicles_are_refused()
{
    bd992_mock::NodeConfig config;

    check(!bd992_mock::parse_node_config("vehicle:\n  cruise_speed_mps: 0\n", config),
          "a cruise speed of zero is refused: it would never move");
    check(!bd992_mock::parse_node_config("vehicle:\n  brake_mps2: -1\n", config),
          "negative braking is refused");
    check(!bd992_mock::parse_node_config("vehicle:\n  lateral_accel_mps2: 0\n", config),
          "a cornering limit of zero is refused: every corner would be a full stop");
    check(!bd992_mock::parse_node_config("publish:\n  rate_hz: 0\n", config),
          "a rate of zero is refused");
    check(!bd992_mock::parse_node_config("publish:\n  position_noise_m: -1.0\n", config),
          "negative noise is refused");
}

void test_every_problem_is_reported_at_once()
{
    // Three separate mistakes; the parser must not stop at the first, or fixing
    // a config takes as many runs as it has errors.
    const std::string yaml = R"(
services:
  route_key: 'map/route?'
vehicle:
  cruise_speed_mps: 0
publish:
  rate_hz: 0
)";
    bd992_mock::NodeConfig config;
    check(!bd992_mock::parse_node_config(yaml, config), "a config with three mistakes is refused");
}

void test_a_malformed_document_is_refused()
{
    bd992_mock::NodeConfig config;
    check(!bd992_mock::parse_node_config("- a\n- list\n", config),
          "a document that is not a mapping is refused");
    check(!bd992_mock::parse_node_config("services:\n  - not a mapping\n", config),
          "a section that is not a mapping is refused");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::critical);
    spdlog::set_pattern("[%^%l%$] %v");

    test_an_empty_config_is_all_defaults();
    test_everything_can_be_overridden();
    test_a_bad_zenoh_key_is_refused();
    test_two_services_on_one_key_are_refused();
    test_impossible_vehicles_are_refused();
    test_every_problem_is_reported_at_once();
    test_a_malformed_document_is_refused();

    spdlog::set_level(spdlog::level::info);
    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all bd992_mock config checks passed");
    return 0;
}
