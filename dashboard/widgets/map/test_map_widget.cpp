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
#include <capnp/message.h>

#include "map_tiles.capnp.h"
#include "mvt/encode.h"
#include "pub_sub/zenoh_service.h"

#include "map/highlight_ids.h"
#include "road_graph/format.h"
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
    check(fields.contains("highlight_zenoh_key") && fields.contains("highlight_color") &&
              fields.contains("highlight_extra_width"),
          "the matched-road highlight is configurable");
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
// The atlas packs each character once, per halo-or-fill, and hands back the
// same placement thereafter. A page that grew every frame would be re-uploaded
// every frame, which is the one cost this whole design exists to avoid.
void test_the_atlas_packs_each_glyph_once()
{
    map_widget::LabelCache cache;
    const QFont font("Arial", 12);
    const QColor halo(Qt::black);
    const QColor text(Qt::white);

    const QRectF first = cache.atlasEntry(QChar('M'), false, font, 3.0, halo, text, 1.0);
    check(!first.isNull(), "a character is packed on first sight");
    check(cache.atlas().dirty(), "and the page is marked for upload");

    cache.atlas().markClean();
    const QRectF again = cache.atlasEntry(QChar('M'), false, font, 3.0, halo, text, 1.0);
    check(again == first, "the second ask returns the same placement");
    check(!cache.atlas().dirty(), "and does not dirty the page");

    // Halo and fill are different shapes -- the halo is the stroked outline
    // and is wider -- so they are separate entries, not one reused twice.
    const QRectF haloRect = cache.atlasEntry(QChar('M'), true, font, 3.0, halo, text, 1.0);
    check(!haloRect.isNull() && haloRect != first,
          "a character's halo is packed separately from its fill");

    // A whole name interns its own alphabet and nothing more: asking for the
    // same name again packs nothing, however many characters it repeats.
    for (const QChar ch : QString("Main Street"))
    {
        cache.atlasEntry(ch, false, font, 3.0, halo, text, 1.0);
    }
    const std::size_t afterName = cache.atlas().glyphs();
    check(afterName < 11, "a name shorter than its own length is packed, got " +
                              std::to_string(afterName));

    cache.atlas().markClean();
    for (const QChar ch : QString("Main Street"))
    {
        cache.atlasEntry(ch, false, font, 3.0, halo, text, 1.0);
    }
    check(cache.atlas().glyphs() == afterName, "the same name a second time packs nothing new");
    check(!cache.atlas().dirty(), "and does not dirty the page");

    // A name sharing letters adds only what it introduces.
    for (const QChar ch : QString("Main Avenue"))
    {
        cache.atlasEntry(ch, false, font, 3.0, halo, text, 1.0);
    }
    check(cache.atlas().glyphs() > afterName, "a new name with new letters adds them");
    check(cache.atlas().glyphs() <= afterName + 4,
          "but reuses the ones it shares, got " + std::to_string(cache.atlas().glyphs()) +
              " against " + std::to_string(afterName));
}

// What is IN the atlas has to be what QPainter drew, pixel for pixel. If the
// packing were off by a row, or the uv rect described the wrong region, the
// GPU would draw exactly that and no screenshot would say so.
void test_a_packed_glyph_is_pixel_identical_to_the_one_rasterised()
{
    map_widget::LabelCache cache;
    const QFont font("Arial", 16);
    const QColor halo(Qt::black);
    const QColor text(Qt::white);

    const QRectF uv = cache.atlasEntry(QChar('A'), false, font, 3.0, halo, text, 1.0);
    check(!uv.isNull(), "the character packed");

    const map_widget::LabelCache::Glyph& glyph =
        cache.glyphFor(QChar('A'), font, 3.0, halo, text, 1.0);
    const QImage& page = cache.atlas().page();
    const QRect region(int(std::lround(uv.x() * page.width())),
                       int(std::lround(uv.y() * page.height())),
                       glyph.fill.width(), glyph.fill.height());
    const QImage packed = page.copy(region);

    check(packed.size() == glyph.fill.size(), "the region is the glyph's size");
    // Converted to one format before comparing: the page is premultiplied and
    // the glyph image carries a device pixel ratio, and QImage::operator==
    // takes both into account.
    check(packed.convertToFormat(QImage::Format_ARGB32) ==
              glyph.fill.convertToFormat(QImage::Format_ARGB32),
          "and holds exactly the pixels QPainter rasterised");
}

// A style change has to empty the atlas with the glyph images it holds. Left
// alone it would draw the new frame's text out of the old style's page, which
// is precisely the "the style change did not apply" symptom the cache key
// exists to prevent.
void test_a_style_change_empties_the_atlas()
{
    map_widget::LabelCache cache;
    const QFont font("Arial", 12);

    cache.atlasEntry(QChar('M'), false, font, 3.0, QColor(Qt::black), QColor(Qt::white), 1.0);
    check(cache.atlas().glyphs() == 1, "one character packed");

    cache.atlasEntry(QChar('M'), false, font, 3.0, QColor(Qt::black), QColor(Qt::red), 1.0);
    check(cache.atlas().glyphs() == 1,
          "a colour change re-packs rather than reusing, got " +
              std::to_string(cache.atlas().glyphs()));
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

// A `transportation_name` line following whatever shape the caller gives it.
// Every road fixture above this one is a straight two-point line, which is
// exactly the geometry a curved-text placement cannot be tested on.
mvt::Tile roadShapeTile(const std::string& name, const std::string& roadClass,
                        const std::vector<mvt::Point>& shape)
{
    mvt::Layer layer;
    layer.name = "transportation_name";
    layer.extent = 4096;
    layer.keys = { "name:latin", "class" };
    layer.values = { mvt::Value(std::in_place_type<std::string>, name),
                     mvt::Value(std::in_place_type<std::string>, roadClass) };

    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.rings.push_back(shape);
    feature.tags = { 0, 0, 1, 1 };
    layer.features.push_back(std::move(feature));

    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));
    return tile;
}

// What the decode worker does to every arriving tile: candidates out,
// decoded features gone. The label tests go through it so they exercise the
// extraction the widget actually runs.
std::shared_ptr<const map_widget::LabelSet> labelsOf(const mvt::Tile& tile)
{
    return std::make_shared<const map_widget::LabelSet>(map_widget::extractLabels(tile));
}

// What a label pass actually put on the canvas, not just how many labels it
// claims to have placed. The stats say "one label"; the pixels say WHERE, and
// the anchor rotation is only observable in the pixels.
struct Painted
{
    map_widget::LabelStats stats;
    QImage canvas;
};

