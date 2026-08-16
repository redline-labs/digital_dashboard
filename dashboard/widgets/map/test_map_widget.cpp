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

#include <QApplication>
#include <QImage>

#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>

#include <memory>
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
    check(config.max_zoom == 14, "and the default max zoom matches the archive");
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

    test_the_widget_constructs_without_a_server();
    test_a_bad_expression_does_not_take_the_widget_down();
    test_the_widget_paints_offscreen();
    test_a_sized_widget_knows_which_tiles_it_needs();
    test_a_zero_sized_widget_asks_for_nothing();
    test_a_failed_tile_backs_off_instead_of_being_asked_for_every_frame();

    spdlog::set_level(spdlog::level::info);

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all map widget checks passed");
    return 0;
}
