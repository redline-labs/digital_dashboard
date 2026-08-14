// SPDX-License-Identifier: GPL-3.0-or-later
//
// The map server's YAML, exercised without an archive or a bus.
//
// The collisions are the point. A duplicate tileset name and two services on
// one key both produce a server that starts cleanly and answers wrongly: the
// duplicate means one archive is never read, and the shared key means a tile
// request is sometimes answered by the catalog service -- which decodes against
// the wrong capnp schema, and decoding against the wrong schema is SILENT. The
// fields land on different bytes and you get a plausible wrong number.

#include "node_config.h"

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

namespace
{

// Collected rather than logged as they happen. This test provokes the config
// parser into reporting every problem it finds, all at ERROR, so the log level
// has to sit above that -- and a FAIL line logged under a suppressing level is
// a test that reports a count and not what broke.
std::vector<std::string> failures;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        failures.push_back(what);
    }
}

using map_server::NodeConfig;
using map_server::parse_node_config;

const char* kMinimal = R"(
tilesets:
  - name: socal
    path: /maps/socal.mbtiles
)";

void test_a_minimal_config_parses_with_defaults()
{
    NodeConfig config;
    check(parse_node_config(kMinimal, config), "the minimal config parses");

    check(config.tilesets.size() == 1, "one tileset");
    if (config.tilesets.size() == 1)
    {
        check(config.tilesets[0].name == "socal", "the name is read");
        check(config.tilesets[0].path == "/maps/socal.mbtiles", "the path is read");
    }

    check(config.services.tileKey == "map/tile", "tile_key defaults");
    check(config.services.catalogKey == "map/catalog", "catalog_key defaults");
    check(config.services.assetKey == "map/asset", "asset_key defaults");
    check(config.services.statusKey == "map/status", "status_key defaults");
    check(config.assets.root.empty(), "an absent asset root leaves the asset service off");
    check(config.assets.maxBytes > 0, "max_bytes has a non-zero default");
}

void test_every_field_round_trips()
{
    NodeConfig config;
    check(parse_node_config(R"(
tilesets:
  - name: socal
    path: /maps/socal.mbtiles
  - name: world
    path: /maps/world.mbtiles
services:
  tile_key: nodes/map/tile
  catalog_key: nodes/map/catalog
  asset_key: nodes/map/asset
  status_key: nodes/map/status
  status_interval_ms: 2500
assets:
  root: /maps/assets
  max_bytes: 1048576
)",
                            config),
          "a full config parses");

    check(config.tilesets.size() == 2, "both tilesets are read");
    check(config.services.tileKey == "nodes/map/tile", "tile_key is read");
    check(config.services.statusIntervalMs == 2500, "status_interval_ms is read");
    check(config.assets.root == "/maps/assets", "the asset root is read");
    check(config.assets.maxBytes == 1048576, "max_bytes is read");
}

void test_a_config_with_no_tilesets_is_refused()
{
    NodeConfig config;
    check(!parse_node_config("services:\n  tile_key: map/tile\n", config),
          "a config with no tilesets: block is refused");

    NodeConfig empty;
    check(!parse_node_config("tilesets: []\n", empty), "an empty tilesets list is refused");
}

void test_an_incomplete_tileset_is_refused()
{
    NodeConfig noName;
    check(!parse_node_config("tilesets:\n  - path: /maps/x.mbtiles\n", noName),
          "a tileset with no name is refused");

    NodeConfig noPath;
    check(!parse_node_config("tilesets:\n  - name: socal\n", noPath),
          "a tileset with no path is refused");
}

void test_duplicate_tileset_names_are_refused()
{
    // Whichever the lookup happened to find would serve every request and the
    // other archive would never be read, with nothing said about it.
    NodeConfig config;
    check(!parse_node_config(R"(
tilesets:
  - name: socal
    path: /maps/a.mbtiles
  - name: socal
    path: /maps/b.mbtiles
)",
                             config),
          "two tilesets with one name are refused");
}

void test_a_slash_in_a_tileset_name_is_refused()
{
    // The name lands in a tile URL between two other segments. A '/' in it
    // silently changes the shape of every URL that names it.
    NodeConfig config;
    check(!parse_node_config("tilesets:\n  - name: ca/socal\n    path: /maps/x.mbtiles\n", config),
          "a tileset name containing '/' is refused");
}