// Lay the frame's text out and draw the quads it produced, in software.
//
// The widget hands these quads to the GPU; here they are rasterised with
// QPainter instead, which is what lets a test say "the name landed HERE, this
// way up" without a GPU in the loop. It is also an independent check on the
// geometry: if a quad's corners or its atlas rect were wrong, the GPU would
// draw exactly the same wrong thing and no screenshot would say so.
Painted paintOnto(const std::vector<map_widget::LabelTile>& tiles, const MapStyle_t& style,
                  double zoom = 14.0, double bearing = 0.0)
{
    Painted out;
    out.canvas = QImage(800, 600, QImage::Format_ARGB32_Premultiplied);
    out.canvas.fill(Qt::transparent);

    const map_widget::Camera camera { { 33.6865966, -117.8557874 }, zoom, bearing };
    const map_widget::Projection projection(camera, 800, 600, 1.0);

    map_widget::LabelCache cache;
    std::vector<map_widget::TextQuad> quads;
    out.stats = map_widget::layOutText(projection, tiles, style, cache, 1.0, quads);

    const QImage& page = cache.atlas().page();
    if (page.isNull())
    {
        return out;
    }

    QPainter painter(&out.canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    for (const map_widget::TextQuad& quad : quads)
    {
        // The atlas rect is in texture coordinates; the page is what they are
        // a fraction of.
        const QRectF src(quad.uv.x() * page.width(), quad.uv.y() * page.height(),
                         quad.uv.width() * page.width(), quad.uv.height() * page.height());
        QPolygonF from;
        from << src.topLeft() << src.topRight() << src.bottomRight() << src.bottomLeft();
        QPolygonF to;
        to << quad.corners[0] << quad.corners[1] << quad.corners[2] << quad.corners[3];

        QTransform place;
        if (!QTransform::quadToQuad(from, to, place))
        {
            continue;
        }
        painter.save();
        painter.setTransform(place);
        painter.drawImage(src.topLeft(), page, src);
        painter.restore();
    }
    return out;
}

map_widget::LabelStats placeOnto(const std::vector<map_widget::LabelTile>& tiles,
                                 const MapStyle_t& style, double zoom = 14.0)
{
    return paintOnto(tiles, style, zoom).stats;
}

// The bounding box of everything drawn. Null if the canvas is empty, which is
// itself the assertion in a "nothing was placed" test.
QRectF inkBounds(const QImage& canvas)
{
    int minX = canvas.width();
    int minY = canvas.height();
    int maxX = -1;
    int maxY = -1;
    for (int y = 0; y < canvas.height(); ++y)
    {
        for (int x = 0; x < canvas.width(); ++x)
        {
            if (qAlpha(canvas.pixel(x, y)) == 0)
            {
                continue;
            }
            minX = std::min(minX, x);
            minY = std::min(minY, y);
            maxX = std::max(maxX, x);
            maxY = std::max(maxY, y);
        }
    }
    if (maxX < 0)
    {
        return {};
    }
    return QRectF(minX, minY, (maxX - minX) + 1, (maxY - minY) + 1);
}

// The headline gap: `transportation_name` is built, shipped, decoded and was
// then ignored, so a nav map had no street names at all.
void test_a_road_name_is_placed()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto tile = labelsOf(roadNameTile("Main Street", "minor"));

    MapStyle_t style;
    // Repeats off: this test is about whether a street name is drawn at all,
    // and a road long enough to carry its name twice would answer with a
    // count that says nothing about either. Repeats have their own tests.
    style.label_repeat_distance = 0;
    const auto stats = placeOnto({ { irvine, tile } }, style);
    check(stats.placed == 1, "the street name is drawn, got " + std::to_string(stats.placed));
}

// The anchor rotation at the heart of the label pass -- tile-local axes turn
// with the map even though the text does not -- had NO test at all: every
// label test ran at bearing 0.0, where the rotation terms are 1 and 0 and a
// sign error is invisible. These two pin it before anything moves it.
//
// A road along the tile's own X axis, labelled at the middle of the tile. At
// bearing 0 the tile's axes are the screen's, so the label sits at the tile
// centre. Turning the map moves that centre to a place trigonometry can
// predict, and nothing else about the label changes.
void test_the_label_anchor_turns_with_the_map()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto tile = labelsOf(roadNameTile("Main Street", "minor"));

    MapStyle_t style;
    // Repeats off: this test is about where a single label lands when the map turns,
    // and a road long enough to carry its name twice would answer with a
    // count that says nothing about either. Repeats have their own tests.
    style.label_repeat_distance = 0;
    // The halo would put ink outside the text box and blur the comparison.
    style.label_halo_width = 0.0;

    const Painted north = paintOnto({ { irvine, tile } }, style, 14.0, 0.0);
    check(north.stats.placed == 1, "the north-up label is placed");
    const QRectF at0 = inkBounds(north.canvas);
    check(!at0.isNull(), "and it left ink on the canvas");

    // The SAME camera, turned a quarter turn. The label must move to where the
    // rotated tile puts its centre -- and must still be drawn upright, which
    // is the invariant the whole pass rests on: an upright "Main Street" is
    // far wider than it is tall at every bearing.
    const Painted east = paintOnto({ { irvine, tile } }, style, 14.0, 90.0);
    check(east.stats.placed == 1, "the turned label is still placed");
    const QRectF at90 = inkBounds(east.canvas);
    check(!at90.isNull(), "and it too left ink");

    // A road label FOLLOWS ITS ROAD, which is the whole point of it. The road
    // runs along the tile's X axis, so a quarter turn of the map stands it up
    // on screen -- and the name has to stand up with it. At bearing 0 the same
    // name is drawn along a horizontal road and is wider than it is tall.
    check(at0.width() > at0.height(), "the name lies along an east-west road");
    check(at90.height() > at90.width(),
          "and stands up with the road when the map turns a quarter, got " +
              std::to_string(at90.width()) + "x" + std::to_string(at90.height()));

    // Turned, not reflowed: the ink that was wide is now tall by about as
    // much. A label that had been re-laid out horizontally would keep its old
    // proportions instead.
    check(std::abs(at90.height() - at0.width()) <= 4.0 &&
              std::abs(at90.width() - at0.height()) <= 4.0,
          "and it is the same name turned, not a differently shaped one");

    // Where "somewhere else" is, computed independently of the label pass.
    // The tile's centre is a fixed world point, so the projection alone says
    // where it lands -- if the label pass and the projection ever disagree
    // about which way the map turns, this is what catches it.
    const map_widget::Camera camera { { 33.6865966, -117.8557874 }, 14.0, 90.0 };
    const map_widget::Projection projection(camera, 800, 600, 1.0);
    const map_widget::ScreenPoint origin = projection.tileOrigin(irvine);
    const double size = projection.tileScreenSize(irvine.z);
    // The road runs the width of the tile at y=2048 of 4096, so its longest
    // run's midpoint is the tile's centre in tile-local units.
    const double lx = 0.5 * size;
    const double ly = 0.5 * size;
    const QPointF expected(origin.x + ((lx * projection.bearingCos()) -
                                       (ly * projection.bearingSin())),
                           origin.y + ((lx * projection.bearingSin()) +
                                       (ly * projection.bearingCos())));

    check(std::abs(at90.center().x() - expected.x()) <= 2.0 &&
              std::abs(at90.center().y() - expected.y()) <= 2.0,
          "the turned label sits on the turned anchor, expected (" +
              std::to_string(expected.x()) + ", " + std::to_string(expected.y()) + ") got (" +
              std::to_string(at90.center().x()) + ", " + std::to_string(at90.center().y()) + ")");
}

