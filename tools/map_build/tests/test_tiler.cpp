// SPDX-License-Identifier: GPL-3.0-or-later
//
// The tiler, and the geometry work whose failures RENDER.
//
//   The PROJECTION decides which tile a feature lands in. Wrong, and the map
//   draws beautifully somewhere else -- so it is anchored against the tile
//   libs/mvt and docs/map.md already name for Irvine.
//
//   SIMPLIFICATION drops points. Too aggressive and roads wander off the
//   junctions they meet; too timid and every tile carries points nobody can see.
//
//   CLIPPING cuts to the tile. A line clipped without a buffer leaves a seam
//   down every boundary; a line that leaves and re-enters must come back as TWO
//   parts, or the renderer draws a straight line across the tile between the
//   crossings.
//
//   THE TMS FLIP. The archive stores rows the other way up from every request.
//   A writer and a reader that disagree produce a map that renders perfectly,
//   mirrored about the equator -- which nobody reads as a coordinate bug. The
//   round trip below is the only place both halves are exercised together.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "map_build/tiler.h"
#include "mbtiles/archive.h"
#include "mbtiles/writer.h"
#include "mvt/decode.h"
#include "mvt/gzip.h"

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

// docs/map.md's anchor: Irvine, which the existing archive carries at
// z14/2828/6562.
constexpr std::int32_t kIrvineLat = 336865966;
constexpr std::int32_t kIrvineLon = -1178557874;

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / name;
}

map_rules::RoadClassification motorway()
{
    map_rules::RoadClassification out;
    out.renderClass = map_rules::RenderClass::Motorway;
    out.routeClass = map_rules::RouteClass::Motorway;
    out.access = map_rules::kAccessMotorcar;
    out.minZoom = 4;
    out.freeFlowSpeedKph = 105;
    return out;
}

map_rules::RoadClassification water()
{
    map_rules::RoadClassification out;
    out.renderClass = map_rules::RenderClass::Water;
    out.minZoom = 6;
    out.isArea = true;
    return out;
}

map_build::DrawInput lineAt(std::int64_t wayId, std::int32_t lat, std::int32_t lon, int points,
                            std::int32_t step, const std::string& name,
                            const map_rules::RoadClassification& classification)
{
    map_build::DrawInput out;
    out.osmWayId = wayId;
    out.classification = classification;
    out.name = name;
    for (int i = 0; i < points; ++i)
    {
        out.geometry.push_back(lat + i * step);
        out.geometry.push_back(lon + i * step);
    }
    return out;
}

void test_the_layer_vocabulary_matches_the_widget()
{
    // These strings are what dashboard/widgets/map/tessellator.cpp switches on.
    // A name that does not match does not fail -- the layer is simply never
    // drawn, and the map comes up missing its water with nothing said.
    check(std::string(map_build::layerFor(map_rules::RenderClass::Motorway)) == "transportation",
          "roads go in transportation");
    check(std::string(map_build::layerFor(map_rules::RenderClass::Minor)) == "transportation",
          "all of them, whatever the class");
    check(std::string(map_build::layerFor(map_rules::RenderClass::Water)) == "water", "water");
    check(std::string(map_build::layerFor(map_rules::RenderClass::Waterway)) == "waterway",
          "waterway");
    check(std::string(map_build::layerFor(map_rules::RenderClass::Building)) == "building",
          "building");
    check(std::string(map_build::layerFor(map_rules::RenderClass::Landuse)) == "landuse",
          "landuse");
    check(std::string(map_build::layerFor(map_rules::RenderClass::Landcover)) == "landcover",
          "landcover");
    check(std::string(map_build::layerFor(map_rules::RenderClass::Boundary)) == "boundary",
          "boundary");
    check(std::string(map_build::layerFor(map_rules::RenderClass::None)).empty(),
          "and nothing undrawable gets a layer");

    // roadPriority() reads these.
    check(std::string(map_build::roadClassFor(map_rules::RenderClass::Motorway)) == "motorway",
          "a motorway says so");
    check(std::string(map_build::roadClassFor(map_rules::RenderClass::Rail)) == "rail",
          "and rail says rail, which the tessellator draws differently");
}

