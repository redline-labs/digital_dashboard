// SPDX-License-Identifier: GPL-3.0-or-later
//
// Encode, decode, and require the two to agree.
//
// The round trip is the strongest test either direction has, and it is also the
// one that can be fooled: an encoder bug that the decoder mirrors passes it. So
// every case below also asserts something about the BYTES or about a property
// the format guarantees, not only that what went in came out.
//
// The traps are decode.h's, from the writing side:
//
//   | Trap                                | What it looks like                |
//   |-------------------------------------|-----------------------------------|
//   | cursor persists across commands     | later rings crumple onto the      |
//   |                                     |   tile corner                     |
//   | ClosePath does not repeat point 1   | a zero-length edge, drawn as a    |
//   |                                     |   spike by some renderers         |
//   | ring winding decides hole/exterior  | every lake with an island fills   |
//   | extent is per layer                 | one layer at the wrong scale      |

#include <cstdint>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>

#include "mvt/decode.h"
#include "mvt/gzip.h"
#include "mvt/encode.h"
#include "mvt/tile.h"

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

mvt::Tile roundTrip(const mvt::Tile& in, bool& ok)
{
    ok = false;
    auto bytes = mvt::encode(in);
    if (!bytes)
    {
        SPDLOG_ERROR("encode failed: {}", mvt::to_string(bytes.error()));
        return {};
    }
    auto out = mvt::decode(*bytes);
    if (!out)
    {
        SPDLOG_ERROR("decode failed: {}", mvt::to_string(out.error()));
        return {};
    }
    ok = true;
    return *out;
}

mvt::Layer roadLayer()
{
    mvt::Layer layer;
    layer.name = "transportation";
    layer.version = 2;
    layer.extent = 4096;
    layer.keys = { "class", "name" };
    layer.values = { mvt::Value { std::string("motorway") },
                     mvt::Value { std::string("Costa Mesa Freeway") } };
    return layer;
}

void test_an_empty_tile_round_trips()
{
    bool ok = false;
    const mvt::Tile out = roundTrip(mvt::Tile {}, ok);
    check(ok, "an empty tile encodes and decodes");
    check(out.layers.empty(), "and has no layers");
}

void test_a_line_round_trips_with_its_attributes()
{
    mvt::Tile in;
    mvt::Layer layer = roadLayer();

    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.hasId = true;
    feature.id = 327832637014017;  // a real SegmentId shape
    feature.tags = { 0, 0, 1, 1 };
    feature.rings = { { { 100, 200 }, { 300, 250 }, { 900, 1000 } } };
    layer.features.push_back(feature);

    in.layers.push_back(layer);

    bool ok = false;
    const mvt::Tile out = roundTrip(in, ok);
    check(ok, "a line tile round trips");
    if (!ok || out.layers.empty() || out.layers[0].features.empty())
    {
        return;
    }

    const mvt::Layer& decoded = out.layers[0];
    check(decoded.name == "transportation", "the layer keeps its name");
    check(decoded.extent == 4096, "and its extent");

    const mvt::Feature& got = decoded.features[0];
    check(got.type == mvt::GeomType::LineString, "the feature keeps its type");
    check(got.hasId && got.id == 327832637014017,
          "and its id -- which is how a tile feature joins back to the road graph");
    check(got.rings.size() == 1 && got.rings[0].size() == 3, "and all three points");
    if (got.rings.size() == 1 && got.rings[0].size() == 3)
    {
        check(got.rings[0][0] == mvt::Point { 100, 200 }, "the first point");
        check(got.rings[0][1] == mvt::Point { 300, 250 }, "the second");
        check(got.rings[0][2] == mvt::Point { 900, 1000 }, "and the third");
    }

    check(decoded.attributeText(got, "class") == "motorway", "the class attribute survives");
    check(decoded.attributeText(got, "name") == "Costa Mesa Freeway", "and the name");
}