// Half a turn is the case a sign error survives: it moves the anchor to the
// point reflected through the tile origin, which a symmetric test cannot tell
// from the right answer. Checked against the projection directly for that
// reason, and at a NON-central anchor so the two are actually different.
void test_the_label_anchor_is_right_at_half_a_turn()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    // A short road up in the tile's first quarter: the anchor is nowhere near
    // the tile centre, so reflecting it lands somewhere clearly wrong.
    const auto tile = labelsOf(roadNameTile("Main Street", "minor", 1024, 512, 3072));

    MapStyle_t style;
    style.label_halo_width = 0.0;
    // Repeats off: this test is about where a single label lands when the map turns,
    // and a road long enough to carry its name twice would answer with a
    // count that says nothing about either. Repeats have their own tests.
    style.label_repeat_distance = 0;

    const Painted turned = paintOnto({ { irvine, tile } }, style, 14.0, 180.0);
    check(turned.stats.placed == 1, "the label survives half a turn");
    const QRectF ink = inkBounds(turned.canvas);
    check(!ink.isNull(), "and left ink");
    // Half a turn puts the road back along the screen's X axis, pointing the
    // other way -- which is the case the upright flip exists for. Wider than
    // tall means it was laid along the road; that it is READABLE rather than
    // mirrored is test_a_road_label_never_reads_backwards' business.
    check(ink.width() > ink.height(), "and lies along the road, which is horizontal again");

    const map_widget::Camera camera { { 33.6865966, -117.8557874 }, 14.0, 180.0 };
    const map_widget::Projection projection(camera, 800, 600, 1.0);
    const map_widget::ScreenPoint origin = projection.tileOrigin(irvine);
    const double size = projection.tileScreenSize(irvine.z);
    const double lx = ((512.0 + 3072.0) / 2.0 / 4096.0) * size;
    const double ly = (1024.0 / 4096.0) * size;
    const QPointF expected(origin.x + ((lx * projection.bearingCos()) -
                                       (ly * projection.bearingSin())),
                           origin.y + ((lx * projection.bearingSin()) +
                                       (ly * projection.bearingCos())));

    check(std::abs(ink.center().x() - expected.x()) <= 2.0 &&
              std::abs(ink.center().y() - expected.y()) <= 2.0,
          "the half-turned label sits on the half-turned anchor, expected (" +
              std::to_string(expected.x()) + ", " + std::to_string(expected.y()) + ") got (" +
              std::to_string(ink.center().x()) + ", " + std::to_string(ink.center().y()) + ")");
}

// A place name is NOT a road name. It has no line to follow, so it stays
// upright at every bearing -- which is the invariant the whole label pass was
// built on, and the one thing curved road labels must not have cost.
void test_a_place_name_stays_upright_when_the_map_turns()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    mvt::Tile tile;
    tile.layers.push_back(labelLayerWith(
        "place", { { "name:latin", mvt::Value(std::in_place_type<std::string>, "Irvine") },
                   { "rank", mvt::Value(std::int64_t { 2 }) } }));
    const auto labels = labelsOf(tile);

    MapStyle_t style;
    style.label_halo_width = 0.0;

    for (const double bearing : { 0.0, 45.0, 90.0, 180.0, 270.0 })
    {
        const Painted painted = paintOnto({ { irvine, labels } }, style, 14.0, bearing);
        check(painted.stats.placed == 1,
              "the town is named at bearing " + std::to_string(bearing));
        const QRectF ink = inkBounds(painted.canvas);
        check(ink.width() > ink.height(),
              "and stays upright -- wider than tall -- at bearing " + std::to_string(bearing));
    }
}

// The upright rule, which is what keeps a road label from reading backwards.
//
// The same road, described in both directions. A renderer that simply laid the
// glyphs out in the order the geometry happens to run would draw one of these
// mirrored; the flip is what makes the two identical.
void test_a_road_label_never_reads_backwards()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };

    MapStyle_t style;
    style.label_halo_width = 0.0;
    // Repeats off: this test is about which way the letters run,
    // and a road long enough to carry its name twice would answer with a
    // count that says nothing about either. Repeats have their own tests.
    style.label_repeat_distance = 0;

    const auto westToEast = labelsOf(roadNameTile("Main Street", "minor", 2048, 256, 3840));
    const auto eastToWest = labelsOf(roadShapeTile("Main Street", "minor",
                                                   { { 3840, 2048 }, { 256, 2048 } }));

    const Painted forward = paintOnto({ { irvine, westToEast } }, style);
    const Painted backward = paintOnto({ { irvine, eastToWest } }, style);

    check(forward.stats.placed == 1, "the westward road is named");
    check(backward.stats.placed == 1, "and so is the same road described eastward");

    // Pixel for pixel. The road is the same road and the name is the same
    // name, so which end the archive happened to start the way at must not be
    // visible in the output at all.
    check(forward.canvas == backward.canvas,
          "a road described backwards is labelled identically -- the text is not mirrored");
}

// A road that doubles back on itself has nowhere to put a name that a reader
// could follow. MapLibre rejects these on a sliding window of turn; so do we,
// and this is the case that separates "rejects a kink" from "rejects
// everything bent".
void test_a_hairpin_gets_no_label_but_a_gentle_curve_does()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };

    MapStyle_t style;
    style.label_halo_width = 0.0;
    // Repeats off: this test is about the curvature test,
    // and a road long enough to carry its name twice would answer with a
    // count that says nothing about either. Repeats have their own tests.
    style.label_repeat_distance = 0;

    // Out and straight back, a couple of degrees off doubling over itself.
    // Every character would sit on top of another one.
    const auto hairpin = labelsOf(roadShapeTile(
        "Mulholland Drive", "minor",
        { { 512, 2048 }, { 3584, 2048 }, { 3584, 2148 }, { 512, 2148 }, { 512, 2248 } }));
    const auto hairpinStats = placeOnto({ { irvine, hairpin } }, style);
    check(hairpinStats.placed == 0,
          "a hairpin is not labelled, got " + std::to_string(hairpinStats.placed));
    check(hairpinStats.suppressed >= 1, "and says so as a suppression rather than a silent drop");

    // A long shallow arc across the tile -- exactly what this feature is FOR.
    // Same name, same length of road, and it must survive.
    std::vector<mvt::Point> arc;
    for (int i = 0; i <= 16; ++i)
    {
        const double t = double(i) / 16.0;
        arc.push_back(mvt::Point { std::int32_t(512 + (t * 3072)),
                                   std::int32_t(2048 - (std::sin(t * 3.14159) * 220.0)) });
    }
    const auto curve = labelsOf(roadShapeTile("Mulholland Drive", "minor", arc));
    const auto curveStats = placeOnto({ { irvine, curve } }, style);
    check(curveStats.placed == 1,
          "but a gentle curve is labelled, got " + std::to_string(curveStats.placed));
}