void test_a_feature_lands_in_the_tile_the_rest_of_the_stack_expects()
{
    // THE anchor. If the projection were wrong the tile would still build, and
    // still render, somewhere else entirely.
    const auto path = scratch("tiler_anchor.mbtiles");

    map_build::Tiler tiler;
    tiler.add(lineAt(1, kIrvineLat, kIrvineLon, 6, 200, "Costa Mesa Freeway", motorway()));

    auto writer = mbtiles::Writer::create(path);
    check(writer.has_value(), "an archive is created");
    if (!writer)
    {
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;

    auto stats = tiler.write(*writer, options, "test", kIrvineLon - 1000, kIrvineLat - 1000,
                             kIrvineLon + 1000, kIrvineLat + 1000);
    check(stats.has_value(), "the pyramid builds");
    check(writer->finish().has_value(), "and the archive closes");
    if (!stats)
    {
        return;
    }

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the archive this tool wrote opens with the READER");
    if (!archive)
    {
        return;
    }

    // z14/2828/6562, in XYZ -- the tile docs/map.md names and libs/mvt pulls
    // out of the real archive.
    auto tile = archive->tile(14, 2828, 6562);
    check(tile.has_value() && tile->has_value(),
          "and the Irvine anchor tile is where the rest of the stack expects it");

    std::filesystem::remove(path);
}

void test_the_tms_flip_round_trips()
{
    // A writer and a reader that disagree about which way rows count produce a
    // map that renders perfectly, mirrored about the equator. Only exercising
    // both halves together catches it.
    const auto path = scratch("tiler_flip.mbtiles");

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    const std::vector<std::uint8_t> north { 'N', 'O', 'R', 'T', 'H' };
    const std::vector<std::uint8_t> south { 'S', 'O', 'U', 'T', 'H' };

    // In XYZ, y counts SOUTHWARD from the top: y=0 is the north edge.
    check(writer->put(4, 3, 0, north).has_value(), "the northern tile writes");
    check(writer->put(4, 3, 15, south).has_value(), "and the southern one");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }

    auto readNorth = archive->tile(4, 3, 0);
    auto readSouth = archive->tile(4, 3, 15);
    check(readNorth.has_value() && readNorth->has_value(), "the northern tile reads back");
    check(readSouth.has_value() && readSouth->has_value(), "and the southern one");

    if (readNorth && *readNorth && readSouth && *readSouth)
    {
        check((*readNorth)->data == north,
              "and the tile written at y=0 comes back at y=0, NOT mirrored");
        check((*readSouth)->data == south, "and likewise at the other end");
    }

    std::filesystem::remove(path);
}

void test_a_tile_decodes_with_the_attributes_a_style_reads()
{
    const auto path = scratch("tiler_attrs.mbtiles");

    map_build::Tiler tiler;
    tiler.add(lineAt(4987265, kIrvineLat, kIrvineLon, 8, 300, "Costa Mesa Freeway", motorway()));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;
    auto stats = tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon,
                             kIrvineLat);
    check(stats.has_value() && stats->tiles > 0, "at least one tile is written");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }

    auto stored = archive->tile(14, 2828, 6562);
    if (!stored || !*stored)
    {
        check(false, "the anchor tile is present");
        return;
    }

    // Stored gzipped, exactly as the spec says and as the server passes through.
    auto inflated = mvt::inflateIfCompressed((*stored)->data);
    check(inflated.has_value(), "the tile inflates");
    if (!inflated)
    {
        return;
    }

    auto tile = mvt::decode(*inflated);
    check(tile.has_value(), "and decodes");
    if (!tile)
    {
        return;
    }

    const mvt::Layer* transportation = tile->layer("transportation");
    check(transportation != nullptr, "with a transportation layer");
    if (transportation == nullptr)
    {
        return;
    }

    check(!transportation->features.empty(), "carrying features");
    check(transportation->extent == 4096, "at the extent the widget assumes");

    const mvt::Feature& feature = transportation->features[0];
    check(feature.type == mvt::GeomType::LineString, "as a line");
    check(transportation->attributeText(feature, "class") == "motorway",
          "with the class the tessellator switches on");
    check(transportation->attributeText(feature, "name") == "Costa Mesa Freeway",
          "and its name");
    check(feature.hasId && feature.id == 4987265,
          "and the source way id, which is how a client joins a tile feature back to the graph");

    std::filesystem::remove(path);
}