void test_the_cursor_persists_across_rings()
{
    // THE encoder trap. A multi-part line's second ring starts from wherever
    // the first ended, not from the tile corner. Resetting the cursor per ring
    // encodes the second part relative to (0,0), which decodes into a shape
    // crumpled into the corner -- a tile that renders, wrongly.
    mvt::Tile in;
    mvt::Layer layer = roadLayer();

    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.rings = {
        { { 100, 100 }, { 200, 200 } },
        { { 3000, 3000 }, { 3100, 3100 } },
    };
    layer.features.push_back(feature);
    in.layers.push_back(layer);

    bool ok = false;
    const mvt::Tile out = roundTrip(in, ok);
    check(ok, "a two-part line round trips");
    if (!ok || out.layers.empty() || out.layers[0].features.empty())
    {
        return;
    }

    const mvt::Feature& got = out.layers[0].features[0];
    check(got.rings.size() == 2, "with both parts");
    if (got.rings.size() == 2)
    {
        check(got.rings[1][0] == mvt::Point { 3000, 3000 },
              "and the second part starts where it was put, not at the tile corner");
        check(got.rings[1][1] == mvt::Point { 3100, 3100 }, "and continues correctly");
    }
}

void test_a_polygon_closes_without_repeating_its_first_point()
{
    // ClosePath implies the closing edge. Writing the repeated point as well
    // leaves a zero-length final edge, which some renderers draw as a spike and
    // which makes every polygon one point larger than it needs to be.
    mvt::Tile in;
    mvt::Layer layer;
    layer.name = "water";
    layer.version = 2;

    mvt::Feature feature;
    feature.type = mvt::GeomType::Polygon;
    // Given closed, as a caller naturally has it.
    feature.rings = { { { 0, 0 }, { 100, 0 }, { 100, 100 }, { 0, 100 }, { 0, 0 } } };
    layer.features.push_back(feature);
    in.layers.push_back(layer);

    bool ok = false;
    const mvt::Tile out = roundTrip(in, ok);
    check(ok, "a polygon round trips");
    if (!ok || out.layers.empty() || out.layers[0].features.empty())
    {
        return;
    }

    const mvt::Feature& got = out.layers[0].features[0];
    check(got.rings.size() == 1, "with one ring");
    if (got.rings.empty())
    {
        return;
    }

    // THE ROUND TRIP IS NOT POINT-IDENTICAL FOR POLYGONS, and that is correct.
    // A closed ring goes in with five points; ClosePath implies the closing
    // edge, so four are written and four come back. decode.cpp leaves rings
    // open on purpose -- signedArea2 wraps with a modulo for exactly this
    // reason. Writing the repeat as well would leave a zero-length final edge.
    check(got.rings[0].size() == 4, "and comes back OPEN -- ClosePath implies the closing edge");
    check(got.rings[0].front() != got.rings[0].back(), "so the first point is not repeated");
    check(got.rings[0][0] == mvt::Point { 0, 0 }, "the corners are where they were");
    check(got.rings[0][1] == mvt::Point { 100, 0 }, "all");
    check(got.rings[0][2] == mvt::Point { 100, 100 }, "four");
    check(got.rings[0][3] == mvt::Point { 0, 100 }, "of them");

    // And the shape is unchanged: same area, which is what a renderer cares
    // about.
    check(mvt::signedArea2(got.rings[0]) == mvt::signedArea2(feature.rings[0]),
          "and the ring encloses the same area either way round");
}

void test_a_degenerate_ring_cannot_poison_the_tile()
{
    // The failure this prevents is not local to the feature. A ring of [A,B,A]
    // -- what a small lake becomes once quantised to tile units at low zoom --
    // loses its repeated closing point to ClosePath and would go out as TWO
    // points. That makes the TILE malformed, and a decoder that rejects a
    // malformed tile discards everything else in it: every road, every label,
    // every coastline in the same square vanishes, and the map reports "no
    // coverage" over a tileset that has the data.
    //
    // So the assertion is that the ROAD survives, not merely that the sliver is
    // gone.
    mvt::Tile in;

    mvt::Layer water;
    water.name = "water";
    water.version = 2;
    mvt::Feature pond;
    pond.type = mvt::GeomType::Polygon;
    pond.rings = { { { 10, 10 }, { 40, 40 }, { 10, 10 } } };
    water.features.push_back(pond);

    mvt::Feature spike;
    spike.type = mvt::GeomType::Polygon;
    // Two distinct points, given without the closing repeat.
    spike.rings = { { { 0, 0 }, { 50, 50 } } };
    water.features.push_back(spike);
    in.layers.push_back(water);

    mvt::Layer roads;
    roads.name = "transportation";
    roads.version = 2;
    mvt::Feature road;
    road.type = mvt::GeomType::LineString;
    road.rings = { { { 0, 0 }, { 4096, 4096 } } };
    roads.features.push_back(road);
    in.layers.push_back(roads);

    bool ok = false;
    const mvt::Tile out = roundTrip(in, ok);
    check(ok, "the tile still DECODES with a degenerate ring in it");
    if (!ok)
    {
        return;
    }

    const mvt::Layer* drawnWater = out.layer("water");
    if (drawnWater != nullptr)
    {
        for (const mvt::Feature& feature : drawnWater->features)
        {
            for (const auto& ring : feature.rings)
            {
                check(ring.size() >= 3, "and every ring that survived can be filled");
            }
        }
    }

    const mvt::Layer* drawnRoads = out.layer("transportation");
    check(drawnRoads != nullptr && !drawnRoads->features.empty(),
          "and the road sharing the tile is untouched");

    // A two-point LINE is perfectly legal and must not be caught by the same
    // rule -- that would delete most of the road network.
    if (drawnRoads != nullptr && !drawnRoads->features.empty())
    {
        check(drawnRoads->features[0].rings.size() == 1 &&
                  drawnRoads->features[0].rings[0].size() == 2,
              "a two-point line is still two points");
    }
}