// The name is drawn ALONG the road, which is the whole feature. On a diagonal
// road the ink has to be diagonal too: a horizontal label on a 45-degree road
// would be far wider than tall, and a followed one is roughly square.
void test_a_road_label_lies_along_a_diagonal_road()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };

    MapStyle_t style;
    style.label_halo_width = 0.0;
    // Repeats off: this test is about the angle a name is drawn at,
    // and a road long enough to carry its name twice would answer with a
    // count that says nothing about either. Repeats have their own tests.
    style.label_repeat_distance = 0;

    const auto flat = labelsOf(roadNameTile("Main Street", "minor", 2048, 256, 3840));
    const auto diagonal = labelsOf(roadShapeTile("Main Street", "minor",
                                                 { { 256, 512 }, { 3840, 3584 } }));

    const QRectF flatInk = inkBounds(paintOnto({ { irvine, flat } }, style).canvas);
    const Painted painted = paintOnto({ { irvine, diagonal } }, style);
    check(painted.stats.placed == 1, "the diagonal road is named");
    const QRectF ink = inkBounds(painted.canvas);

    check(!flatInk.isNull() && !ink.isNull(), "both left ink");
    check(ink.height() > flatInk.height() * 2.0,
          "the name climbs with the road rather than lying flat across it, got height " +
              std::to_string(ink.height()) + " against " + std::to_string(flatInk.height()));
    check(ink.width() < flatInk.width(),
          "and takes less width than the same name drawn horizontally");
}

// The glyph tier is what makes curved text affordable: a label is a row of
// individually placed characters, and rendering each one every frame would
// cost what the string cache was built to avoid.
void test_the_glyph_tier_renders_an_alphabet_not_a_name_list()
{
    map_widget::LabelCache cache;
    QFont font("Arial", 12);

    const QColor halo(Qt::black);
    const QColor text(Qt::white);

    for (const QChar ch : QString("Main Street"))
    {
        cache.glyphFor(ch, font, 3.0, halo, text, 1.0);
    }
    // "Main Street" spells nine distinct characters: M a i n space S t r e.
    const std::size_t afterOne = cache.glyphCount();
    check(afterOne == 9, "one name interns its own alphabet, got " + std::to_string(afterOne));

    // A second name sharing letters adds only what it introduces.
    for (const QChar ch : QString("Main Avenue"))
    {
        cache.glyphFor(ch, font, 3.0, halo, text, 1.0);
    }
    check(cache.glyphCount() > afterOne, "a new name with new letters adds them");
    check(cache.glyphCount() <= 14,
          "but reuses the ones it shares, got " + std::to_string(cache.glyphCount()));

    // A style change has to empty this tier as well as the string tier, or
    // half the alphabet stays in the old colour.
    cache.glyphFor(QChar('M'), font, 3.0, halo, QColor(Qt::red), 1.0);
    check(cache.glyphCount() == 1, "a colour change re-keys the glyph tier too, got " +
                                       std::to_string(cache.glyphCount()));
}

// A road crosses every tile it passes through and each tile carries the WHOLE
// name, so without dedup one street is labelled once per tile on screen. With
// repeats it should be labelled several times -- but spread out, never twice
// in the same place, which is the whole difference between repeating a name
// and failing to dedup it.
void test_a_road_repeats_its_name_along_itself_but_never_twice_in_one_place()
{
    const auto tile = labelsOf(roadNameTile("Main Street", "minor"));

    std::vector<map_widget::LabelTile> tiles;
    for (std::uint32_t x = 2827; x <= 2829; ++x)
    {
        for (std::uint32_t y = 6561; y <= 6563; ++y)
        {
            tiles.push_back({ map_widget::TileId { 14, x, y }, tile });
        }
    }

    // Repeats off is the old rule, and it still has to hold: this is what
    // stops a street being named once per tile it happens to cross.
    MapStyle_t once;
    once.label_repeat_distance = 0;
    const auto onceStats = placeOnto(tiles, once);
    check(onceStats.placed == 1,
          "with repeats off, nine tiles of one road produce one label, got " +
              std::to_string(onceStats.placed));

    // On by default, so the name is readable wherever the driver is looking.
    MapStyle_t style;
    style.label_halo_width = 0.0;
    style.label_repeat_distance = 250;
    const Painted painted = paintOnto(tiles, style);
    check(painted.stats.placed > 1,
          "with repeats on, the road carries its name more than once, got " +
              std::to_string(painted.stats.placed));

    // Every instance at least the repeat distance from the last. Read off the
    // canvas rather than trusted: the three roads run across three rows of
    // tiles, so the labels form horizontal bands, and two labels closer
    // together than this would merge into one band of ink.
    const QRectF ink = inkBounds(painted.canvas);
    check(!ink.isNull(), "and left ink");
    check(ink.width() > 250.0,
          "spread along the road rather than stacked at one point, got width " +
              std::to_string(ink.width()));

    // Turning the distance up thins them out again -- the knob does what it
    // says, which a count alone would not show.
    MapStyle_t sparse;
    sparse.label_repeat_distance = 4096;
    const auto sparseStats = placeOnto(tiles, sparse);
    check(sparseStats.placed <= painted.stats.placed,
          "a longer repeat distance never produces more labels, got " +
              std::to_string(sparseStats.placed) + " against " +
              std::to_string(painted.stats.placed));
}

// ============================================================================
// Water names
// ============================================================================

// `water_name` carries BOTH shapes, which no other label layer does: a lake's
// name sits at a point inside it, a river's runs along the line. map_build
// emits them that way on purpose (extract.cpp), and the extractor has to
// follow the feature rather than the layer.
mvt::Tile waterNameTile(const std::string& name, const std::string& waterClass, bool asLine)
{
    mvt::Layer layer;
    layer.name = "water_name";
    layer.extent = 4096;
    layer.keys = { "name:latin", "class" };
    layer.values = { mvt::Value(std::in_place_type<std::string>, name),
                     mvt::Value(std::in_place_type<std::string>, waterClass) };

    mvt::Feature feature;
    feature.type = asLine ? mvt::GeomType::LineString : mvt::GeomType::Point;
    if (asLine)
    {
        feature.rings.push_back({ { 256, 2048 }, { 3840, 2048 } });
    }
    else
    {
        feature.rings.push_back({ { 2048, 2048 } });
    }
    feature.tags = { 0, 0, 1, 1 };
    layer.features.push_back(std::move(feature));

    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));
    return tile;
}

void test_a_river_is_named_along_itself_and_a_lake_at_a_point()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };

    MapStyle_t style;
    style.label_halo_width = 0.0;
    style.label_repeat_distance = 0;

    // The river runs east-west across the tile, so its name lies flat -- and
    // the lake's name is drawn upright at its point. At bearing 0 both come
    // out horizontal, which is why the interesting test is the turned one
    // below.
    const auto river = labelsOf(waterNameTile("Santa Ana River", "river", true));
    const auto riverStats = placeOnto({ { irvine, river } }, style);
    check(riverStats.placed == 1,
          "the river is named, got " + std::to_string(riverStats.placed));

    const auto lake = labelsOf(waterNameTile("Irvine Lake", "lake", false));
    const auto lakeStats = placeOnto({ { irvine, lake } }, style);
    check(lakeStats.placed == 1, "the lake is named, got " + std::to_string(lakeStats.placed));

    // Turn the map. The river's name must turn with the river; the lake's must
    // not turn at all, because a lake has no line to follow.
    const Painted turnedRiver = paintOnto({ { irvine, river } }, style, 14.0, 90.0);
    const QRectF riverInk = inkBounds(turnedRiver.canvas);
    check(!riverInk.isNull() && riverInk.height() > riverInk.width(),
          "a turned river stands its name up with itself");

    const Painted turnedLake = paintOnto({ { irvine, lake } }, style, 14.0, 90.0);
    const QRectF lakeInk = inkBounds(turnedLake.canvas);
    check(!lakeInk.isNull() && lakeInk.width() > lakeInk.height(),
          "but a lake's name stays upright, because a point has no direction");
}