void test_the_archive_advertises_the_layers_it_actually_carries()
{
    // `vector_layers` is how a client learns what is in the archive. Advertise
    // a layer with no features and it waits forever for tiles that will never
    // carry it; omit a layer that is there and it never asks. Both render an
    // emptier map than the archive holds, with nothing in the logs.
    const auto path = scratch("tiler_json.mbtiles");

    map_build::DrawInput lake;
    lake.osmWayId = 42;
    lake.classification = water();
    lake.closed = true;
    const std::int32_t size = 4000;
    lake.geometry = {
        kIrvineLat,        kIrvineLon,        kIrvineLat,  kIrvineLon + size,
        kIrvineLat + size, kIrvineLon + size, kIrvineLat + size, kIrvineLon,
        kIrvineLat,        kIrvineLon,
    };

    map_build::Tiler tiler;
    tiler.add(lineAt(1, kIrvineLat, kIrvineLon, 6, 200, "Costa Mesa Freeway", motorway()));
    tiler.add(std::move(lake));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;
    check(tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon + size,
                      kIrvineLat + size)
              .has_value(),
          "the pyramid builds");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }

    const std::string& json = archive->metadata().json;
    check(!json.empty(), "the archive carries a json metadata column");
    check(json.find("\"vector_layers\"") != std::string::npos, "naming vector_layers");
    check(json.find("\"transportation\"") != std::string::npos, "with the road layer");
    check(json.find("\"water\"") != std::string::npos, "and the water layer");
    check(json.find("\"building\"") == std::string::npos,
          "and NOT a layer nothing was written into");
    check(json.find("\"name\"") != std::string::npos, "listing the fields a style reads");

    // And it survives into the TileJSON the server hands a client.
    const std::string tileJson = archive->tileJson("zenoh://map/tile/{z}/{x}/{y}");
    check(tileJson.find("\"vector_layers\"") != std::string::npos,
          "which is what reaches the client, at the top level where a style looks");

    std::filesystem::remove(path);
}

void test_a_place_label_reaches_the_widget_that_draws_it()
{
    // The label contract, end to end. dashboard/widgets/map/labels.cpp will
    // draw NOTHING unless all four of these hold: a layer literally named
    // "place", a feature of type Point, a non-empty "name:latin", and geometry
    // in the tile it claims to be in. Each one failing on its own produces a
    // map that renders perfectly and is simply missing every label -- with
    // nothing in any log.
    const auto path = scratch("tiler_place.mbtiles");

    map_build::DrawInput city;
    city.osmWayId = 150941;
    city.isPoint = true;
    city.name = "Irvine";
    city.place.kind = map_rules::PlaceKind::City;
    city.place.minZoom = 6;
    city.place.labelRank = 2;
    city.place.population = 307670;
    city.classification.renderClass = map_rules::RenderClass::Place;
    city.classification.minZoom = 6;
    city.geometry = { kIrvineLat, kIrvineLon };

    map_build::Tiler tiler;
    tiler.add(std::move(city));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;
    check(tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon, kIrvineLat)
              .has_value(),
          "the pyramid builds");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }
    auto stored = archive->tile(14, 2828, 6562);
    if (!stored || !*stored)
    {
        check(false, "the label lands in the anchor tile");
        return;
    }
    auto inflated = mvt::inflateIfCompressed((*stored)->data);
    auto tile = inflated ? mvt::decode(*inflated) : mvt::Result<mvt::Tile> {};
    if (!inflated || !tile)
    {
        check(false, "the tile decodes");
        return;
    }

    const mvt::Layer* layer = tile->layer("place");
    check(layer != nullptr, "the layer is called 'place', which is the name labels.cpp asks for");
    if (layer == nullptr || layer->features.empty())
    {
        check(false, "and it carries the label");
        return;
    }

    const mvt::Feature& label = layer->features[0];
    check(label.type == mvt::GeomType::Point, "as a Point, which is all labels.cpp accepts");
    check(layer->attributeText(label, "name:latin") == "Irvine",
          "with name:latin, which is the field labels.cpp reads");
    check(layer->attributeText(label, "name") == "Irvine",
          "and plain name too, for any reader that asks the obvious way");
    check(layer->attributeText(label, "class") == "city", "and its kind");
    check(!label.rings.empty() && label.rings.front().size() == 1,
          "and exactly one point of geometry");

    if (!label.rings.empty() && !label.rings.front().empty())
    {
        // Inside the tile, not merely somewhere. A label placed outside its own
        // tile draws in the wrong town.
        const mvt::Point& p = label.rings.front().front();
        check(p.x >= 0 && p.x <= 4096 && p.y >= 0 && p.y <= 4096,
              "positioned inside the tile that carries it");
    }

    std::filesystem::remove(path);
}