void test_ring_winding_survives()
{
    // Winding is the only thing that says which ring is a hole. An encoder that
    // rewound rings -- or a caller that let it -- fills every lake with an
    // island solid.
    mvt::Tile in;
    mvt::Layer layer;
    layer.name = "water";
    layer.version = 2;

    mvt::Feature feature;
    feature.type = mvt::GeomType::Polygon;
    // Exterior clockwise (positive area in MVT's y-down space), hole the other
    // way round.
    feature.rings = {
        { { 0, 0 }, { 1000, 0 }, { 1000, 1000 }, { 0, 1000 }, { 0, 0 } },
        { { 200, 200 }, { 200, 800 }, { 800, 800 }, { 800, 200 }, { 200, 200 } },
    };
    layer.features.push_back(feature);
    in.layers.push_back(layer);

    const std::int64_t areaBefore0 = mvt::signedArea2(feature.rings[0]);
    const std::int64_t areaBefore1 = mvt::signedArea2(feature.rings[1]);
    check(areaBefore0 * areaBefore1 < 0, "the two rings wind opposite ways to begin with");

    bool ok = false;
    const mvt::Tile out = roundTrip(in, ok);
    check(ok, "a polygon with a hole round trips");
    if (!ok || out.layers.empty() || out.layers[0].features.empty())
    {
        return;
    }

    const mvt::Feature& got = out.layers[0].features[0];
    check(got.rings.size() == 2, "with both rings");
    if (got.rings.size() != 2)
    {
        return;
    }

    const std::int64_t after0 = mvt::signedArea2(got.rings[0]);
    const std::int64_t after1 = mvt::signedArea2(got.rings[1]);
    check((areaBefore0 < 0) == (after0 < 0), "the exterior keeps its winding");
    check((areaBefore1 < 0) == (after1 < 0), "and so does the hole");
    check(after0 * after1 < 0, "so they still wind opposite ways");
}

void test_a_non_default_extent_survives()
{
    // extent is PER LAYER. A tile built at 8192 and read as 4096 draws that
    // layer at half scale -- which looks like a projection bug, not a tile one.
    mvt::Tile in;
    mvt::Layer layer = roadLayer();
    layer.extent = 8192;

    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.rings = { { { 0, 0 }, { 8000, 8000 } } };
    layer.features.push_back(feature);
    in.layers.push_back(layer);

    bool ok = false;
    const mvt::Tile out = roundTrip(in, ok);
    check(ok, "a tile with a non-default extent round trips");
    if (ok && !out.layers.empty())
    {
        check(out.layers[0].extent == 8192, "keeping its extent");
    }
}