// map_build deliberately emits NAMELESS river segments, because the layer
// carries the geometry a label runs along and the name may live on a parent
// way that was split. Those must not become blank labels.
void test_a_nameless_river_segment_claims_no_label()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto nameless = labelsOf(waterNameTile("", "river", true));
    check(nameless->labels.empty(), "a nameless river yields no candidate at all");

    MapStyle_t style;
    const auto stats = placeOnto({ { irvine, nameless } }, style);
    check(stats.placed == 0, "and nothing is drawn for it");
}

// A street name must survive a river crossing it. This is the ranking, and it
// is the reason water sits below roads rather than beside them.
void test_a_street_name_outranks_the_river_it_crosses()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };

    // Both through the middle of the tile, so they compete for the same
    // pixels and exactly one can win.
    const auto road = labelsOf(roadNameTile("Main Street", "minor", 2048, 256, 3840));
    const auto river = labelsOf(waterNameTile("Santa Ana River", "river", true));

    MapStyle_t style;
    style.label_repeat_distance = 0;
    const auto stats = placeOnto({ { irvine, road }, { irvine, river } }, style);
    check(stats.placed == 1, "only one of the two fits, got " + std::to_string(stats.placed));
    check(stats.suppressed >= 1, "and the loser is counted as suppressed");

    // Which one won is the whole point, so it is read off the pixels.
    MapStyle_t bare;
    bare.label_halo_width = 0.0;
    bare.label_repeat_distance = 0;
    const QRectF both = inkBounds(paintOnto({ { irvine, road }, { irvine, river } }, bare).canvas);
    const QRectF roadOnly = inkBounds(paintOnto({ { irvine, road } }, bare).canvas);
    check(std::abs(both.width() - roadOnly.width()) <= 1.0,
          "and it is the street, not the river -- the ink matches the road drawn alone");
}

// Water labels answer to their own toggle and their own zoom floor, exactly as
// road labels do.
void test_water_labels_respect_their_zoom_floor_and_their_toggle()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto river = labelsOf(waterNameTile("Santa Ana River", "river", true));

    MapStyle_t style;
    style.label_repeat_distance = 0;
    check(placeOnto({ { irvine, river } }, style).placed == 1, "drawn at the floor");

    MapStyle_t below = style;
    below.detail.water_label = 15;
    check(placeOnto({ { irvine, river } }, below).placed == 0, "and not below it");

    MapStyle_t off = style;
    off.show_water_labels = false;
    check(placeOnto({ { irvine, river } }, off).placed == 0, "nor with the toggle off");

    // And the road toggle must not reach it -- two layers, two switches.
    MapStyle_t roadsOff = style;
    roadsOff.show_road_labels = false;
    check(placeOnto({ { irvine, river } }, roadsOff).placed == 1,
          "turning street names off leaves the river named");
}

// A river beats a stream where they collide, the way a motorway beats a lane.
void test_a_river_outranks_a_stream()
{
    const mvt::Layer layer = waterNameTile("X", "river", true).layers.front();
    const map_widget::LabelRank river = map_widget::waterRank(layer, layer.features.front());

    const mvt::Layer streamLayer = waterNameTile("X", "stream", true).layers.front();
    const map_widget::LabelRank stream =
        map_widget::waterRank(streamLayer, streamLayer.features.front());

    check(river.tier == stream.tier, "both are water, so both sit in the same tier");
    check(river.magnitude > stream.magnitude, "but the river carries more weight");

    // Below a road, which is the ranking decision worth pinning.
    const mvt::Layer roadLayer = roadNameTile("Y", "minor").layers.front();
    const map_widget::LabelRank road = map_widget::roadRank(roadLayer, roadLayer.features.front());
    check(river.tier < road.tier, "and water ranks below any road");
}

// A name wider than the road it names reads as text floating over the map.
// MVT leaves two-metre stubs in tile corners wherever it clips a road, and
// without this each of them claims a label.
void test_a_stub_too_short_for_its_name_is_not_labelled()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto stub =
        labelsOf(roadNameTile("A Very Long Street Name Indeed", "minor", 2048, 2040, 2056));

    const MapStyle_t style;
    const auto stats = placeOnto({ { irvine, stub } }, style);
    check(stats.placed == 0, "a 16-unit stub does not get a 29-character name");
}

// Street names are z14 information: below that the roads themselves are a grey
// smear and a name labels something the driver cannot see.
void test_road_labels_respect_their_zoom_floor_and_their_toggle()
{
    const map_widget::TileId irvine { 14, 2828, 6562 };
    const auto tile = labelsOf(roadNameTile("Main Street", "minor"));

    MapStyle_t style;
    // Repeats off: this test is about the zoom floor and the toggle,
    // and a road long enough to carry its name twice would answer with a
    // count that says nothing about either. Repeats have their own tests.
    style.label_repeat_distance = 0;
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

    mvt::Tile tile;
    tile.layers.push_back(std::move(layer));

    MapStyle_t style;
    // Repeats off: this test is about which text a road is labelled with,
    // and a road long enough to carry its name twice would answer with a
    // count that says nothing about either. Repeats have their own tests.
    style.label_repeat_distance = 0;
    const auto stats =
        placeOnto({ { map_widget::TileId { 14, 2828, 6562 }, labelsOf(tile) } }, style);
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

// Drive a camera ease to completion. Eases advance in paintEvent, and a
// hidden test widget gets no paints on its own -- so the test paints, the
// way the ticker would in a shown one.
void settleCamera(MapWidget& widget)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    render(widget);
    while (widget.status().animating && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        render(widget);
    }
}

void scroll(MapWidget& widget, const QPointF& at, int notches)
{
    // angleDelta only, with a null pixelDelta: that is what a wheel with
    // detents sends, and it is the branch a mouse takes.
    QWheelEvent event(at, at, QPoint(0, 0), QPoint(0, notches * 120), Qt::NoButton,
                      Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&widget, &event);
}