void test_a_collapsed_area_does_not_take_the_whole_tile_down()
{
    // THE most expensive failure this tiler can produce, and it is not local.
    //
    // At low zoom a small pond quantises to two coincident tile units. Written
    // as a two-point ring it makes the tile MALFORMED, and a decoder that
    // rejects a malformed tile throws away everything else in it -- every road,
    // every label, every coastline for that square. The map then reports "no
    // coverage" over a tileset that has the data, and the pond is nowhere near
    // where anyone is looking.
    //
    // So this asserts the whole tile still decodes, not merely that the pond is
    // absent.
    const auto path = scratch("tiler_collapse.mbtiles");

    map_build::DrawInput pond;
    pond.osmWayId = 99;
    pond.classification = water();
    pond.classification.minZoom = 0;
    pond.closed = true;
    // About 3 m across: sub-unit at z14, and far below one unit at z9.
    const std::int32_t tiny = 300;
    pond.geometry = {
        kIrvineLat,        kIrvineLon,        kIrvineLat,        kIrvineLon + tiny,
        kIrvineLat + tiny, kIrvineLon + tiny, kIrvineLat + tiny, kIrvineLon,
        kIrvineLat,        kIrvineLon,
    };

    map_build::Tiler tiler;
    tiler.add(std::move(pond));
    // Long enough to survive generalization at z9: a road shorter than a
    // couple of tile units is a dot, and is dropped by design.
    tiler.add(lineAt(1, kIrvineLat, kIrvineLon, 8, 20000, "Costa Mesa Freeway", motorway()));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 9;
    options.maxZoom = 14;
    options.progressEvery = 0;
    // Generalization off, deliberately. It would drop the pond long before it
    // could collapse, and then this test would prove only that the FIRST
    // defence works -- leaving the one that protects the tile untested.
    options.minExtent = 0.0;
    check(tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon, kIrvineLat)
              .has_value(),
          "the pyramid builds");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }

    // Every zoom, because the collapse happens at whichever zoom the shape
    // finally falls below one tile unit.
    const std::uint32_t xs[] = { 88, 176, 353, 707, 1414, 2828 };
    const std::uint32_t ys[] = { 205, 410, 820, 1640, 3281, 6562 };
    for (std::uint8_t z = 9; z <= 14; ++z)
    {
        const std::size_t i = z - 9;
        auto stored = archive->tile(z, xs[i], ys[i]);
        if (!stored || !*stored)
        {
            check(false, "z" + std::to_string(z) + " tile is present");
            continue;
        }
        auto inflated = mvt::inflateIfCompressed((*stored)->data);
        auto tile = inflated ? mvt::decode(*inflated) : mvt::Result<mvt::Tile> {};
        check(inflated.has_value() && tile.has_value(),
              "z" + std::to_string(z) + " decodes -- a collapsed pond must not " +
                  "make the whole tile malformed");
        if (!inflated || !tile)
        {
            continue;
        }

        // And every ring it does carry can actually be filled.
        for (const mvt::Layer& layer : tile->layers)
        {
            for (const mvt::Feature& feature : layer.features)
            {
                if (feature.type != mvt::GeomType::Polygon)
                {
                    continue;
                }
                for (const auto& ring : feature.rings)
                {
                    check(ring.size() >= 3,
                          "every polygon ring written has at least three points");
                }
            }
        }

        // The road is what a reader came for, and it survives.
        check(tile->layer("transportation") != nullptr,
              "and the road in that tile is still there at z" + std::to_string(z));
    }

    std::filesystem::remove(path);
}

