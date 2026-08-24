// SPDX-License-Identifier: GPL-3.0-or-later
//
// The config surface, and the mistakes that make a node start cleanly and
// behave wrongly.
//
// Two are worth the test: a schema name this build does not know -- the
// subscriber would then decode against nothing and the node would sit silent
// forever -- and two topics sharing a key, which interleaves two message types
// on one topic so a subscriber decodes whichever arrives against the schema it
// expected. Neither is visible at startup without this check.

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

void test_a_minimal_config_parses_with_defaults()
{
    map_match::NodeConfig config;
    const bool ok = map_match::parse_node_config("graph: /tmp/socal.graph\n", config);

    check(ok, "a config with only a graph path parses");
    check(config.graphPath == "/tmp/socal.graph", "keeping the path");
    check(config.position.positionKey == "nodes/bd992/gsof/lat_long_height",
          "and defaulting to the three GSOF record topics");
    check(config.position.velocityKey == "nodes/bd992/gsof/velocity", "the velocity one");
    check(config.position.sigmaKey == "nodes/bd992/gsof/position_sigma", "and the accuracy one");
    check(config.services.horizonKey == "nodes/map_match/horizon", "and the horizon topic");
    check(config.match.beamWidth > 0, "and a usable beam width");
}

void test_the_graph_path_is_required()
{
    map_match::NodeConfig config;
    check(!map_match::parse_node_config("position:\n  position_key: a/b\n", config),
          "a config with no graph is refused");
}

void test_everything_can_be_overridden()
{
    const std::string yaml = R"(
graph: /data/graph.bin
position:
  position_key: sensors/gnss
  velocity_key: sensors/gnss_velocity
  sigma_key: sensors/gnss_sigma
  stale_after_ms: 500
match:
  search_radius_m: 25.0
  beam_width: 3
  min_sigma_m: 1.0
  max_sigma_m: 50.0
  default_sigma_m: 12.0
  transition_beta_m: 30.0
  heading_valid_above_mps: 0.5
  lookahead_m: 500
services:
  horizon_key: nav/horizon
  status_key: nav/status
  horizon_interval_ms: 250
  status_interval_ms: 1000
)";

    map_match::NodeConfig config;
    check(map_match::parse_node_config(yaml, config), "a fully specified config parses");
    check(config.position.positionKey == "sensors/gnss", "the position key");
    check(config.position.velocityKey == "sensors/gnss_velocity", "the velocity key");
    check(config.position.sigmaKey == "sensors/gnss_sigma", "the accuracy key");
    check(config.position.staleAfterMs == 500, "the staleness window");
    check(config.match.beamWidth == 3, "the beam width");
    check(config.match.lookaheadM == 500, "the lookahead");
    check(config.services.horizonKey == "nav/horizon", "and the horizon key");
}

void test_the_three_record_topics_must_differ()
{
    // Pointing two of them at one key is not a typo the node can survive: the
    // subscriber would decode a velocity record against the position schema and
    // hand back a plausible wrong answer, because capnp reads the same bytes at
    // different offsets rather than failing.
    map_match::NodeConfig config;
    const bool ok = map_match::parse_node_config(
        "graph: /tmp/g\nposition:\n  position_key: same/key\n  velocity_key: same/key\n", config);
    check(!ok, "two record subscriptions on one key are refused");
}

void test_two_topics_on_one_key_are_refused()
{
    map_match::NodeConfig config;
    const bool ok = map_match::parse_node_config(
        "graph: /tmp/g\nservices:\n  horizon_key: same/key\n  status_key: same/key\n", config);
    check(!ok, "two publishers on one key are refused");

    map_match::NodeConfig other;
    const bool alsoOk = map_match::parse_node_config(
        "graph: /tmp/g\nposition:\n  position_key: same/key\nservices:\n  horizon_key: same/key\n",
        other);
    check(!alsoOk, "and so is publishing onto the topic we subscribe");
}

void test_a_bad_zenoh_key_is_refused()
{
    // Zenoh keys outside the allowed charset fail SILENTLY -- the publisher
    // refuses and nothing subscribes. Caught here so a typo is a startup error
    // rather than a topic nobody can reach.
    map_match::NodeConfig config;
    check(!map_match::parse_node_config("graph: /tmp/g\nservices:\n  horizon_key: 'has spaces'\n",
                                        config),
          "a key with a space is refused");

    map_match::NodeConfig other;
    check(!map_match::parse_node_config("graph: /tmp/g\nservices:\n  horizon_key: 'has*star'\n",
                                        other),
          "and so is one with a wildcard");
}

void test_an_impossible_sigma_range_is_refused()
{
    map_match::NodeConfig config;
    check(!map_match::parse_node_config(
              "graph: /tmp/g\nmatch:\n  min_sigma_m: 30.0\n  max_sigma_m: 5.0\n", config),
          "a minimum sigma above the maximum is refused");

    map_match::NodeConfig other;
    check(!map_match::parse_node_config("graph: /tmp/g\nmatch:\n  beam_width: 0\n", other),
          "and a beam width of zero");
}

void test_every_problem_is_reported_at_once()
{
    // The accumulating parse: three mistakes should take one run to fix.
    map_match::NodeConfig config;
    const bool ok = map_match::parse_node_config(
        "position:\n  position_key: 'bad key'\nservices:\n  horizon_key: 'also bad'\n", config);
    check(!ok, "a config with several problems is refused");
}

void test_a_malformed_document_is_refused()
{
    map_match::NodeConfig config;
    check(!map_match::parse_node_config("just a string", config),
          "a document that is not a mapping is refused");
    check(!map_match::parse_node_config("graph: [\n", config), "and so is broken YAML");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::critical);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_minimal_config_parses_with_defaults();
    test_the_graph_path_is_required();
    test_everything_can_be_overridden();
    test_the_three_record_topics_must_differ();
    test_two_topics_on_one_key_are_refused();
    test_a_bad_zenoh_key_is_refused();
    test_an_impossible_sigma_range_is_refused();
    test_every_problem_is_reported_at_once();
    test_a_malformed_document_is_refused();

    spdlog::set_level(spdlog::level::info);
    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all map_match config checks passed");
    return 0;
}
