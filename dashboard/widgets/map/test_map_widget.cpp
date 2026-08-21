// SPDX-License-Identifier: GPL-3.0-or-later
//
// The map widget through the paths the apps and the agent interface use.
//
// Not a pixel test -- that is the screenshot loop's job. What is checked here
// is what a screenshot cannot tell apart: the config surface every layout goes
// through, and that the widget CONSTRUCTS AND PAINTS under
// QT_QPA_PLATFORM=offscreen.
//
// That last one is the reason this file exists. A surface-bound renderer --
// anything QRhiWidget or QOpenGLWidget derived -- reports "QRhi is not supported
// on this platform" offscreen and hands back a null map, so the map would be
// invisible to ui_screenshot and to every gui test, and the first symptom is a
// segfault rather than a blank image. Painting into a QImage here is the
// standing check that this widget does not have that problem.

#include "map/config.h"
#include "map/map_widget.h"
#include "map/labels.h"

#include "config_codec/config_json.h"

#include <QAbstractButton>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>
#include <QPen>
#include <QPainterPath>
#include <QFontMetricsF>
#include <QImage>
#include <QWheelEvent>

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

#include <memory>
#include <utility>
#include <string>

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

// ============================================================================
// Config
// ============================================================================

void test_defaults_point_somewhere_real()
{
    // Irvine, CA. The archive on the bench covers Southern California, so a
    // widget that opens on (0, 0) shows an empty rectangle and looks exactly
    // like a misconfigured one.
    MapConfig_t config;
    check(config.center_latitude > 33.0 && config.center_latitude < 34.5,
          "the default latitude is in Southern California");
    check(config.center_longitude < -117.0 && config.center_longitude > -119.0,
          "the default longitude is in Southern California");
    check(config.tile_zenoh_key == "map/tile",
          "and the default tile key matches configs/map_server.yaml");
    // NOT "matches the archive" any more. min_zoom/max_zoom bound the camera;
    // which tile level is drawn comes from the server, which reports what its
    // archive actually holds on every reply. The default lets the camera reach
    // about three levels past a street-level archive, which is roughly where
    // there is too little left in frame for it to read as a map.
    check(config.max_zoom > 14 && config.max_zoom <= 22,
          "the default camera maximum allows some magnification past a z14 archive");
}

void test_out_of_range_values_are_clamped_not_refused()
{
    MapConfig_t config;
    config.zoom = 99.0;
    config.center_latitude = 91.0;
    config.center_longitude = 400.0;
    config.marker_size = 999;
    config.style.road_width_scale = 100.0;

    const std::vector<std::string> notes = validate(config);

    check(config.zoom == 22.0, "a zoom past 22 is clamped");
    check(config.center_latitude < 85.06 && config.center_latitude > 85.05,
          "a latitude outside Web Mercator is clamped to its edge -- past it the projection "
          "runs to infinity and paints nothing at all");
    check(config.center_longitude == 180.0, "a longitude past 180 is clamped");
    check(config.marker_size == 64, "an absurd marker size is clamped");
    check(config.style.road_width_scale == 8.0, "and the nested style is validated too");
    check(!notes.empty(), "with every clamp reported rather than done silently");
}

void test_an_inverted_zoom_range_is_put_in_order()
{
    // min > max would make every tile request fall outside the range and the
    // map would be permanently, silently blank.
    MapConfig_t config;
    config.min_zoom = 14;
    config.max_zoom = 4;

    (void)validate(config);

    check(config.min_zoom == 4, "the smaller becomes min_zoom");
    check(config.max_zoom == 14, "and the larger becomes max_zoom");
}

void test_bearing_wraps_rather_than_clamping()
{
    MapConfig_t config;

    config.bearing = 370.0;
    (void)validate(config);
    check(config.bearing > 9.9 && config.bearing < 10.1, "370 wraps to 10");

    config.bearing = -10.0;
    (void)validate(config);
    check(config.bearing > 349.9 && config.bearing < 350.1,
          "-10 wraps to 350, rather than clamping a legitimate heading to due north");
}

// ============================================================================
// The JSON patch path -- what widget.set_config drives
// ============================================================================

void test_a_partial_patch_applies_and_a_bad_field_is_reported()
{
    MapConfig_t config;
    const double originalLatitude = config.center_latitude;

    std::vector<std::string> errors;
    config_codec::applyJson(nlohmann::json { { "zoom", 15.0 } }, config, "", errors);
    check(errors.empty(), "a partial patch applies cleanly");
    check(config.zoom == 15.0, "and changes the field it names");
    check(config.center_latitude == originalLatitude, "and leaves the others alone");

    // The all-or-nothing part is widget_methods' doing: it patches a COPY and
    // installs it only when errors is empty. What is pinned here is that a bad
    // field is reported at all -- without that the copy is installed and the
    // widget ends up in a state no config file could describe.
    MapConfig_t second;
    std::vector<std::string> secondErrors;
    config_codec::applyJson(nlohmann::json { { "zoom", 12.0 }, { "no_such_field", 1 } }, second,
                            "", secondErrors);
    check(!secondErrors.empty(), "a patch naming an unknown field is reported");
}

void test_the_nested_style_is_patchable()
{
    // The style is a nested reflected struct, so the editor and the agent
    // interface reach it by path. If they could not, restyling would mean a
    // rebuild -- which is most of why the style is a struct and not a document.
    MapConfig_t config;
    std::vector<std::string> errors;
    config_codec::applyJson(nlohmann::json { { "style", { { "water", "#123456" } } } }, config, "",
                            errors);

    check(errors.empty(), "a nested style patch applies");
    check(config.style.water.value() == "#123456", "and reaches the nested field");
    check(config.style.background.value() != "#123456", "without disturbing its siblings");
}