void test_identity_survives_where_a_client_joins_back_to_the_graph()
{
    // THE trade this tiler makes, in both directions.
    //
    // Below mergeBelowZoom, ways sharing every attribute become one feature and
    // the source id is dropped -- because a merged feature is "Jamboree Road",
    // not any one way, and keeping the first id would silently attribute the
    // whole road to an arbitrary one of them.
    //
    // At and above it, every way keeps its own id, because that is what lets a
    // client recolour the road it is on or draw a route by highlighting the
    // features it already has, rather than overlaying a full-precision polyline
    // that visibly diverges from the tile geometry.
    //
    // Get the boundary wrong in one direction and low-zoom tiles bloat; in the
    // other, road highlighting silently picks the wrong road.
    const auto path = scratch("tiler_merge.mbtiles");

    // Laid out along a single latitude, and short, so that all three land in
    // ONE tile at both zooms. Ways in different tiles cannot merge whatever the
    // rule says, and a test whose geometry straddles a tile boundary quietly
    // stops testing the rule at all.
    const auto eastward = [](std::int64_t wayId, std::int32_t lonOffset,
                             const std::string& name) {
        map_build::DrawInput out;
        out.osmWayId = wayId;
        out.classification = motorway();
        out.name = name;
        for (int i = 0; i < 6; ++i)
        {
            out.geometry.push_back(kIrvineLat);
            out.geometry.push_back(kIrvineLon + lonOffset + i * 8000);
        }
        return out;
    };

    map_build::Tiler tiler;
    // Two ways of one road, same class and name, laid end to end.
    tiler.add(eastward(1001, 0, "Jamboree Road"));
    tiler.add(eastward(1002, 50000, "Jamboree Road"));
    // And a different road that must NOT be folded in with them.
    tiler.add(eastward(1003, 0, "Culver Drive"));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 12;
    options.maxZoom = 13;
    options.progressEvery = 0;
    check(tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon, kIrvineLat)
              .has_value(),
          "the pyramid builds");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }

    const auto roadsIn = [&](std::uint8_t z, std::uint32_t x,
                             std::uint32_t y) -> std::vector<mvt::Feature> {
        auto stored = archive->tile(z, x, y);
        if (!stored || !*stored)
        {
            return {};
        }
        auto inflated = mvt::inflateIfCompressed((*stored)->data);
        auto tile = inflated ? mvt::decode(*inflated) : mvt::Result<mvt::Tile> {};
        if (!inflated || !tile)
        {
            return {};
        }
        const mvt::Layer* layer = tile->layer("transportation");
        return layer == nullptr ? std::vector<mvt::Feature> {} : layer->features;
    };

    // z12: below the boundary, so the two Jamboree ways fold together and
    // Culver stays separate -- two features, not three.
    const auto low = roadsIn(12, 707, 1640);
    check(low.size() == 2, "below the boundary, ways of one road become one feature");
    for (const mvt::Feature& feature : low)
    {
        if (feature.rings.size() > 1)
        {
            check(!feature.hasId,
                  "and the MERGED feature carries no source id, because it is no longer any "
                  "one way");
        }
        else
        {
            // The road that had nothing to merge with is untouched, id and all.
            // Stripping ids from everything would be the easy mistake here, and
            // it would cost identity for no size saving at all.
            check(feature.hasId, "while a road that merged with nothing keeps its id");
        }
    }

    // z13: at the boundary, every way is its own feature and keeps its id.
    const auto high = roadsIn(13, 1414, 3281);
    check(high.size() >= 2, "at the boundary the ways are separate features again");
    for (const mvt::Feature& feature : high)
    {
        check(feature.hasId && feature.id >= 1001 && feature.id <= 1003,
              "each keeping the source way id a client joins back to the graph with");
        check(feature.rings.size() == 1, "and none of them is a merged multi-part feature");
    }

    std::filesystem::remove(path);
}

void test_a_lake_with_an_island_keeps_its_hole()
{
    // The vector tile format has no flag for a hole. The ONLY thing separating
    // an island from the lake around it is which way its ring turns, so a
    // reversed inner ring does not fail -- it fills the island and punches out
    // the lake, and the map looks like a rendering fault rather than a data one.
    //
    // The roles survive projection deliberately: Web Mercator's y grows
    // southward, so a ring's sign flips between latitude and tile coordinates,
    // and a winding decided before that point is decided twice.
    const auto path = scratch("tiler_hole.mbtiles");

    map_build::DrawInput lake;
    lake.osmWayId = 777;
    lake.classification = water();
    lake.classification.minZoom = 0;
    lake.closed = true;
    const std::int32_t size = 40000;
    lake.geometry = {
        kIrvineLat,        kIrvineLon,        kIrvineLat,        kIrvineLon + size,
        kIrvineLat + size, kIrvineLon + size, kIrvineLat + size, kIrvineLon,
    };
    // An island in the middle, given in the SAME direction as the outer ring --
    // which is what OSM data does, and what makes the role rather than the
    // input winding the thing that has to be believed.
    lake.innerRings.push_back({
        kIrvineLat + size / 4,     kIrvineLon + size / 4,
        kIrvineLat + size / 4,     kIrvineLon + size / 2,
        kIrvineLat + size / 2,     kIrvineLon + size / 2,
        kIrvineLat + size / 2,     kIrvineLon + size / 4,
    });

    map_build::Tiler tiler;
    tiler.add(std::move(lake));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;
    check(tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon + size,
                      kIrvineLat + size)
              .has_value(),
          "the pyramid builds");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }
    auto stored = archive->tile(14, 2828, 6562);
    if (!stored || !*stored)
    {
        check(false, "the lake lands in the anchor tile");
        return;
    }
    auto inflated = mvt::inflateIfCompressed((*stored)->data);
    auto tile = inflated ? mvt::decode(*inflated) : mvt::Result<mvt::Tile> {};
    if (!inflated || !tile)
    {
        check(false, "the tile decodes");
        return;
    }

    const mvt::Layer* layer = tile->layer("water");
    if (layer == nullptr || layer->features.empty())
    {
        check(false, "the water layer carries the lake");
        return;
    }

    const mvt::Feature& lakeOut = layer->features[0];
    check(lakeOut.rings.size() == 2, "the lake comes back as two rings, not one");
    if (lakeOut.rings.size() != 2)
    {
        return;
    }
    check(mvt::isExteriorRing(lakeOut.rings[0]),
          "the first ring is EXTERIOR -- positive area, which is what fills");
    check(!mvt::isExteriorRing(lakeOut.rings[1]),
          "and the island is INTERIOR -- negative area, which is what makes it a hole");
}