void swipe(MapWidget& widget, const QPointF& at, int pixels)
{
    // pixelDelta set: that is what a trackpad sends, and it is the branch a
    // two-finger gesture takes. angleDelta is filled in too, because a real
    // trackpad reports both -- which is exactly why the widget must prefer
    // the pixels.
    QWheelEvent event(at, at, QPoint(0, pixels), QPoint(0, pixels * 2), Qt::NoButton,
                      Qt::NoModifier, Qt::ScrollUpdate, false);
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
    // The recentre is a fly-back now, not a teleport; let it land.
    settleCamera(widget);

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

    // Mid-ease: paint once, some of the way in. The anchor property must hold
    // DURING the glide, not just at its ends -- a zoom that wanders off the
    // pointer and comes back is worse than one that snaps.
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    render(widget);
    const double midZoom = widget.status().camera.zoom;
    check(midZoom > 12.0, "the wheel eases the zoom upward, got " + std::to_string(midZoom));
    check(widget.status().animating, "and is still easing after one frame");
    const map_widget::Coordinate midway =
        projectionOf(widget).coordinateForScreen(map_widget::ScreenPoint { at.x(), at.y() });
    check(sameCoordinate(before, midway), "the pointer keeps its place mid-ease");

    settleCamera(widget);
    check(widget.status().camera.zoom == 13.0, "and lands exactly on the target, got " +
                                                   std::to_string(widget.status().camera.zoom));
    check(!widget.status().animating, "with the ease finished");

    const map_widget::Coordinate after =
        projectionOf(widget).coordinateForScreen(map_widget::ScreenPoint { at.x(), at.y() });
    check(sameCoordinate(before, after),
          "and the place under the pointer stays under the pointer while the scale changes");
}

void test_a_trackpad_swipe_zooms_by_what_the_fingers_asked_for()
{
    // The bug this pins: a trackpad's pixelDelta arrives as a stream every few
    // milliseconds, and each event used to restart the shared 140 ms ease from
    // wherever the last one had reached. easeSmooth() is flat at t=0, so an
    // ease restarted 8 ms in never travelled more than about 1% of its span
    // and a long swipe moved the zoom by a fraction of a level. Dragging felt
    // fluid because it eases nothing; only the zoom was slow.
    //
    // So: send a stream the way a trackpad does, with no time to ease between
    // events, and require that the whole gesture arrives.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.interactive = true;
    config.follow_vehicle = false;
    config.zoom = 12.0;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);

    const QPointF at(90.0, 70.0);
    const map_widget::Coordinate before =
        projectionOf(widget).coordinateForScreen(map_widget::ScreenPoint { at.x(), at.y() });

    // 240 px of finger travel, delivered 20 px at a time back to back. At
    // 120 px per notch and half a level per notch that is one whole level.
    for (int i = 0; i < 12; ++i)
    {
        swipe(widget, at, 20);
        render(widget);
    }
    settleCamera(widget);

    const double zoom = widget.status().camera.zoom;
    check(std::abs(zoom - 13.0) < 1e-9,
          "240 px of two-finger swipe zooms one whole level, got " + std::to_string(zoom));

    const map_widget::Coordinate after =
        projectionOf(widget).coordinateForScreen(map_widget::ScreenPoint { at.x(), at.y() });
    check(sameCoordinate(before, after),
          "and the place under the fingers stays under the fingers");
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

    // At EVERY step of the ease, not just at the end: the centre must never
    // be written while following, or a single wheel notch quietly suspends it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    do
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        render(widget);
        check(sameCoordinate(widget.status().camera.center, centre),
              "the centre never moves during the ease");
        check(!widget.status().cameraMoved, "and following is never suspended");
    } while (widget.status().animating && std::chrono::steady_clock::now() < deadline);

    check(widget.status().camera.zoom == 13.0, "the wheel still zooms, landing on target");

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
    settleCamera(widget);
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
    settleCamera(widget);
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

// A one-tile map server, in-process: answers every requested coordinate with
// the same encoded water quad. What the fade tests need is real arrivals
// through the real path -- zenoh reply, worker decode, tessellate, drain --
// because the fade clock starts the first time a tile is DRAWN.
class FakeTileServer
{
  public:
    explicit FakeTileServer(std::string key)
    {
        mvt::Tile tile;
        mvt::Layer water;
        water.name = "water";
        water.extent = 4096;
        mvt::Feature quad;
        quad.type = mvt::GeomType::Polygon;
        quad.rings.push_back({ { 0, 0 }, { 4096, 0 }, { 4096, 4096 }, { 0, 4096 }, { 0, 0 } });
        water.features.push_back(std::move(quad));
        tile.layers.push_back(std::move(water));
        auto encoded = mvt::encode(tile);
        check(encoded.has_value(), "the fake server's tile encodes");
        mBytes = std::move(*encoded);

        mService = std::make_unique<pub_sub::ZenohService<::MapTileRequest, ::MapTileResponse>>(
            std::move(key), [this](const ::MapTileRequest::Reader& req,
                                   ::MapTileResponse::Builder& resp) {
                resp.setStatus(::MapStatus::OK);
                resp.setMinzoom(10);
                resp.setMaxzoom(14);
                auto tiles = resp.initTiles(req.getTiles().size());
                for (unsigned i = 0; i < req.getTiles().size(); ++i)
                {
                    auto result = tiles[i];
                    result.setCoord(req.getTiles()[i]);
                    result.setStatus(::MapStatus::OK);
                    result.setEncoding(::MapEncoding::IDENTITY);
                    result.setData(::capnp::Data::Reader(mBytes.data(), mBytes.size()));
                }
            });
    }

  private:
    std::vector<std::uint8_t> mBytes;
    std::unique_ptr<pub_sub::ZenohService<::MapTileRequest, ::MapTileResponse>> mService;
};

void test_a_new_tile_fades_in_and_the_ticker_stops()
{
    // The crossfade, end to end: a tile that just arrived draws translucent,
    // the animation ticker repaints the widget on its own until the fade
    // settles, and then everything goes quiet -- the ticker dies and the GPU
    // memo takes over again. Stuck tilesFading or a ticker that never stops
    // are the two ways this feature turns into a battery drain.
    FakeTileServer server("test/map_fade/tile");

    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.tile_zenoh_key = "test/map_fade/tile";
    config.zoom = 14.0;
    config.tile_fade_ms = 400;

    MapWidget widget(config);
    widget.resize(256, 256);
    // Shown, so the ticker's update() reaches paintEvent -- same reasoning as
    // the heal test.
    widget.show();

    // Wait for the first arrivals to be DRAWN (not merely cached): the expose
    // paint requests, replies decode on the workers, the drain repaints.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (widget.status().tilesDrawn == 0 && std::chrono::steady_clock::now() < deadline)
    {
        pump(std::chrono::milliseconds(20));
    }
    const MapWidget::Status arriving = widget.status();
    check(arriving.tilesDrawn > 0, "tiles arrive through the fake server");
    check(arriving.tilesFading > 0, "and draw mid-fade, not popped in");

    // Past the fade, with NO manual paints: the ticker must carry it home.
    pump(std::chrono::milliseconds(700));
    const MapWidget::Status settled = widget.status();
    check(settled.tilesFading == 0, "every fade settles");
    check(settled.tilesDrawn > 0, "with the tiles still on screen");

    // Quiet: two identical paints in a row are served from the memo, which is
    // only possible if no alpha is still moving underneath.
    render(widget);
    const std::uint64_t reused = widget.status().gpu.reused;
    render(widget);
    check(widget.status().gpu.reused == reused + 1,
          "and the settled frame is served from the memo again");
}