void test_the_doubly_nested_style_groups_are_patchable()
{
    // style.widths and style.detail sit TWO levels down from the widget config,
    // which is one level deeper than anything else here reaches. If the codec
    // stopped recursing, per-layer styling would silently be read-only over
    // widget.set_config while still looking editable in the panel.
    MapConfig_t config;
    std::vector<std::string> errors;
    config_codec::applyJson(nlohmann::json { { "style",
                                               { { "widths", { { "motorway", 9.0 } } },
                                                 { "detail", { { "building", 15 } } } } } },
                            config, "", errors);

    check(errors.empty(), "a two-level-deep patch applies");
    check(config.style.widths.motorway == 9.0, "and reaches the width");
    check(config.style.detail.building == 15, "and the threshold");
    check(config.style.widths.road_minor == 1.5, "leaving the siblings at their defaults");
}

void test_style_groups_are_clamped_not_refused()
{
    // Every other config in this tree clamps rather than throwing out a layout,
    // and the nested groups have to be reached by validate() for that to hold.
    MapConfig_t config;
    config.style.widths.motorway = 5000.0;
    config.style.detail.building = 99;
    config.style.label_halo_width = -3.0;
    config.style.label_spacing = 900;

    const std::vector<std::string> notes = validate(config);

    check(config.style.widths.motorway == 40.0, "an absurd width is clamped");
    check(config.style.detail.building == 22, "a threshold past z22 is clamped");
    check(config.style.label_halo_width == 0.0, "a negative halo becomes no halo");
    check(config.style.label_spacing == 64, "and the spacing is bounded");
    check(notes.size() >= 4, "with every clamp reported rather than done silently");
}

void test_the_config_describes_itself()
{
    const nlohmann::json described = config_codec::describeType<MapConfig_t>();
    check(described.contains("fields"), "the config describes its fields");
    if (!described.contains("fields"))
    {
        return;
    }

    // "fields" is an OBJECT keyed by field name, not an array of entries.
    const auto& fields = described["fields"];
    check(fields.contains("tileset"), "tileset is editable");
    check(fields.contains("follow_vehicle"), "follow_vehicle is editable");
    check(fields.contains("style"), "and the style is exposed rather than hidden");
    check(fields.contains("track_width") && fields.contains("marker_outline_color"),
          "the vehicle's own styling is editable too");
    check(fields.contains("position_schema_type") &&
              fields["position_schema_type"].value("type", "") == "enum",
          "the schema type is offered as an enum the editor can list");
}

// ============================================================================
// Label classification
// ============================================================================

void test_place_classes_order_labels_by_importance()
{
    // Which name survives when two labels collide. The road classification is
    // the tessellator's business and is tested there; this is the label pass's
    // half of the same idea.
    using map_widget::placePriority;

    check(placePriority("country") > placePriority("state"), "country beats state");
    check(placePriority("state") > placePriority("city"), "state beats city");
    check(placePriority("city") > placePriority("town"), "city beats town");
    check(placePriority("town") > placePriority("village"), "town beats village");
    check(placePriority("village") > placePriority("a_class_from_the_future"),
          "and anything known beats a class we do not recognise");
}


// A label point with whatever attributes the caller names. `rank` and
// `population` are written by map_build as numbers, so they go in as numbers
// here rather than as strings -- reading them as text is how they came to be
// ignored in the first place.
mvt::Layer labelLayerWith(const std::string& name,
                          const std::vector<std::pair<std::string, mvt::Value>>& attributes)
{
    mvt::Layer layer;
    layer.name = name;
    layer.extent = 4096;

    mvt::Feature feature;
    feature.type = mvt::GeomType::Point;
    feature.rings.push_back({ { 2048, 2048 } });
    for (const auto& [key, value] : attributes)
    {
        feature.tags.push_back(static_cast<std::uint32_t>(layer.keys.size()));
        feature.tags.push_back(static_cast<std::uint32_t>(layer.values.size()));
        layer.keys.push_back(key);
        layer.values.push_back(value);
    }
    layer.features.push_back(std::move(feature));
    return layer;
}

map_widget::LabelRank rankOfPlace(const std::vector<std::pair<std::string, mvt::Value>>& attributes)
{
    const mvt::Layer layer = labelLayerWith("place", attributes);
    return map_widget::placeRank(layer, layer.features.front());
}

// The tiler's own ordering beats the class name, because the class name cannot
// distinguish the two places the rank was written to separate.
void test_a_places_rank_attribute_outranks_its_class_name()
{
    // rank 0 is a country, rank 8 a locality -- LOW is important, which is the
    // opposite sense to the value this returns, and inverting it is the bug
    // this test exists to catch.
    check(rankOfPlace({ { "rank", mvt::Value(std::int64_t { 0 }) } }).tier >
              rankOfPlace({ { "rank", mvt::Value(std::int64_t { 8 }) } }).tier,
          "a low rank sorts above a high one");

    // map_build writes these through builder.number(), which picks an integer
    // or a double encoding by value. Reading only one of them yields a silent
    // zero and the old decode-order behaviour back.
    check(rankOfPlace({ { "rank", mvt::Value(2.0) } }).tier ==
              rankOfPlace({ { "rank", mvt::Value(std::int64_t { 2 }) } }).tier,
          "a rank encoded as a double reads the same as one encoded as an integer");
}