void test_min_zoom_keeps_detail_out_of_low_zoom_tiles()
{
    // The clutter dial, applied at BUILD time. A style can only ever be
    // stricter than this -- nothing can draw what the tile does not carry.
    const auto path = scratch("tiler_minzoom.mbtiles");

    map_rules::RoadClassification service;
    service.renderClass = map_rules::RenderClass::Service;
    service.routeClass = map_rules::RouteClass::Service;
    service.access = map_rules::kAccessMotorcar;
    service.minZoom = 14;

    map_build::Tiler tiler;
    // Long enough to clear generalization at z10, so the only thing this
    // test measures is the ZOOM rule.
    tiler.add(lineAt(1, kIrvineLat, kIrvineLon, 6, 8000, "Freeway", motorway()));
    tiler.add(lineAt(2, kIrvineLat, kIrvineLon, 6, 8000, "Parking aisle", service));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 10;
    options.maxZoom = 14;
    options.progressEvery = 0;
    auto stats = tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon,
                             kIrvineLat);
    check(stats.has_value(), "the pyramid builds");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }

    const auto count = [&](std::uint8_t z, std::uint32_t x, std::uint32_t y) -> std::size_t {
        auto stored = archive->tile(z, x, y);
        if (!stored || !*stored)
        {
            return 0;
        }
        auto inflated = mvt::inflateIfCompressed((*stored)->data);
        if (!inflated)
        {
            return 0;
        }
        auto tile = mvt::decode(*inflated);
        if (!tile)
        {
            return 0;
        }
        const mvt::Layer* layer = tile->layer("transportation");
        return layer == nullptr ? 0 : layer->features.size();
    };

    // The z10 tile containing Irvine.
    check(count(10, 176, 410) == 1, "at z10 only the freeway is carried");
    check(count(14, 2828, 6562) == 2, "and at z14 both are");

    std::filesystem::remove(path);
}

// Tile one road, then count the pieces the anchor tile holds it in. Returns 0
// if anything upstream of the count failed, which the caller reads as a failure
// like any other.
std::size_t partsInAnchorTile(const std::string& file, std::vector<std::int32_t> geometry)
{
    const auto path = scratch(file);

    map_build::DrawInput feature;
    feature.osmWayId = 7;
    feature.classification = motorway();
    feature.name = "Excursion";
    feature.geometry = std::move(geometry);

    // The pyramid is built over the road's own extent, so the tiler visits
    // every tile the road touches.
    std::int32_t west = feature.geometry[1];
    std::int32_t east = west;
    std::int32_t south = feature.geometry[0];
    std::int32_t north = south;
    for (std::size_t i = 0; i < feature.geometry.size(); i += 2)
    {
        south = std::min(south, feature.geometry[i]);
        north = std::max(north, feature.geometry[i]);
        west = std::min(west, feature.geometry[i + 1]);
        east = std::max(east, feature.geometry[i + 1]);
    }

    map_build::Tiler tiler;
    tiler.add(std::move(feature));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        return 0;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;
    if (!tiler.write(*writer, options, "test", west, south, east, north) || !writer->finish())
    {
        return 0;
    }

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        return 0;
    }
    auto stored = archive->tile(14, 2828, 6562);
    if (!stored || !*stored)
    {
        return 0;
    }
    auto inflated = mvt::inflateIfCompressed((*stored)->data);
    auto tile = inflated ? mvt::decode(*inflated) : mvt::Result<mvt::Tile> {};
    if (!inflated || !tile)
    {
        return 0;
    }
    const mvt::Layer* layer = tile->layer("transportation");
    if (layer == nullptr)
    {
        return 0;
    }

    std::size_t parts = 0;
    for (const mvt::Feature& drawn : layer->features)
    {
        parts += drawn.rings.size();
    }

    std::filesystem::remove(path);
    return parts;
}