void test_a_fading_tile_keeps_its_stand_in()
{
    // THE trap in the crossfade: a tile that is still fading must keep its
    // stand-in underneath. Treat "fading" as "arrived" and the ancestor is
    // dropped the frame the fade starts -- the new tile blends with the
    // background instead of the picture it replaces, and the map flashes dark
    // exactly where it was meant to ease over.
    FakeTileServer server("test/map_fade_standin/tile");

    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.tile_zenoh_key = "test/map_fade_standin/tile";
    config.zoom = 14.0;
    config.interactive = true;
    config.follow_vehicle = false;
    config.tile_fade_ms = 400;

    MapWidget widget(config);
    widget.resize(256, 256);
    widget.show();

    // Settle at z14: tiles arrive, fade, and go quiet.
    const auto settleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((widget.status().tilesDrawn == 0 || widget.status().tilesFading > 0) &&
           std::chrono::steady_clock::now() < settleDeadline)
    {
        pump(std::chrono::milliseconds(20));
    }
    check(widget.status().tilesDrawn > 0 && widget.status().tilesFading == 0,
          "the map settles at z14 first");

    // Zoom out a level. The z13 tiles are new: they arrive, and while they
    // FADE the cached z14 children must stand in underneath.
    scroll(widget, QPointF(128.0, 128.0), -2);
    bool sawStandInUnderFade = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        pump(std::chrono::milliseconds(10));
        const MapWidget::Status status = widget.status();
        if (status.tilesFading > 0 && status.tilesStandIn > 0)
        {
            sawStandInUnderFade = true;
            break;
        }
        if (status.tilesDrawn > 0 && status.tilesFading == 0 && status.camera.zoom < 14.0)
        {
            break; // faded out completely without ever showing a stand-in
        }
    }
    check(sawStandInUnderFade, "a fading z13 tile keeps its z14 children underneath");
}

void test_recentre_flies_back_and_lands_following()
{
    // The recentre button glides the camera home instead of teleporting it,
    // and hands control back to Follow Vehicle only on landing.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.interactive = true;
    config.follow_vehicle = false;
    config.zoom = 12.0;
    config.center_latitude = 33.6866;
    config.center_longitude = -117.8558;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);
    const map_widget::Coordinate home = widget.status().camera.center;

    // Drag away, far enough that mid-flight is unambiguous.
    drag(widget, QPointF(200.0, 150.0), QPointF(40.0, 30.0));
    render(widget);
    const map_widget::Coordinate away = widget.status().camera.center;
    check(!sameCoordinate(away, home), "the drag moved the camera");
    check(widget.status().cameraMoved, "and following is suspended");

    auto* button = widget.findChild<QAbstractButton*>();
    // isHidden(), not isVisible() -- the parent is never shown here.
    check(button != nullptr && !button->isHidden(), "the recentre button is showing");
    button->click();

    check(button->isHidden(), "the button hides the moment the fly-back starts");

    // Mid-flight: strictly between the two, still counted as camera-moved.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    render(widget);
    const MapWidget::Status midway = widget.status();
    check(midway.animating, "the fly-back is in flight");
    check(!sameCoordinate(midway.camera.center, away) &&
              !sameCoordinate(midway.camera.center, home),
          "and the camera is strictly between where it was and home");

    settleCamera(widget);
    const MapWidget::Status landed = widget.status();
    check(sameCoordinate(landed.camera.center, home), "the camera lands on the target");
    check(!landed.cameraMoved, "and control is handed back -- following resumes");
    check(!landed.animating, "with nothing left animating");
}

void test_a_drag_cancels_the_fly_back()
{
    // The grab wins. A hand on the map mid-flight stops the camera where it
    // is and leaves it suspended -- flying on out from under a drag would
    // fight the user for the wheel.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.interactive = true;
    config.follow_vehicle = false;
    config.zoom = 12.0;

    MapWidget widget(config);
    widget.resize(400, 300);
    render(widget);

    drag(widget, QPointF(200.0, 150.0), QPointF(40.0, 30.0));
    render(widget);
    auto* button = widget.findChild<QAbstractButton*>();
    check(button != nullptr && !button->isHidden(), "dragged away, button showing");
    button->click();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    render(widget);
    check(widget.status().animating, "the fly-back is in flight");

    drag(widget, QPointF(100.0, 100.0), QPointF(120.0, 110.0));
    check(!widget.status().animating, "a drag cancels it on the spot");
    check(widget.status().cameraMoved, "and the camera stays suspended where the hand put it");
}

void test_highlight_way_ids_come_out_of_a_horizon()
{
    // The join the highlight rides on: horizon segment ids collapse to the way
    // ids map_build stamps on tile features. The matched segment counts, and
    // so does every SEGMENT profile on the ROOT path -- the road ahead -- but
    // a side branch's segments must not: lighting them paints the junction.
    ::capnp::MallocMessageBuilder message;
    auto horizon = message.initRoot<::MapHorizon>();
    horizon.setHasPosition(true);
    horizon.getPosition().getWhere().setSegmentId(road_graph::makeSegmentId(1234, 7));
    horizon.getPosition().setPathId(5);

    auto paths = horizon.initPaths(2);
    paths[0].setPathId(5); // the root path is first, by the schema's contract
    paths[1].setPathId(9);

    auto profiles = horizon.initProfiles(4);
    profiles[0].setPathId(5);
    profiles[0].getValue().setSegment(road_graph::makeSegmentId(1234, 8)); // same way, next piece
    profiles[1].setPathId(5);
    profiles[1].getValue().setSegment(road_graph::makeSegmentId(777, 0)); // the road ahead
    profiles[2].setPathId(9);
    profiles[2].getValue().setSegment(road_graph::makeSegmentId(31337, 0)); // side branch: out
    profiles[3].setPathId(5);
    profiles[3].getValue().setRoadName("Jamboree Road"); // not a segment: filtered past

    const auto ids = map_widget::highlightWayIds(message.getRoot<::MapHorizon>().asReader());
    check(ids == std::vector<std::uint64_t> { 777, 1234 },
          "sorted, deduplicated, root-path-only way ids");

    // No fix means no highlight -- an empty list, not yesterday's roads.
    ::capnp::MallocMessageBuilder lost;
    auto without = lost.initRoot<::MapHorizon>();
    without.setHasPosition(false);
    check(map_widget::highlightWayIds(lost.getRoot<::MapHorizon>().asReader()).empty(),
          "no position, no highlight");
}