void test_negative_coordinates_survive()
{
    // Tiles carry a buffer, so real points fall outside 0..extent on both
    // sides. Clamping them puts a seam down every tile boundary.
    mvt::Tile in;
    mvt::Layer layer = roadLayer();

    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.rings = { { { -256, -256 }, { 2048, 2048 }, { 4352, 4352 } } };
    layer.features.push_back(feature);
    in.layers.push_back(layer);

    bool ok = false;
    const mvt::Tile out = roundTrip(in, ok);
    check(ok, "a tile with buffered coordinates round trips");
    if (ok && !out.layers.empty() && !out.layers[0].features.empty())
    {
        const auto& ring = out.layers[0].features[0].rings[0];
        check(ring[0] == mvt::Point { -256, -256 }, "a point outside the tile on the low side");
        check(ring[2] == mvt::Point { 4352, 4352 }, "and on the high side");
    }
}

void test_every_value_kind_survives()
{
    mvt::Tile in;
    mvt::Layer layer;
    layer.name = "mixed";
    layer.version = 2;
    layer.keys = { "text", "number", "integer", "flag" };
    layer.values = {
        mvt::Value { std::string("hello") },
        mvt::Value { 1.5 },
        mvt::Value { std::int64_t { -42 } },
        mvt::Value { true },
    };

    mvt::Feature feature;
    feature.type = mvt::GeomType::Point;
    feature.rings = { { { 10, 20 } } };
    feature.tags = { 0, 0, 1, 1, 2, 2, 3, 3 };
    layer.features.push_back(feature);
    in.layers.push_back(layer);

    bool ok = false;
    const mvt::Tile out = roundTrip(in, ok);
    check(ok, "a tile with every value kind round trips");
    if (!ok || out.layers.empty() || out.layers[0].features.empty())
    {
        return;
    }

    const mvt::Layer& decoded = out.layers[0];
    const mvt::Feature& got = decoded.features[0];
    check(decoded.attributeText(got, "text") == "hello", "a string");
    check(decoded.attributeText(got, "integer") == "-42", "a negative integer");

    auto flag = decoded.attribute(got, "flag");
    check(flag.has_value() && std::holds_alternative<bool>(*flag), "a bool");
}

void test_a_tag_index_past_the_table_is_refused()
{
    // Written, it decodes to a different attribute or to none, and the tile
    // still renders -- so the failure only ever shows up as a road with
    // somebody else's name on it.
    mvt::Tile in;
    mvt::Layer layer = roadLayer();

    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.rings = { { { 0, 0 }, { 10, 10 } } };
    feature.tags = { 0, 99 };  // value index 99 does not exist
    layer.features.push_back(feature);
    in.layers.push_back(layer);

    auto bytes = mvt::encode(in);
    check(!bytes.has_value(), "a tag index past the value table is refused");
}

void test_gzip_round_trips_through_the_decoder()
{
    // What actually goes into an .mbtiles. The archive stores tiles gzipped and
    // every consumer inflates before decoding, so this is the real path.
    mvt::Tile in;
    mvt::Layer layer = roadLayer();
    mvt::Feature feature;
    feature.type = mvt::GeomType::LineString;
    feature.tags = { 0, 0 };
    for (int i = 0; i < 200; ++i)
    {
        feature.rings.push_back({ { i * 10, i * 10 }, { i * 10 + 5, i * 10 + 5 } });
    }
    layer.features.push_back(feature);
    in.layers.push_back(layer);

    auto raw = mvt::encode(in);
    check(raw.has_value(), "the tile encodes");
    if (!raw)
    {
        return;
    }

    auto gz = mvt::gzipCompress(*raw);
    check(gz.has_value(), "and gzips");
    if (!gz)
    {
        return;
    }
    check(gz->size() < raw->size(), "to something smaller");

    auto inflated = mvt::inflateIfCompressed(*gz);
    check(inflated.has_value(), "and inflates again");
    if (inflated)
    {
        check(*inflated == *raw, "back to exactly the same bytes");
        auto decoded = mvt::decode(*inflated);
        check(decoded.has_value() && decoded->layers.size() == 1,
              "and decodes to the tile that went in");
    }
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_an_empty_tile_round_trips();
    test_a_line_round_trips_with_its_attributes();
    test_the_cursor_persists_across_rings();
    test_a_polygon_closes_without_repeating_its_first_point();
    test_a_degenerate_ring_cannot_poison_the_tile();
    test_ring_winding_survives();
    test_a_non_default_extent_survives();
    test_negative_coordinates_survive();
    test_every_value_kind_survives();
    test_a_tag_index_past_the_table_is_refused();
    test_gzip_round_trips_through_the_decoder();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all vector tile encode checks passed");
    return 0;
}