// The headline case: the class/rank tag alone cannot tell a large town from a
// small city, because where that line falls is local convention.
void test_a_large_town_outranks_a_small_city()
{
    const auto bigTown = rankOfPlace({ { "rank", mvt::Value(std::int64_t { 3 }) },
                                       { "population", mvt::Value(std::int64_t { 400'000 }) } });
    const auto smallCity = rankOfPlace({ { "rank", mvt::Value(std::int64_t { 2 }) },
                                         { "population", mvt::Value(std::int64_t { 3'000 }) } });
    check(bigTown.tier >= smallCity.tier,
          "a town of 400 000 is not buried by a city of 3 000");

    // Promotion is UPWARD only, for map_rules' reason: a city tagged with a
    // small population is far more often a stale tag than a tiny city.
    check(smallCity.tier == rankOfPlace({ { "rank", mvt::Value(std::int64_t { 2 }) } }).tier,
          "a small population never demotes a city");

    // ... and it does not run away with itself. A big town is not a country.
    check(bigTown.tier < rankOfPlace({ { "rank", mvt::Value(std::int64_t { 0 }) } }).tier,
          "a large town still loses to a country");
}

// Within one tier the bigger place wins, rather than whichever tile decoded
// first.
void test_population_settles_a_collision_between_two_cities()
{
    const auto big = rankOfPlace({ { "rank", mvt::Value(std::int64_t { 2 }) },
                                   { "population", mvt::Value(std::int64_t { 900'000 }) } });
    const auto small = rankOfPlace({ { "rank", mvt::Value(std::int64_t { 2 }) },
                                     { "population", mvt::Value(std::int64_t { 210'000 }) } });
    check(big.tier == small.tier, "two cities share a tier");
    check(big.magnitude > small.magnitude, "and the bigger one wins on magnitude");
}

// The bench archive predates `rank`, so the class name has to keep working --
// requiring the attribute would blank every label on it.
void test_a_place_without_rank_falls_back_to_its_class()
{
    check(rankOfPlace({ { "class", mvt::Value(std::in_place_type<std::string>, "country") } }).tier >
              rankOfPlace({ { "class", mvt::Value(std::in_place_type<std::string>, "town") } }).tier,
          "with no rank attribute the class still orders the labels");
}

// A circuit ranks between a town and a city: worth seeing from a distance, but
// it must not push a city name off a country view.
void test_a_track_ranks_between_a_town_and_a_city()
{
    const mvt::Layer layer = labelLayerWith("track_label", {});
    const auto track = map_widget::trackRank(layer, layer.features.front());

    check(track.tier > rankOfPlace({ { "rank", mvt::Value(std::int64_t { 3 }) } }).tier,
          "a circuit outranks a town");
    check(track.tier < rankOfPlace({ { "rank", mvt::Value(std::int64_t { 2 }) } }).tier,
          "and loses to a city");
}

// map_build writes a length-derived rank on every circuit, 0 for the longest.
// Ignoring it is a map that labels the kart track and hides Spa.
void test_a_longer_circuit_outranks_a_shorter_one()
{
    const mvt::Layer longTrack =
        labelLayerWith("track_label", { { "rank", mvt::Value(std::int64_t { 0 }) } });
    const mvt::Layer shortTrack =
        labelLayerWith("track_label", { { "rank", mvt::Value(std::int64_t { 18 }) } });

    const auto big = map_widget::trackRank(longTrack, longTrack.features.front());
    const auto small = map_widget::trackRank(shortTrack, shortTrack.features.front());

    check(big.tier == small.tier, "both are circuits and share a tier");
    check(big.magnitude > small.magnitude, "the longer circuit keeps its name");
}


// The cache used to CLEAR at its ceiling. A label costs 0.88 ms to render, so
// a viewport holding forty of them paid two dropped frames on the frame that
// crossed the threshold -- and again on any frame that crossed back.
void test_the_label_cache_evicts_rather_than_clearing()
{
    map_widget::LabelCache cache;
    const QFont font;
    const QColor halo(Qt::black);
    const QColor text(Qt::white);

    // Fill past the ceiling. The exact ceiling is the cache's business; what
    // matters is that it stops growing and does not empty.
    for (int i = 0; i < 700; ++i)
    {
        cache.entryFor(QString::number(i), font, 3.0, halo, text, 1.0);
    }

    check(cache.size() > 1, "the cache did not empty itself when it filled up");
    check(cache.size() <= 512, "and it is still bounded");
}

// Eviction has to drop what left the screen, not what is on it. A plain FIFO
// evicts the label you are looking at while keeping ones you drove past.
//
// The scenario is the smallest one that tells the two apart: fill the cache
// exactly, hit the OLDEST entry so a least-recently-used policy promotes it,
// then force one eviction. LRU drops the runner-up; FIFO drops the entry that
// was just used.
void test_the_label_cache_evicts_the_least_recently_used()
{
    map_widget::LabelCache cache;
    const QFont font;
    const QColor halo(Qt::black);
    const QColor text(Qt::white);
    const auto get = [&](const QString& what) {
        cache.entryFor(what, font, 3.0, halo, text, 1.0);
    };

    const QString onScreen = QStringLiteral("Irvine");
    get(onScreen);
    while (cache.size() < 512)
    {
        get(QString::number(cache.size()));
    }
    check(cache.contains(onScreen), "the oldest entry is present before the eviction");

    // The viewport is still showing it, so it is asked for again -- which is
    // exactly what a stationary map does every frame.
    get(onScreen);

    // One more name arrives, so something has to go.
    get(QStringLiteral("a name that has not been seen before"));

    check(cache.contains(onScreen),
          "the label the viewport is still asking for survived; a FIFO would have dropped it");
}


// ============================================================================
// Road labels
// ============================================================================

// A `transportation_name` line feature carrying a name, spanning the tile.
mvt::Tile roadNameTile(const std::string& name, const std::string& roadClass,
                       std::int32_t y = 2048, std::int32_t fromX = 0, std::int32_t toX = 4096)
{
    mvt::Layer layer;
    layer.name = "transportation_name";
    layer.extent = 4096;
    layer.keys = { "name:latin", "class" };
    layer.values = { mvt::Value(std::in_place_type<std::string>, name),
                     mvt::Value(std::in_place_type<std::string>, roadClass) };

    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.rings.push_back({ { fromX, y }, { toX, y } });
    feature.tags = { 0, 0, 1, 1 };
    layer.features.push_back(std::move(feature));

    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));
    return tile;
}

map_widget::LabelStats placeOnto(const std::vector<map_widget::LabelTile>& tiles,
                                 const MapStyle_t& style, double zoom = 14.0)
{
    // A QImage painter: no window, no GPU, and exactly the raster engine the
    // real label pass runs on.
    QImage canvas(800, 600, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);

    const map_widget::Camera camera { { 33.6865966, -117.8557874 }, zoom, 0.0 };
    const map_widget::Projection projection(camera, 800, 600, 1.0);

    map_widget::LabelCache cache;
    return map_widget::paintLabels(painter, projection, tiles, style, cache);
}

// The headline gap: `transportation_name` is built, shipped, decoded and was
// then ignored, so a nav map had no street names at all.
void test_a_road_name_is_placed()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto tile = std::make_shared<mvt::Tile>(roadNameTile("Main Street", "minor"));

    const MapStyle_t style;
    const auto stats = placeOnto({ { irvine, tile } }, style);
    check(stats.placed == 1, "the street name is drawn, got " + std::to_string(stats.placed));
}

// A road crosses every tile it passes through and each tile carries the WHOLE
// name, so without dedup one street is labelled once per tile on screen.
void test_a_road_crossing_several_tiles_is_named_once()
{
    const auto tile = std::make_shared<mvt::Tile>(roadNameTile("Main Street", "minor"));

    std::vector<map_widget::LabelTile> tiles;
    for (std::uint32_t x = 2827; x <= 2829; ++x)
    {
        for (std::uint32_t y = 6561; y <= 6563; ++y)
        {
            tiles.push_back({ map_widget::TileId { 14, x, y }, tile });
        }
    }

    const MapStyle_t style;
    const auto stats = placeOnto(tiles, style);
    check(stats.placed == 1,
          "nine tiles of the same road produce one label, got " + std::to_string(stats.placed));
}

// A name wider than the road it names reads as text floating over the map.
// MVT leaves two-metre stubs in tile corners wherever it clips a road, and
// without this each of them claims a label.
void test_a_stub_too_short_for_its_name_is_not_labelled()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto stub = std::make_shared<mvt::Tile>(
        roadNameTile("A Very Long Street Name Indeed", "minor", 2048, 2040, 2056));

    const MapStyle_t style;
    const auto stats = placeOnto({ { irvine, stub } }, style);
    check(stats.placed == 0, "a 16-unit stub does not get a 29-character name");
}

// Street names are z14 information: below that the roads themselves are a grey
// smear and a name labels something the driver cannot see.
void test_road_labels_respect_their_zoom_floor_and_their_toggle()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto tile = std::make_shared<mvt::Tile>(roadNameTile("Main Street", "minor"));

    MapStyle_t style;
    check(placeOnto({ { irvine, tile } }, style, 14.0).placed == 1, "drawn at the floor");
    check(placeOnto({ { irvine, tile } }, style, 12.0).placed == 0, "and not below it");

    style.show_road_labels = false;
    check(placeOnto({ { irvine, tile } }, style, 14.0).placed == 0,
          "and not at all when the toggle is off");
}

// A numbered route with no name still has something to say, and map_build emits
// it into this layer for exactly that reason.
void test_a_numbered_route_falls_back_to_its_ref()
{
    mvt::Layer layer;
    layer.name = "transportation_name";
    layer.extent = 4096;
    layer.keys = { "ref", "class" };
    layer.values = { mvt::Value(std::in_place_type<std::string>, "I-405"),
                     mvt::Value(std::in_place_type<std::string>, "motorway") };
    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.rings.push_back({ { 0, 2048 }, { 4096, 2048 } });
    feature.tags = { 0, 0, 1, 1 };
    layer.features.push_back(std::move(feature));

    auto tile = std::make_shared<mvt::Tile>();
    tile->layers.push_back(std::move(layer));

    const MapStyle_t style;
    const auto stats = placeOnto({ { map_widget::TileId { 14, 2828, 6562 }, tile } }, style);
    check(stats.placed == 1, "an unnamed motorway is labelled with its route number");
}

// A street name must not push a town off the map, but it should outrank the
// name of a junction three miles away.
void test_a_road_ranks_between_a_neighbourhood_and_a_locality()
{
    mvt::Layer roads;
    roads.name = "transportation_name";
    roads.extent = 4096;
    roads.keys = { "class" };
    roads.values = { mvt::Value(std::in_place_type<std::string>, "minor") };
    mvt::Feature road;
    road.type = mvt::GeomType::LineString;
    road.tags = { 0, 0 };
    roads.features.push_back(std::move(road));

    const auto rank = map_widget::roadRank(roads, roads.features.front());
    check(rank.tier < rankOfPlace({ { "rank", mvt::Value(std::int64_t { 7 }) } }).tier,
          "a street name loses to a neighbourhood");
    check(rank.tier > rankOfPlace({ { "rank", mvt::Value(std::int64_t { 8 }) } }).tier,
          "and beats a locality");
}

// Among roads the bigger road keeps its name, using the tessellator's own
// ladder rather than a second one that could drift from it.
void test_a_motorway_name_outranks_a_side_street()
{
    const auto rankOfRoad = [](const std::string& roadClass) {
        mvt::Layer layer;
        layer.name = "transportation_name";
        layer.extent = 4096;
        layer.keys = { "class" };
        layer.values = { mvt::Value(std::in_place_type<std::string>, roadClass) };
        mvt::Feature f;
        f.type = mvt::GeomType::LineString;
        f.tags = { 0, 0 };
        layer.features.push_back(std::move(f));
        return map_widget::roadRank(layer, layer.features.front());
    };

    check(rankOfRoad("motorway").magnitude > rankOfRoad("primary").magnitude,
          "a motorway name outranks a primary road's");
    check(rankOfRoad("primary").magnitude > rankOfRoad("minor").magnitude,
          "and a primary road's outranks a side street's");
}

// ============================================================================
// Construction and painting -- the offscreen check
// ============================================================================

void test_the_widget_constructs_without_a_server()
{
    // No map_server is running here. That must not be fatal: the dashboard
    // starts before its nodes do, and a widget that threw would take the whole
    // layout with it.
    MapConfig_t config;
    config.position_zenoh_key.clear();

    std::unique_ptr<MapWidget> widget;
    try
    {
        widget = std::make_unique<MapWidget>(config);
    }
    catch (const std::exception& e)
    {
        check(false, std::string("constructing the widget threw: ") + e.what());
        return;
    }

    check(widget != nullptr, "the widget constructs with no map_server on the bus");
    check(widget->getConfig().zoom == config.zoom, "and reports back the config it was given");

    widget->resize(320, 240);
    check(widget->width() == 320, "and takes a size");
}

void test_a_bad_expression_does_not_take_the_widget_down()
{
    // A layout can name an expression that does not compile against the schema.
    // The subscription is then simply absent -- logged, and the map still
    // draws. Refusing to construct would turn one typo into no dashboard.
    MapConfig_t config;
    config.position_zenoh_key = "nodes/bd992/position";
    config.latitude_expression = "this is not an expression (((";
    config.longitude_expression = "nor is this";

    std::unique_ptr<MapWidget> widget;
    try
    {
        widget = std::make_unique<MapWidget>(config);
    }
    catch (const std::exception& e)
    {
        check(false, std::string("an invalid position expression threw: ") + e.what());
        return;
    }

    check(widget != nullptr, "an invalid position expression still yields a widget");
}

void test_the_widget_paints_offscreen()
{
    // THE check this file exists for. Rendering into a QImage drives the same
    // paintEvent the screenshot path does, with no window, no GPU and no RHI.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.style.background = helpers::Color("#112233");
    config.show_status = false;

    MapWidget widget(config);
    widget.resize(400, 300);

    QImage canvas(400, 300, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    widget.render(&canvas);

    // The background colour proves the paint pass ran at all. A widget that
    // failed to initialise leaves the image exactly as it was filled.
    const QColor sampled = canvas.pixelColor(200, 20);
    check(sampled.red() == 0x11 && sampled.green() == 0x22 && sampled.blue() == 0x33,
          "the widget painted its background, so the paint path works offscreen");

    // And it did so through the GPU, not by falling back to a cleared widget.
    // There IS no fallback: without a backend the map draws labels and marker
    // over an empty background, which in a screenshot looks exactly like a
    // tileset with no coverage.
    check(widget.status().gpuReady,
          "the GPU backend came up under the offscreen platform, so the map is "
          "visible to ui_screenshot");
}

void test_the_widget_paints_at_its_screens_ratio()
{
    // The GPU frame used to be built from logical width and height and blitted
    // as it came, so on a 2x screen the map geometry was rendered at half the
    // resolution the panel has and upscaled -- underneath labels and a marker
    // that QPainter drew at the full ratio.
    //
    // Whatever ratio this run has, the frame must have been rendered at it.
    // That is trivially true at 1x and is the whole assertion under
    // map_test_widget_hidpi, which runs this same binary with
    // QT_SCALE_FACTOR=2 -- see the CMakeLists. An offscreen widget has no
    // screen to take a ratio from, so that variable is the only way to reach
    // the HiDPI path at all.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.style.background = helpers::Color("#112233");
    config.show_status = false;

    MapWidget widget(config);
    widget.resize(400, 300);

    QImage canvas(400, 300, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    widget.render(&canvas);

    if (!widget.status().gpuReady)
    {
        return;
    }

    const double ratio = widget.devicePixelRatioF();
    check(std::abs(widget.status().gpu.devicePixelRatio - ratio) < 1e-9,
          "the GPU frame is rendered at the ratio of the screen the widget is on, wanted " +
              std::to_string(ratio) + " and the frame used " +
              std::to_string(widget.status().gpu.devicePixelRatio));

    // Still the same picture, whatever the ratio: the frame carries its own
    // ratio, so the blit covers the widget rather than a quarter of it.
    const QColor sampled = canvas.pixelColor(200, 150);
    check(sampled.red() == 0x11 && sampled.green() == 0x22 && sampled.blue() == 0x33,
          "and the blit still covers the widget, got rgb(" + std::to_string(sampled.red()) +
              "," + std::to_string(sampled.green()) + "," + std::to_string(sampled.blue()) +
              ")");
}

// Draw the widget into a throwaway image, which is what makes its status mean
// something. Qt does not deliver a resize event to a widget that has never been
// shown, so a test that only resized would be reading the tile set worked out
// for Qt's default 640x480 -- and would pass while the widget drew the wrong
// thing at the wrong scale.
void render(MapWidget& widget)
{
    QImage canvas(widget.width(), widget.height(), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::black);
    widget.render(&canvas);
}

// ============================================================================
// The mouse
// ============================================================================

// Qt delivers these through the event loop in a running app; a test posts them
// straight at the widget, which reaches the same handlers.
void drag(MapWidget& widget, const QPointF& from, const QPointF& to)
{
    QMouseEvent press(QEvent::MouseButtonPress, from, from, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QApplication::sendEvent(&widget, &press);

    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &move);

    QMouseEvent release(QEvent::MouseButtonRelease, to, to, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QApplication::sendEvent(&widget, &release);
}

void scroll(MapWidget& widget, const QPointF& at, int notches)
{
    // angleDelta only, with a null pixelDelta: that is what a wheel with
    // detents sends, and it is the branch a mouse takes.
    QWheelEvent event(at, at, QPoint(0, 0), QPoint(0, notches * 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &event);
}

map_widget::Projection projectionOf(const MapWidget& widget)
{
    return map_widget::Projection(widget.status().camera, widget.width(), widget.height());
}

bool sameCoordinate(const map_widget::Coordinate& a, const map_widget::Coordinate& b)
{
    // A tenth of a microdegree is about a centimetre. The drag arithmetic is
    // exact up to floating point, so this is loose enough to survive the
    // round trip through degrees and tight enough that a pixel of slip fails.
    return std::abs(a.latitude - b.latitude) < 1e-7 &&
           std::abs(a.longitude - b.longitude) < 1e-7;
}

void test_a_map_that_was_not_asked_to_be_interactive_ignores_the_mouse()
{
    // The default, and the one every existing layout gets. A dashboard is a
    // surface people brace a hand against on a bad road; a map that panned when
    // they did would be worse than one that never moves.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.follow_vehicle = false;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);

    const map_widget::Camera before = widget.status().camera;
    drag(widget, QPointF(120.0, 90.0), QPointF(200.0, 160.0));
    scroll(widget, QPointF(200.0, 150.0), 3);

    check(widget.status().camera.center == before.center, "a drag does not move the camera");
    check(widget.status().camera.zoom == before.zoom, "and the wheel does not zoom");
    check(!widget.status().cameraMoved, "and nothing reports having been moved");
    check(widget.findChild<QAbstractButton*>() == nullptr,
          "and there is no recentre button to be found, because there is nothing to recentre");
}

void test_a_drag_keeps_the_grabbed_point_under_the_pointer()
{
    // THE assertion for panning, and the reason worldForScreen() is the
    // primitive rather than a screen-space delta. Anything that merely moves
    // the camera "about the right amount" drifts under the pointer, and on a
    // ROTATED map a screen-space delta sends the map off at an angle to the
    // drag entirely -- which is why this runs at a bearing as well as square.
    for (const double bearing : { 0.0, 37.0 })
    {
        MapConfig_t config;
        config.position_zenoh_key.clear();
        config.interactive = true;
        config.follow_vehicle = false;
        config.zoom = 14.0;
        config.bearing = bearing;

        MapWidget widget(config);
        widget.resize(400, 300);
        render(widget);

        const QPointF from(120.0, 90.0);
        const QPointF to(190.0, 145.0);

        const map_widget::Coordinate grabbed =
            projectionOf(widget).coordinateForScreen(map_widget::ScreenPoint { from.x(),
                                                                               from.y() });

        drag(widget, from, to);

        const map_widget::Coordinate under =
            projectionOf(widget).coordinateForScreen(map_widget::ScreenPoint { to.x(), to.y() });

        const std::string at = " (bearing " + std::to_string(int(bearing)) + ")";
        check(sameCoordinate(grabbed, under),
              "the place that was grabbed ends up under the pointer" + at);
        check(widget.status().cameraMoved, "and the camera reports having been moved" + at);
        check(widget.status().camera.zoom == 14.0, "with the zoom untouched" + at);
    }
}

void test_the_recentre_button_appears_with_the_pan_and_undoes_it()
{
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.interactive = true;
    // No vehicle to go back to in a test with no bus, so recentring goes back
    // to the configured centre. Same code either way -- camera() simply falls
    // through to the next source once the pan is dropped.
    config.follow_vehicle = false;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);

    auto* button = widget.findChild<QAbstractButton*>();
    check(button != nullptr, "an interactive map has a recentre button");
    if (button == nullptr)
    {
        return;
    }

    // isHidden() rather than isVisible(): a child of a widget that was never
    // shown is never "visible", so isVisible() would be false here whatever the
    // code did and the assertion would pass for the wrong reason.
    check(button->isHidden(), "hidden while the camera is still where the layout put it");

    const map_widget::Coordinate configured = widget.status().camera.center;
    drag(widget, QPointF(120.0, 90.0), QPointF(220.0, 170.0));

    check(!button->isHidden(), "shown once a drag has moved the camera");
    check(!sameCoordinate(widget.status().camera.center, configured),
          "which it has");

    button->click();

    check(sameCoordinate(widget.status().camera.center, configured),
          "clicking it puts the camera back on the configured centre");
    check(!widget.status().cameraMoved, "and the pan is forgotten");
    check(button->isHidden(), "so the button takes itself away again");
}

void test_the_wheel_zooms_about_the_pointer()
{
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.interactive = true;
    config.follow_vehicle = false;
    config.zoom = 12.0;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);

    // Deliberately NOT the centre of the widget: anchoring on the pointer and
    // anchoring on the centre are the same thing there, and this test would
    // pass on either.
    const QPointF at(90.0, 70.0);
    const map_widget::Coordinate before =
        projectionOf(widget).coordinateForScreen(map_widget::ScreenPoint { at.x(), at.y() });

    scroll(widget, at, 2);

    check(widget.status().camera.zoom > 12.0, "the wheel zooms in, got " +
                                                  std::to_string(widget.status().camera.zoom));

    const map_widget::Coordinate after =
        projectionOf(widget).coordinateForScreen(map_widget::ScreenPoint { at.x(), at.y() });
    check(sameCoordinate(before, after),
          "and the place under the pointer stays under the pointer while the scale changes");
}

void test_the_wheel_does_not_stop_the_map_following_the_vehicle()
{
    // Wanting a closer look is not asking to be left behind. Zooming while
    // Follow Vehicle is on anchors on the CENTRE instead of the pointer, so the
    // vehicle does not move on screen and there is nothing to suspend.
    //
    // Note there is no position here -- no bus -- and that is the point: follow
    // is a declared intent, not a state that waits for a fix. A rule that keyed
    // off hasPosition() would let a zoom taken while the GPS was still coming
    // up cancel following for good.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.interactive = true;
    config.follow_vehicle = true;
    config.zoom = 12.0;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);

    const map_widget::Coordinate centre = widget.status().camera.center;
    scroll(widget, QPointF(90.0, 70.0), 2);

    check(widget.status().camera.zoom > 12.0, "the wheel still zooms");
    check(sameCoordinate(widget.status().camera.center, centre),
          "about the centre, which does not move");
    check(!widget.status().cameraMoved, "so following is not suspended");

    auto* button = widget.findChild<QAbstractButton*>();
    check(button != nullptr && button->isHidden(),
          "and no recentre button appears, because there is nothing to go back to");
}

void test_the_wheel_stops_at_the_camera_range_the_layout_allows()
{
    // min_zoom/max_zoom bound the CAMERA and nothing else. They are not the
    // archive's range -- that comes from the server on every reply -- so a
    // layout may legitimately let the user zoom past what any archive holds,
    // and refreshTiles() then draws the deepest level there is, magnified.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.interactive = true;
    config.follow_vehicle = false;
    config.min_zoom = 6;
    config.max_zoom = 17;
    config.zoom = 12.0;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);

    scroll(widget, QPointF(200.0, 150.0), 40);
    check(widget.status().camera.zoom == 17.0,
          "the wheel reaches the configured maximum, got " +
              std::to_string(widget.status().camera.zoom));

    // And it still knows what to draw up there. With no server the archive
    // range is unknown, so the request is uncapped rather than blank -- the
    // failure this guards against is a widget that asks for nothing at all
    // once the camera passes some limit.
    check(widget.status().tilesVisible > 0,
          "and still works out tiles to ask for past it, got " +
              std::to_string(widget.status().tilesVisible));

    scroll(widget, QPointF(200.0, 150.0), -80);
    check(widget.status().camera.zoom == 6.0, "and zooming out stops at min_zoom, got " +
                                                  std::to_string(widget.status().camera.zoom));
}

void test_a_drag_cannot_leave_the_projection()
{
    // Dragging north past the pole produces a latitude Web Mercator has no
    // answer for -- the forward transform runs to infinity and the map paints
    // nothing at all -- and dragging west past the date line produces a
    // longitude that would project a whole world away.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.interactive = true;
    config.follow_vehicle = false;
    config.zoom = 1.0;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);

    for (int i = 0; i < 12; ++i)
    {
        drag(widget, QPointF(200.0, 40.0), QPointF(40.0, 260.0));
    }

    const map_widget::Camera camera = widget.status().camera;
    check(camera.center.latitude <= 85.06 && camera.center.latitude >= -85.06,
          "a drag off the top of the world stops at the Mercator limit, got " +
              std::to_string(camera.center.latitude));
    check(camera.center.longitude >= -180.0 && camera.center.longitude < 180.0,
          "and one across the date line wraps rather than running off, got " +
              std::to_string(camera.center.longitude));
    check(widget.status().tilesVisible > 0, "and the map still knows what to draw");
}

void test_a_sized_widget_knows_which_tiles_it_needs()
{
    // The bridge between the projection and the bus. If this is empty, no tile
    // is ever requested and the map is blank for a reason no log line reports.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.zoom = 14.0;

    MapWidget widget(config);
    widget.resize(512, 512);
    render(widget);

    const MapWidget::Status status = widget.status();
    check(status.tilesVisible > 0, "a sized widget works out which tiles it needs");
    check(status.tilesDrawn == 0, "and has none of them, with no server running");
    check(!status.hasPosition, "and no position");

    // Requests were actually issued. Without a server they will time out, but
    // "asked and got nothing" and "never asked" are different bugs and the
    // status has to separate them.
    check(status.tiles.requested > 0, "and it did ask for them");
}

void test_a_zero_sized_widget_asks_for_nothing()
{
    // A widget in a layout that has not laid out yet has zero size. The
    // projection would divide by it; asking for tiles would be asking for the
    // whole world.
    MapConfig_t config;
    config.position_zenoh_key.clear();

    MapWidget widget(config);
    widget.resize(0, 0);

    // No render(): a zero-sized QImage is not constructible, and the paint path
    // guards on size for the same reason. What this pins is that nothing was
    // worked out speculatively at construction -- a widget that fetched tiles
    // for Qt's default 640x480 would have asked for a dozen it will never draw.
    check(widget.status().tilesVisible == 0, "a zero-sized widget needs no tiles");
    check(widget.status().tiles.requested == 0, "and has asked for none");
}

// Let queued invokes run. The tile source hands failures to the GUI thread
// through QMetaObject::invokeMethod, so nothing is folded into the backoff
// until the event loop turns.
void pump(std::chrono::milliseconds duration)
{
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until)
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

void test_a_failed_tile_backs_off_instead_of_being_asked_for_every_frame()
{
    // With no server, every request times out. A failed tile is not cached --
    // there is nothing to draw -- so without a backoff it is re-requested on
    // the very next paint, forever: a permanent queue of queries at fix rate,
    // each waiting out the full timeout.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.zoom = 14.0;
    config.request_timeout_ms = 150;

    MapWidget widget(config);
    widget.resize(512, 512);

    render(widget);
    const std::uint64_t firstRound = widget.status().tiles.requested;
    check(firstRound > 0, "the first paint asks for tiles");

    // Long enough for the requests to time out and for the drain to fold the
    // failures in, but well inside the 500 ms first retry.
    pump(std::chrono::milliseconds(320));

    const MapWidget::Status afterFailure = widget.status();
    check(afterFailure.tiles.failed > 0, "and they fail with no server running");
    check(afterFailure.tiles.backingOff > 0, "which puts them in backoff");

    render(widget);
    check(widget.status().tiles.requested == firstRound,
          "so the next paint asks for NOTHING new while the backoff holds");

    // Past the first retry, it must try again -- a map that gave up for good
    // would stay blank after map_server finally came up.
    pump(std::chrono::milliseconds(500));
    render(widget);
    check(widget.status().tiles.requested > firstRound,
          "and once the backoff expires it asks again");
}

void test_a_large_label_is_not_clipped_by_its_image()
{
    // The cached image is sized from the text's bounding box plus halo
    // padding, and the glyphs are drawn by mapping that box onto (pad, pad).
    // The padding exists so the halo and its antialiasing fade out INSIDE the
    // image; if a placement term is dropped -- the left bearing was, once --
    // the ink drifts toward an edge, and the first symptom is a label that
    // looks shaved on one side. Descenders and an accent probe all four
    // edges, at a size where every error is pixels rather than fractions.
    QFont font;
    font.setPointSizeF(30.0);
    const QString label = QStringLiteral("\u00C1gjy");

    map_widget::LabelCache cache;
    const map_widget::LabelCache::Entry& entry =
        cache.entryFor(label, font, 3.0, QColor("#101216"), QColor("#e8eaed"), 1.0);

    const QImage& image = entry.image;
    check(!image.isNull(), "a large label renders");

    // No ink may touch the outermost row or column on any side: the padding
    // exists so the halo and its antialiasing fade out INSIDE the image.
    bool edgeTouched = false;
    for (int x = 0; x < image.width(); ++x)
    {
        edgeTouched |= qAlpha(image.pixel(x, 0)) != 0;
        edgeTouched |= qAlpha(image.pixel(x, image.height() - 1)) != 0;
    }
    for (int y = 0; y < image.height(); ++y)
    {
        edgeTouched |= qAlpha(image.pixel(0, y)) != 0;
        edgeTouched |= qAlpha(image.pixel(image.width() - 1, y)) != 0;
    }
    check(!edgeTouched, "no ink or halo reaches the image edge at 30 pt");
}

void test_a_cached_label_is_pixel_identical_to_drawing_it_directly()
{
    // The label pass blits a pre-rendered image instead of stroking and
    // filling the glyph outlines every frame -- 0.88 ms per label down to
    // 0.014 ms. That is only a legitimate trade if the pixels are the same,
    // and "the halo looks a bit different" is not something a screenshot
    // review reliably catches.
    QFont font;
    font.setPointSizeF(12.0);
    const QColor halo("#101216");
    const QColor text("#e8eaed");
    constexpr double kHaloWidth = 3.0;
    const QString label = QStringLiteral("Santa Ana");

    const QFontMetricsF metrics(font);
    const QRectF bounds = metrics.boundingRect(label);

    // Somewhere with room for the halo on every side.
    const QPointF at(20.0, 20.0);
    const QSize canvasSize(static_cast<int>(std::ceil(bounds.width())) + 60,
                           static_cast<int>(std::ceil(bounds.height())) + 60);

    // What the code used to do, inline.
    QImage direct(canvasSize, QImage::Format_ARGB32_Premultiplied);
    direct.fill(Qt::transparent);
    {
        QPainter painter(&direct);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        QPainterPath glyphs;
        // `at` is the ink box's top left, exactly as paintLabels() places it:
        // the box the collision pass claims is metrics.boundingRect(), so the
        // ink must land inside that box, not hang below it from the baseline.
        glyphs.addText(at.x() - bounds.x(), at.y() - bounds.y(), font, label);

        QPen pen(halo);
        pen.setWidthF(kHaloWidth);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(glyphs);

        painter.setPen(Qt::NoPen);
        painter.fillPath(glyphs, text);
    }

    // What it does now.
    QImage blitted(canvasSize, QImage::Format_ARGB32_Premultiplied);
    blitted.fill(Qt::transparent);
    {
        map_widget::LabelCache cache;
        QPainter painter(&blitted);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::TextAntialiasing, true);

        const map_widget::LabelCache::Entry& entry =
            cache.entryFor(label, font, kHaloWidth, halo, text, 1.0);
        painter.drawImage(at + entry.offset, entry.image);

        check(cache.size() == 1, "one render is cached");
        // A second ask must not re-render, and must land in the same place.
        const map_widget::LabelCache::Entry& again =
            cache.entryFor(label, font, kHaloWidth, halo, text, 1.0);
        check(&again == &entry, "and the second ask reuses it");
        check(again.bounds == entry.bounds, "with the same bounds for placement");
    }

    check(direct == blitted,
          "a blitted label is pixel-identical to stroking and filling it inline");

    // A style change must not serve stale pixels.
    {
        map_widget::LabelCache cache;
        cache.entryFor(label, font, kHaloWidth, halo, text, 1.0);
        cache.entryFor(label, font, kHaloWidth, halo, QColor("#ff0000"), 1.0);
        check(cache.size() == 1, "changing a colour empties the cache rather than reusing it");
    }
}

} // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    QApplication app(argc, argv);

    test_defaults_point_somewhere_real();
    test_out_of_range_values_are_clamped_not_refused();
    test_an_inverted_zoom_range_is_put_in_order();
    test_bearing_wraps_rather_than_clamping();

    test_a_partial_patch_applies_and_a_bad_field_is_reported();
    test_the_nested_style_is_patchable();
    test_the_doubly_nested_style_groups_are_patchable();
    test_style_groups_are_clamped_not_refused();
    test_the_config_describes_itself();

    test_place_classes_order_labels_by_importance();
    test_a_places_rank_attribute_outranks_its_class_name();
    test_a_large_town_outranks_a_small_city();
    test_population_settles_a_collision_between_two_cities();
    test_a_place_without_rank_falls_back_to_its_class();
    test_a_track_ranks_between_a_town_and_a_city();
    test_a_longer_circuit_outranks_a_shorter_one();
    test_the_label_cache_evicts_rather_than_clearing();
    test_the_label_cache_evicts_the_least_recently_used();

    test_a_road_name_is_placed();
    test_a_road_crossing_several_tiles_is_named_once();
    test_a_stub_too_short_for_its_name_is_not_labelled();
    test_road_labels_respect_their_zoom_floor_and_their_toggle();
    test_a_numbered_route_falls_back_to_its_ref();
    test_a_road_ranks_between_a_neighbourhood_and_a_locality();
    test_a_motorway_name_outranks_a_side_street();

    test_the_widget_constructs_without_a_server();
    test_a_bad_expression_does_not_take_the_widget_down();
    test_the_widget_paints_offscreen();
    test_the_widget_paints_at_its_screens_ratio();

    test_a_map_that_was_not_asked_to_be_interactive_ignores_the_mouse();
    test_a_drag_keeps_the_grabbed_point_under_the_pointer();
    test_the_recentre_button_appears_with_the_pan_and_undoes_it();
    test_the_wheel_zooms_about_the_pointer();
    test_the_wheel_does_not_stop_the_map_following_the_vehicle();
    test_the_wheel_stops_at_the_camera_range_the_layout_allows();
    test_a_drag_cannot_leave_the_projection();
    test_a_sized_widget_knows_which_tiles_it_needs();
    test_a_zero_sized_widget_asks_for_nothing();
    test_a_failed_tile_backs_off_instead_of_being_asked_for_every_frame();
    test_a_large_label_is_not_clipped_by_its_image();
    test_a_cached_label_is_pixel_identical_to_drawing_it_directly();

    spdlog::set_level(spdlog::level::info);

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all map widget checks passed");
    return 0;
}