void test_deferred_counts_only_tiles_that_would_have_been_asked()
{
    // A viewport bigger than one request defers its tail to the next paint,
    // and the deferred counter is how that shows up in status(). It must
    // count only tiles the cap actually pushed out: the second paint walks
    // the same list with the first round's tiles skipped -- backing off here,
    // since with no server they fail on the spot -- and charging skipped
    // tiles to the cap again made the number read as a viewport permanently
    // too big when it was filling in normally.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.zoom = 14.0;
    config.request_timeout_ms = 150;

    // Large enough that the walk wants more than two full requests, so the
    // cap still bites on the second paint.
    MapWidget widget(config);
    widget.resize(6400, 4000);

    render(widget);
    const MapWidget::Status first = widget.status();
    check(first.tiles.deferred > 0, "a huge viewport defers part of its first round");

    // Let the first round's failures fold into backoff, but stay inside the
    // 500 ms first retry so the second paint skips them rather than
    // re-asking. The widget is never shown, so nothing paints in between.
    pump(std::chrono::milliseconds(320));
    check(widget.status().tiles.backingOff > 0, "the first round is backing off");

    render(widget);
    const MapWidget::Status second = widget.status();
    const std::uint64_t firstIncrement = first.tiles.deferred;
    const std::uint64_t secondIncrement = second.tiles.deferred - first.tiles.deferred;
    check(secondIncrement > 0, "the cap still bites on the second paint");
    check(secondIncrement < firstIncrement,
          "but tiles already in flight are not charged to the cap again");
}

void test_a_failed_map_heals_itself_without_a_position_stream()
{
    // The failure that matters in the car: dashboard up, map_server not yet.
    // With no position stream and no user there is NOTHING that repaints this
    // widget except itself, and requests are only issued from the paint pass.
    // So a failed batch must repaint (that paint arms the retry timer), and
    // the timer's own repaint is what asks again once the backoff expires.
    //
    // No manual render() anywhere in this test -- that is the point. Painting
    // it by hand after the failures fold would arm the timer as a side effect
    // and mask the failure-repaint this exists to pin. The widget is shown
    // instead, so its own update() calls reach paintEvent; everything that
    // happens after show() is the widget's doing.
    MapConfig_t config;
    config.position_zenoh_key.clear();
    config.zoom = 14.0;
    config.request_timeout_ms = 150;

    MapWidget widget(config);
    widget.resize(512, 512);
    widget.show();

    // The expose paint issues the first round. With no server the failures
    // come back almost at once (no queryable reads as NoReply immediately,
    // not after the timeout), fold into backoff on the drain -- and the
    // failure repaint arms the timer.
    pump(std::chrono::milliseconds(320));
    const MapWidget::Status afterFailure = widget.status();
    const std::uint64_t firstRound = afterFailure.tiles.requested;
    check(firstRound > 0, "the expose paint asks for tiles");
    check(afterFailure.tiles.backingOff > 0, "with no server they end up backing off");
    check(afterFailure.retryPending, "and the failure repaint armed the retry timer");

    // Past the 500 ms first retry: the timer repaints, the paint asks again.
    // Deadline-polled rather than one fixed wait -- retry plus scheduling
    // jitter on a loaded machine must not read as the mechanism being broken.
    const auto retryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (widget.status().tiles.requested == firstRound &&
           std::chrono::steady_clock::now() < retryDeadline)
    {
        pump(std::chrono::milliseconds(50));
    }
    check(widget.status().tiles.requested > firstRound,
          "the timer repaints on its own and the paint asks again");
}

void test_a_large_glyph_is_not_clipped_by_its_image()
{
    // Each glyph image is sized from the character's bounding box plus halo
    // padding, and the outline is drawn by mapping that box onto (pad, pad).
    // The padding exists so the halo and its antialiasing fade out INSIDE the
    // image; if a placement term is dropped -- the left bearing was, once --
    // the ink drifts toward an edge, and the first symptom is text that looks
    // shaved on one side. Now that every character is packed into an atlas,
    // clipped ink would be baked into the page and drawn that way forever.
    //
    // A descender and an accent probe all four edges, at a size where every
    // error is pixels rather than fractions.
    QFont font;
    font.setPointSizeF(30.0);

    map_widget::LabelCache cache;
    for (const QChar ch : QStringLiteral("\u00C1gjy"))
    {
        const map_widget::LabelCache::Glyph& glyph =
            cache.glyphFor(ch, font, 3.0, QColor(Qt::black), QColor(Qt::white), 1.0);
        for (int pass = 0; pass < 2; ++pass)
        {
            const QImage& image = pass == 0 ? glyph.halo : glyph.fill;
            check(!image.isNull(), "the character rasterised");
            const std::string what =
                std::string(pass == 0 ? "halo" : "fill") + " of '" + std::string(1, ch.toLatin1());

            // Every edge row and column must be empty: ink touching one means
            // the glyph was drawn too close to the boundary and has been cut.
            bool edgeInk = false;
            for (int x = 0; x < image.width(); ++x)
            {
                edgeInk = edgeInk || qAlpha(image.pixel(x, 0)) != 0 ||
                          qAlpha(image.pixel(x, image.height() - 1)) != 0;
            }
            for (int y = 0; y < image.height(); ++y)
            {
                edgeInk = edgeInk || qAlpha(image.pixel(0, y)) != 0 ||
                          qAlpha(image.pixel(image.width() - 1, y)) != 0;
            }
            check(!edgeInk, what + "' fades out inside its image rather than at the edge");
        }
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
    test_the_atlas_packs_each_glyph_once();
    test_a_packed_glyph_is_pixel_identical_to_the_one_rasterised();
    test_a_style_change_empties_the_atlas();

    test_a_road_name_is_placed();
    test_the_label_anchor_turns_with_the_map();
    test_the_label_anchor_is_right_at_half_a_turn();
    test_a_place_name_stays_upright_when_the_map_turns();
    test_a_road_label_never_reads_backwards();
    test_a_hairpin_gets_no_label_but_a_gentle_curve_does();
    test_a_road_label_lies_along_a_diagonal_road();
    test_the_glyph_tier_renders_an_alphabet_not_a_name_list();
    test_a_road_repeats_its_name_along_itself_but_never_twice_in_one_place();
    test_a_river_is_named_along_itself_and_a_lake_at_a_point();
    test_a_nameless_river_segment_claims_no_label();
    test_a_street_name_outranks_the_river_it_crosses();
    test_water_labels_respect_their_zoom_floor_and_their_toggle();
    test_a_river_outranks_a_stream();
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
    test_a_trackpad_swipe_zooms_by_what_the_fingers_asked_for();
    test_the_wheel_does_not_stop_the_map_following_the_vehicle();
    test_the_wheel_stops_at_the_camera_range_the_layout_allows();
    test_a_drag_cannot_leave_the_projection();
    test_a_sized_widget_knows_which_tiles_it_needs();
    test_a_zero_sized_widget_asks_for_nothing();
    test_a_failed_tile_backs_off_instead_of_being_asked_for_every_frame();
    test_a_new_tile_fades_in_and_the_ticker_stops();
    test_a_fading_tile_keeps_its_stand_in();
    test_recentre_flies_back_and_lands_following();
    test_a_drag_cancels_the_fly_back();
    test_highlight_way_ids_come_out_of_a_horizon();
    test_deferred_counts_only_tiles_that_would_have_been_asked();
    test_a_failed_map_heals_itself_without_a_position_stream();
    test_a_large_glyph_is_not_clipped_by_its_image();

    spdlog::set_level(spdlog::level::info);

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all map widget checks passed");
    return 0;
}