void test_a_line_that_leaves_and_returns_comes_back_in_pieces()
{
    // A road can leave a tile two ways, and the clipper ends the piece by a
    // different branch for each. Both shapes are here because covering only one
    // leaves the other branch free to be deleted with every check still green
    // -- which is exactly what happened when this test had only the first.
    const std::int32_t big = 400000;  // about 4 km, several tiles at z14

    // ONE: out east, along, and back west. The middle leg lies wholly outside
    // the tile. Joining the two visits would draw a line straight across the
    // tile between them.
    check(partsInAnchorTile("tiler_parts_excursion.mbtiles",
                            {
                                kIrvineLat,        kIrvineLon,
                                kIrvineLat,        kIrvineLon + big,
                                kIrvineLat + 1000, kIrvineLon + big,
                                kIrvineLat + 1000, kIrvineLon,
                            }) >= 2,
          "a road that leaves the tile and comes back is TWO parts, not one across the middle");

    // TWO: out east and straight back, with no leg wholly outside -- every leg
    // here crosses the tile, so only the cut-at-the-far-end branch can end the
    // piece. Joined, the two crossings of the east edge get a segment drawn
    // between them: a phantom road down the tile boundary.
    check(partsInAnchorTile("tiler_parts_spur.mbtiles",
                            {
                                kIrvineLat,        kIrvineLon,
                                kIrvineLat + 500,  kIrvineLon + big,
                                kIrvineLat + 1000, kIrvineLon,
                            }) >= 2,
          "and so is one that turns around outside and comes straight back");
}

void test_geometry_extends_past_the_tile_edge()
{
    // The buffer. Without it every line stops dead at the boundary and a
    // renderer's line joins leave a seam down every tile edge.
    const auto path = scratch("tiler_buffer.mbtiles");

    map_build::Tiler tiler;
    // A long road that crosses several tiles at z14.
    tiler.add(lineAt(1, kIrvineLat, kIrvineLon, 40, 3000, "Long Road", motorway()));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;
    auto stats = tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat,
                             kIrvineLon + 120000, kIrvineLat + 120000);
    check(stats.has_value() && stats->tiles > 1, "the road spans several tiles");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }

    auto stored = archive->tile(14, 2828, 6562);
    if (!stored || !*stored)
    {
        check(false, "the anchor tile is present");
        return;
    }
    auto inflated = mvt::inflateIfCompressed((*stored)->data);
    auto tile = inflated ? mvt::decode(*inflated) : mvt::Result<mvt::Tile> {};
    if (!inflated || !tile)
    {
        check(false, "the tile decodes");
        return;
    }

    const mvt::Layer* layer = tile->layer("transportation");
    if (layer == nullptr || layer->features.empty())
    {
        check(false, "the tile has a road");
        return;
    }

    bool outside = false;
    for (const mvt::Feature& feature : layer->features)
    {
        for (const auto& ring : feature.rings)
        {
            for (const mvt::Point& point : ring)
            {
                if (point.x < 0 || point.y < 0 || point.x > 4096 || point.y > 4096)
                {
                    outside = true;
                }
            }
        }
    }
    check(outside, "and its geometry runs PAST the tile edge, into the buffer");

    std::filesystem::remove(path);
}