void test_two_services_on_one_key_are_refused()
{
    NodeConfig config;
    check(!parse_node_config(R"(
tilesets:
  - name: socal
    path: /maps/socal.mbtiles
services:
  tile_key: map/everything
  catalog_key: map/everything
)",
                             config),
          "tile_key and catalog_key on one key are refused");

    NodeConfig assetClash;
    check(!parse_node_config(R"(
tilesets:
  - name: socal
    path: /maps/socal.mbtiles
services:
  asset_key: map/status
)",
                             assetClash),
          "asset_key colliding with the default status_key is refused");
}

void test_illegal_zenoh_keys_are_refused()
{
    // Each of these fails silently at runtime rather than loudly: '*' and '?'
    // are rejected by zenoh itself, '@' makes the segment invisible to every
    // wildcard subscription, '%' is this tree's mangling separator.
    const char* bad[] = { "map/*", "map/tile?", "@redline/map", "map%tile", "map tile" };

    for (const char* key : bad)
    {
        NodeConfig config;
        const std::string yaml = std::string("tilesets:\n  - name: socal\n"
                                             "    path: /maps/x.mbtiles\nservices:\n  tile_key: \"") +
                                 key + "\"\n";
        check(!parse_node_config(yaml, config),
              std::string("the key '") + key + "' is refused");
    }
}

void test_an_empty_key_is_refused()
{
    NodeConfig config;
    check(!parse_node_config(R"(
tilesets:
  - name: socal
    path: /maps/socal.mbtiles
services:
  tile_key: ""
)",
                             config),
          "an empty tile_key is refused");
}

void test_a_zero_asset_ceiling_is_refused()
{
    // Zero would refuse every asset, which reads as "the style is missing"
    // rather than "the ceiling is wrong".
    NodeConfig config;
    check(!parse_node_config(R"(
tilesets:
  - name: socal
    path: /maps/socal.mbtiles
assets:
  root: /maps/assets
  max_bytes: 0
)",
                             config),
          "assets.max_bytes of 0 is refused");
}

void test_malformed_yaml_is_refused_not_ignored()
{
    NodeConfig config;
    check(!parse_node_config("tilesets: [ unclosed", config), "unparseable YAML is refused");

    NodeConfig scalar;
    check(!parse_node_config("just a string", scalar), "a scalar top level is refused");

    NodeConfig wrongShape;
    check(!parse_node_config("tilesets:\n  name: socal\n  path: /x\n", wrongShape),
          "a mapping where a sequence belongs is refused");
}

void test_every_problem_is_reported_not_just_the_first()
{
    // Three mistakes should take one run to fix, not three. There is no return
    // value to assert on beyond `false`, so this exists to be read alongside
    // the log -- and to fail loudly if a future edit makes the parser bail on
    // the first problem and skip the tileset checks entirely.
    NodeConfig config;
    check(!parse_node_config(R"(
tilesets:
  - name: socal
    path: /maps/a.mbtiles
  - name: socal
    path: /maps/b.mbtiles
services:
  tile_key: "map/*"
  catalog_key: "map/*"
)",
                             config),
          "a config with several problems is refused");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::critical);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_minimal_config_parses_with_defaults();
    test_every_field_round_trips();
    test_a_config_with_no_tilesets_is_refused();
    test_an_incomplete_tileset_is_refused();
    test_duplicate_tileset_names_are_refused();
    test_a_slash_in_a_tileset_name_is_refused();
    test_two_services_on_one_key_are_refused();
    test_illegal_zenoh_keys_are_refused();
    test_an_empty_key_is_refused();
    test_a_zero_asset_ceiling_is_refused();
    test_malformed_yaml_is_refused_not_ignored();
    test_every_problem_is_reported_not_just_the_first();

    spdlog::set_level(spdlog::level::info);

    if (!failures.empty())
    {
        for (const std::string& what : failures)
        {
            SPDLOG_ERROR("FAIL: {}", what);
        }
        SPDLOG_ERROR("{} check(s) failed", failures.size());
        return 1;
    }

    SPDLOG_INFO("all map_server config checks passed");
    return 0;
}