void test_an_area_stays_an_area_through_clipping()
{
    // A polygon clipped as a line would leave the fill open and the renderer
    // would close it across the tile, painting a wedge of water over the city.
    const auto path = scratch("tiler_area.mbtiles");

    map_build::DrawInput lake;
    lake.osmWayId = 42;
    lake.classification = water();
    lake.name = "Reservoir";
    lake.closed = true;
    const std::int32_t size = 60000;  // spans more than one z14 tile
    lake.geometry = {
        kIrvineLat,        kIrvineLon,
        kIrvineLat,        kIrvineLon + size,
        kIrvineLat + size, kIrvineLon + size,
        kIrvineLat + size, kIrvineLon,
        kIrvineLat,        kIrvineLon,
    };

    map_build::Tiler tiler;
    tiler.add(std::move(lake));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;
    auto stats = tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon + size,
                             kIrvineLat + size);
    check(stats.has_value() && stats->tiles > 1, "the lake spans several tiles");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }

    auto stored = archive->tile(14, 2828, 6562);
    if (!stored || !*stored)
    {
        check(false, "the anchor tile is present");
        return;
    }
    auto inflated = mvt::inflateIfCompressed((*stored)->data);
    auto tile = inflated ? mvt::decode(*inflated) : mvt::Result<mvt::Tile> {};
    if (!inflated || !tile)
    {
        check(false, "the tile decodes");
        return;
    }

    const mvt::Layer* layer = tile->layer("water");
    check(layer != nullptr, "the water layer is there");
    if (layer == nullptr || layer->features.empty())
    {
        check(false, "with the lake in it");
        return;
    }

    const mvt::Feature& feature = layer->features[0];
    check(feature.type == mvt::GeomType::Polygon, "and it is still a POLYGON after clipping");
    check(!feature.rings.empty() && feature.rings[0].size() >= 3,
          "with a ring that can be filled");
    check(mvt::signedArea2(feature.rings[0]) != 0, "and a non-zero area");

    std::filesystem::remove(path);
}

void test_simplification_drops_collinear_points()
{
    // A straight road with forty shape points is forty points of tile for a
    // shape two points describe. What must NOT happen is a road bending
    // somewhere it does not.
    const auto path = scratch("tiler_simplify.mbtiles");

    map_build::DrawInput straight;
    straight.osmWayId = 1;
    straight.classification = motorway();
    for (int i = 0; i < 40; ++i)
    {
        // Dead straight, one metre apart.
        straight.geometry.push_back(kIrvineLat);
        straight.geometry.push_back(kIrvineLon + i * 90);
    }

    map_build::Tiler tiler;
    tiler.add(std::move(straight));

    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        check(false, "archive created");
        return;
    }

    map_build::TileOptions options;
    options.minZoom = 14;
    options.maxZoom = 14;
    options.progressEvery = 0;
    auto stats = tiler.write(*writer, options, "test", kIrvineLon, kIrvineLat, kIrvineLon,
                             kIrvineLat);
    check(stats.has_value(), "the pyramid builds");
    check(writer->finish().has_value(), "the archive closes");

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        check(false, "archive opens");
        return;
    }
    auto stored = archive->tile(14, 2828, 6562);
    if (!stored || !*stored)
    {
        check(false, "the anchor tile is present");
        return;
    }
    auto inflated = mvt::inflateIfCompressed((*stored)->data);
    auto tile = inflated ? mvt::decode(*inflated) : mvt::Result<mvt::Tile> {};
    if (!inflated || !tile)
    {
        check(false, "the tile decodes");
        return;
    }

    const mvt::Layer* layer = tile->layer("transportation");
    if (layer == nullptr || layer->features.empty())
    {
        check(false, "the road is in the tile");
        return;
    }

    check(layer->features[0].rings[0].size() < 10,
          "forty collinear points collapse to a handful");
    check(layer->features[0].rings[0].size() >= 2, "but never fewer than the two that make a line");

    std::filesystem::remove(path);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_the_layer_vocabulary_matches_the_widget();
    test_a_feature_lands_in_the_tile_the_rest_of_the_stack_expects();
    test_the_tms_flip_round_trips();
    test_a_tile_decodes_with_the_attributes_a_style_reads();
    test_the_archive_advertises_the_layers_it_actually_carries();
    test_a_place_label_reaches_the_widget_that_draws_it();
    test_a_collapsed_area_does_not_take_the_whole_tile_down();
    test_identity_survives_where_a_client_joins_back_to_the_graph();
    test_a_lake_with_an_island_keeps_its_hole();
    test_min_zoom_keeps_detail_out_of_low_zoom_tiles();
    test_a_line_that_leaves_and_returns_comes_back_in_pieces();
    test_geometry_extends_past_the_tile_edge();
    test_an_area_stays_an_area_through_clipping();
    test_simplification_drops_collinear_points();

    spdlog::set_level(spdlog::level::info);
    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all tiler checks passed");
    return 0;
}
