// SPDX-License-Identifier: GPL-3.0-or-later
//
// Decoding a vector tile.
//
// Every case here is a way the format is easy to get subtly wrong, and what
// they have in common is that getting them wrong does not fail -- it renders.
// A ring that winds the other way fills a lake solid. An extent assumed to be
// 4096 draws one layer at the wrong scale. A geometry cursor reset per command
// collapses every road onto the tile's corner. None of those throw, none of
// them log, and all of them look like a styling problem.
//
// tile_builder.h encodes longhand from the spec rather than through the
// decoder's own varint and zigzag routines, so a test cannot agree with the bug
// it is checking for. tests/test_real_tiles.cpp is the other half of that
// argument.

#include "mvt/decode.h"
#include "mvt/gzip.h"

#include "tile_builder.h"

#include <spdlog/spdlog.h>

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

using mvt_test::FeatureSpec;
using mvt_test::Geometry;
using mvt_test::LayerBuilder;
using mvt_test::tileOf;

// ============================================================================
// Structure
// ============================================================================

void test_an_empty_tile_is_empty_not_an_error()
{
    // Real archives are full of these. The SoCal file has thousands of 23-byte
    // tiles, and a decoder that rejected an empty one would drop them all.
    auto tile = mvt::decode({});
    check(tile.has_value(), "an empty buffer decodes");
    check(tile.has_value() && tile->layers.empty(), "to a tile with no layers");
}

void test_layers_keys_values_and_features_round_trip()
{
    const auto bytes = tileOf({ LayerBuilder("transportation")
                                    .extent(4096)
                                    .key("class")
                                    .key("oneway")
                                    .stringValue("motorway")
                                    .boolValue(true)
                                    .feature(FeatureSpec {
                                        .type = 2,
                                        .geometry = Geometry().moveTo({ { 10, 20 } })
                                                        .lineTo({ { 30, 40 } })
                                                        .bytes(),
                                        .tags = { 0, 0, 1, 1 },
                                        .id = 7,
                                        .hasId = true,
                                    })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "the tile decodes");
    if (!tile)
    {
        SPDLOG_ERROR("  {}", mvt::to_string(tile.error()));
        return;
    }

    check(tile->layers.size() == 1, "one layer");
    if (tile->layers.empty())
    {
        return;
    }

    const mvt::Layer& layer = tile->layers[0];
    check(layer.name == "transportation", "the layer name");
    check(layer.extent == 4096, "the extent");
    check(layer.features.size() == 1, "one feature");
    if (layer.features.empty())
    {
        return;
    }

    const mvt::Feature& feature = layer.features[0];
    check(feature.id == 7 && feature.hasId, "the feature id");
    check(feature.type == mvt::GeomType::LineString, "the geometry type");
    check(layer.attributeText(feature, "class") == "motorway", "a string attribute resolves");
    check(layer.attributeText(feature, "oneway") == "true", "a bool attribute resolves");
    check(!layer.attribute(feature, "no_such_key").has_value(),
          "a key the feature does not carry resolves to nothing");
    check(tile->layer("transportation") != nullptr, "the layer is findable by name");
    check(tile->layer("nope") == nullptr, "and an absent one is not");
}

void test_the_value_alternatives_decode()
{
    const auto bytes = tileOf({ LayerBuilder("v")
                                    .key("s")
                                    .key("i")
                                    .key("d")
                                    .key("b")
                                    .stringValue("text")
                                    .sintValue(-42)
                                    .doubleValue(1.5)
                                    .boolValue(false)
                                    .feature(FeatureSpec {
                                        .type = 1,
                                        .geometry = Geometry().moveTo({ { 1, 1 } }).bytes(),
                                        .tags = { 0, 0, 1, 1, 2, 2, 3, 3 },
                                    })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "the value tile decodes");
    if (!tile || tile->layers.empty() || tile->layers[0].features.empty())
    {
        return;
    }

    const mvt::Layer& layer = tile->layers[0];
    const mvt::Feature& feature = layer.features[0];

    check(layer.attributeText(feature, "s") == "text", "a string value");
    const auto integer = layer.attribute(feature, "i");
    check(integer && std::holds_alternative<std::int64_t>(*integer) &&
              std::get<std::int64_t>(*integer) == -42,
          "a NEGATIVE sint64 value, which is where zigzag goes wrong");
    const auto number = layer.attribute(feature, "d");
    check(number && std::holds_alternative<double>(*number) &&
              std::get<double>(*number) > 1.49 && std::get<double>(*number) < 1.51,
          "a double value");
    const auto flag = layer.attribute(feature, "b");
    check(flag && std::holds_alternative<bool>(*flag) && !std::get<bool>(*flag),
          "a false bool, which must not read as absent");
}

void test_unknown_fields_are_skipped_not_rejected()
{
    // The forward-compatibility rule in the spec. A decoder that errors here
    // breaks on the next revision of a format it would otherwise read fine.
    const auto bytes = tileOf({ LayerBuilder("l")
                                    .unknownField()
                                    .feature(FeatureSpec {
                                        .type = 1,
                                        .geometry = Geometry().moveTo({ { 5, 5 } }).bytes(),
                                    })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "a layer with an unknown field still decodes");
    check(tile && !tile->layers.empty() && tile->layers[0].features.size() == 1,
          "and the features around it survive");
}

void test_unpacked_tags_decode_too()
{
    // Packed is what every encoder emits, but the proto allows one varint per
    // field and the wire type says which is present. Assuming packed silently
    // loses every tag on a tile written the other way.
    const auto bytes = tileOf({ LayerBuilder("l")
                                    .key("class")
                                    .stringValue("river")
                                    .feature(FeatureSpec {
                                        .type = 2,
                                        .geometry = Geometry().moveTo({ { 0, 0 } })
                                                        .lineTo({ { 4, 4 } })
                                                        .bytes(),
                                        .tags = { 0, 0 },
                                        .unpackedTags = true,
                                    })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "unpacked tags decode");
    if (tile && !tile->layers.empty() && !tile->layers[0].features.empty())
    {
        check(tile->layers[0].attributeText(tile->layers[0].features[0], "class") == "river",
              "and resolve to the same attribute");
    }
}

// ============================================================================
// Extent
// ============================================================================

void test_an_absent_extent_defaults_to_4096()
{
    const auto bytes = tileOf({ LayerBuilder("l")
                                    .withoutExtent()
                                    .feature(FeatureSpec {
                                        .type = 1,
                                        .geometry = Geometry().moveTo({ { 1, 1 } }).bytes(),
                                    })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "a layer with no extent field decodes");
    check(tile && !tile->layers.empty() && tile->layers[0].extent == 4096,
          "and takes the proto default of 4096 rather than zero");
}

void test_a_non_default_extent_is_carried()
{
    // Extent is per LAYER. A renderer that hardcodes 4096 works until it meets
    // a tile that does not, and then draws that one layer at the wrong scale --
    // roads that miss their own buildings by a factor of four.
    const auto bytes = tileOf({ LayerBuilder("a").extent(4096)
                                    .feature(FeatureSpec { .type = 1,
                                                           .geometry = Geometry().moveTo({ { 1, 1 } }).bytes() })
                                    .bytes(),
                                LayerBuilder("b").extent(512)
                                    .feature(FeatureSpec { .type = 1,
                                                           .geometry = Geometry().moveTo({ { 1, 1 } }).bytes() })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "a tile with two extents decodes");
    if (tile && tile->layers.size() == 2)
    {
        check(tile->layers[0].extent == 4096, "the first layer keeps its extent");
        check(tile->layers[1].extent == 512, "and the second keeps its own, different one");
    }
}

void test_a_zero_extent_is_refused()
{
    // Extent is the divisor in every coordinate transform. Zero would be a
    // division by zero at paint time, several layers away from the cause.
    const auto bytes = tileOf({ LayerBuilder("l").extent(0).bytes() });
    check(!mvt::decode(bytes).has_value(), "a zero extent is refused at decode time");
}

// ============================================================================
// Geometry
// ============================================================================

void test_coordinates_are_absolute_after_delta_decoding()
{
    // The cursor persists ACROSS commands. A decoder that resets it per command
    // collapses every part of every feature onto the tile origin.
    const auto bytes =
        tileOf({ LayerBuilder("l")
                     .feature(FeatureSpec {
                         .type = 2,
                         .geometry = Geometry().moveTo({ { 100, 200 } })
                                         .lineTo({ { 150, 200 }, { 150, 260 } })
                                         .bytes(),
                     })
                     .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "the delta tile decodes");
    if (!tile || tile->layers.empty() || tile->layers[0].features.empty())
    {
        return;
    }

    const auto& rings = tile->layers[0].features[0].rings;
    check(rings.size() == 1, "one part");
    if (rings.empty())
    {
        return;
    }

    const std::vector<mvt::Point> expected { { 100, 200 }, { 150, 200 }, { 150, 260 } };
    check(rings[0] == expected, "the points come back at their absolute positions");
}

void test_coordinates_outside_the_extent_survive()
{
    // Tiles carry a buffer so a line crossing the edge joins up with its
    // neighbour. Clamping or rejecting those points puts a visible seam down
    // every tile boundary, which reads as a rendering bug rather than a decode
    // one.
    const auto bytes = tileOf({ LayerBuilder("l").extent(4096)
                                    .feature(FeatureSpec {
                                        .type = 2,
                                        .geometry = Geometry().moveTo({ { -256, -300 } })
                                                        .lineTo({ { 4352, 5000 } })
                                                        .bytes(),
                                    })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "a tile with buffered geometry decodes");
    if (!tile || tile->layers.empty() || tile->layers[0].features.empty())
    {
        return;
    }

    const auto& rings = tile->layers[0].features[0].rings;
    check(!rings.empty() && rings[0].size() == 2, "both points survive");
    if (!rings.empty() && rings[0].size() == 2)
    {
        check(rings[0][0].x == -256 && rings[0][0].y == -300,
              "a negative coordinate is kept, not clamped to zero");
        check(rings[0][1].x == 4352 && rings[0][1].y == 5000,
              "a coordinate past the extent is kept, not clamped to it");
    }
}

void test_a_multipoint_moveto_makes_one_part_per_point()
{
    const auto bytes =
        tileOf({ LayerBuilder("l")
                     .feature(FeatureSpec {
                         .type = 1,
                         .geometry = Geometry().moveTo({ { 10, 10 }, { 20, 20 }, { 30, 30 } })
                                         .bytes(),
                     })
                     .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "a multipoint decodes");
    if (tile && !tile->layers.empty() && !tile->layers[0].features.empty())
    {
        check(tile->layers[0].features[0].rings.size() == 3,
              "a MoveTo of three points is three parts, not one part of three");
    }
}

void test_a_multi_part_linestring_splits_on_moveto()
{
    const auto bytes = tileOf({ LayerBuilder("l")
                                    .feature(FeatureSpec {
                                        .type = 2,
                                        .geometry = Geometry().moveTo({ { 0, 0 } })
                                                        .lineTo({ { 10, 0 } })
                                                        .moveTo({ { 50, 50 } })
                                                        .lineTo({ { 60, 50 } })
                                                        .bytes(),
                                    })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "a two-part linestring decodes");
    if (tile && !tile->layers.empty() && !tile->layers[0].features.empty())
    {
        const auto& rings = tile->layers[0].features[0].rings;
        check(rings.size() == 2, "two parts");
        if (rings.size() == 2)
        {
            check(rings[1][0] == mvt::Point { 50, 50 },
                  "and the second part's delta is from the first part's END, not from zero");
        }
    }
}

// ============================================================================
// Winding
// ============================================================================

void test_ring_winding_distinguishes_a_hole_from_an_island()
{
    // THE polygon trap. In MVT's coordinate system -- y increasing downward --
    // a positive signed area is an exterior ring and a negative one is a hole.
    // Ignore it and every lake with an island in it fills solid, and every
    // courtyard disappears.
    //
    // Clockwise in screen coordinates (y down) gives a positive area: the
    // square (0,0) (10,0) (10,10) (0,10) below.
    const std::vector<mvt::Point> clockwise { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
    const std::vector<mvt::Point> counterClockwise { { 0, 0 }, { 0, 10 }, { 10, 10 }, { 10, 0 } };

    check(mvt::signedArea2(clockwise) > 0, "a clockwise ring has positive area");
    check(mvt::signedArea2(counterClockwise) < 0, "a counter-clockwise ring has negative area");
    check(mvt::isExteriorRing(clockwise), "so clockwise is the exterior");
    check(!mvt::isExteriorRing(counterClockwise), "and counter-clockwise is a hole");

    // Degenerate rings have no orientation and must not claim one.
    check(mvt::signedArea2({}) == 0, "an empty ring has no area");
    check(mvt::signedArea2({ { 0, 0 }, { 1, 1 } }) == 0, "a two-point ring has no area");
    check(!mvt::isExteriorRing({ { 0, 0 }, { 1, 1 } }), "and is not an exterior ring");

    // The area is exact integer arithmetic, so a ring far outside the tile --
    // legal, because of the buffer -- still gets the right sign rather than
    // losing it to floating point.
    const std::vector<mvt::Point> farAway { { 1'000'000, 1'000'000 },
                                            { 1'000'010, 1'000'000 },
                                            { 1'000'010, 1'000'010 },
                                            { 1'000'000, 1'000'010 } };
    check(mvt::signedArea2(farAway) > 0, "a ring far from the origin keeps its orientation");
}

void test_a_polygon_with_a_hole_decodes_as_two_rings()
{
    Geometry geometry;
    geometry.moveTo({ { 0, 0 } })
        .lineTo({ { 100, 0 }, { 100, 100 }, { 0, 100 } })
        .closePath()
        // The hole, wound the other way.
        .moveTo({ { 25, 25 } })
        .lineTo({ { 25, 75 }, { 75, 75 }, { 75, 25 } })
        .closePath();

    const auto bytes = tileOf({ LayerBuilder("l")
                                    .feature(FeatureSpec { .type = 3, .geometry = geometry.bytes() })
                                    .bytes() });

    auto tile = mvt::decode(bytes);
    check(tile.has_value(), "a polygon with a hole decodes");
    if (!tile)
    {
        SPDLOG_ERROR("  {}", mvt::to_string(tile.error()));
        return;
    }
    if (tile->layers.empty() || tile->layers[0].features.empty())
    {
        return;
    }

    const auto& rings = tile->layers[0].features[0].rings;
    check(rings.size() == 2, "two rings");
    if (rings.size() != 2)
    {
        return;
    }

    check(mvt::isExteriorRing(rings[0]), "the first is the exterior");
    check(!mvt::isExteriorRing(rings[1]), "the second is the hole");
    check(rings[0].size() == 4, "the exterior has four points, not five");
    check(rings[0].front() != rings[0].back(),
          "ClosePath does NOT repeat the first point -- a renderer that assumes it does "
          "draws a zero-length final segment");
}

// ============================================================================
// Malformed input
// ============================================================================

void test_malformed_geometry_is_refused()
{
    const auto tileWith = [](const mvt_test::Bytes& geometry, std::uint32_t type = 2) {
        return tileOf({ LayerBuilder("l")
                            .feature(FeatureSpec { .type = type, .geometry = geometry })
                            .bytes() });
    };

    // A command id that is not MoveTo, LineTo or ClosePath.
    check(!mvt::decode(tileWith(Geometry().rawCommand(3, 1).rawDelta(1, 1).bytes())).has_value(),
          "an unknown geometry command is refused");

    // ClosePath's count is fixed at 1 by the spec.
    check(!mvt::decode(tileWith(Geometry().moveTo({ { 0, 0 } }).rawCommand(7, 2).bytes()))
               .has_value(),
          "ClosePath with a count other than 1 is refused");

    // ClosePath with nothing open.
    check(!mvt::decode(tileWith(Geometry().rawCommand(7, 1).bytes())).has_value(),
          "ClosePath with no open ring is refused");

    // A count of zero would loop forever in a naive decoder.
    check(!mvt::decode(tileWith(Geometry().rawCommand(1, 0).bytes())).has_value(),
          "a geometry command with a zero count is refused");

    // Parameters cut short: the count says two points, the bytes have one.
    check(!mvt::decode(tileWith(Geometry().rawCommand(1, 2).rawDelta(5, 5).bytes())).has_value(),
          "a command whose parameters are cut short is refused");
}

void test_a_polygon_ring_that_is_too_small_is_refused()
{
    // Two points cannot bound an area. Drawing it fills to somewhere arbitrary.
    Geometry geometry;
    geometry.moveTo({ { 0, 0 } }).lineTo({ { 10, 10 } }).closePath();

    const auto bytes = tileOf({ LayerBuilder("l")
                                    .feature(FeatureSpec { .type = 3, .geometry = geometry.bytes() })
                                    .bytes() });
    check(!mvt::decode(bytes).has_value(), "a two-point polygon ring is refused");
}

void test_out_of_range_tag_indices_are_refused()
{
    // An out-of-range index resolved at paint time is an out-of-bounds read in
    // the hot path, so it is checked once at decode.
    const auto badKey = tileOf({ LayerBuilder("l")
                                     .key("class")
                                     .stringValue("road")
                                     .feature(FeatureSpec {
                                         .type = 1,
                                         .geometry = Geometry().moveTo({ { 1, 1 } }).bytes(),
                                         .tags = { 5, 0 },
                                     })
                                     .bytes() });
    check(!mvt::decode(badKey).has_value(), "a tag key index past the key table is refused");

    const auto badValue = tileOf({ LayerBuilder("l")
                                       .key("class")
                                       .stringValue("road")
                                       .feature(FeatureSpec {
                                           .type = 1,
                                           .geometry = Geometry().moveTo({ { 1, 1 } }).bytes(),
                                           .tags = { 0, 9 },
                                       })
                                       .bytes() });
    check(!mvt::decode(badValue).has_value(), "a tag value index past the value table is refused");
}

void test_an_odd_tag_count_is_refused()
{
    // Tags are (key, value) pairs. Pairing up regardless would attach every
    // later attribute to the wrong key -- a park that reads as a motorway.
    const auto bytes = tileOf({ LayerBuilder("l")
                                    .key("class")
                                    .stringValue("road")
                                    .feature(FeatureSpec {
                                        .type = 1,
                                        .geometry = Geometry().moveTo({ { 1, 1 } }).bytes(),
                                        .tags = { 0 },
                                    })
                                    .bytes() });
    check(!mvt::decode(bytes).has_value(), "an odd number of tags is refused");
}

void test_a_truncated_tile_is_refused()
{
    const auto full = tileOf({ LayerBuilder("transportation")
                                   .key("class")
                                   .stringValue("motorway")
                                   .feature(FeatureSpec {
                                       .type = 2,
                                       .geometry = Geometry().moveTo({ { 0, 0 } })
                                                       .lineTo({ { 100, 100 } })
                                                       .bytes(),
                                       .tags = { 0, 0 },
                                   })
                                   .bytes() });

    check(mvt::decode(full).has_value(), "the full tile decodes");

    // Every proper prefix must be refused rather than half-decoded. A tile cut
    // by a short read would otherwise come back with some of its roads.
    int accepted = 0;
    for (std::size_t cut = 1; cut < full.size(); ++cut)
    {
        const std::vector<std::uint8_t> shortened(full.begin(),
                                                  full.begin() + static_cast<std::ptrdiff_t>(cut));
        if (mvt::decode(shortened).has_value())
        {
            ++accepted;
        }
    }
    check(accepted == 0, "no proper prefix of the tile decodes as valid");
}

void test_a_still_compressed_tile_says_so()
{
    // 0x1F 0x8B is protobuf field 3, wire type 7 -- so without this check the
    // error names a wire type and sends the reader to the decoder rather than
    // to the missing inflate.
    const std::vector<std::uint8_t> gzipped { 0x1F, 0x8B, 0x08, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0x00, 0x03 };
    auto tile = mvt::decode(gzipped);
    check(!tile.has_value(), "a gzipped buffer is refused");
    if (!tile)
    {
        check(tile.error().message.find("compressed") != std::string::npos,
              "and the message says it is still compressed");
    }
    check(mvt::looksCompressed(gzipped), "gzip is recognised");
    check(!mvt::looksCompressed({}), "an empty buffer is not compressed");
}

// ============================================================================
// Inflate
// ============================================================================

void test_inflate_round_trips_and_refuses_rubbish()
{
    // A gzip stream of "hello", produced by gzip(1) and pasted here rather than
    // generated with zlib -- for the same reason the tile builder encodes
    // longhand. Header, deflate block, CRC32, length.
    const std::vector<std::uint8_t> hello {
        0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0xCB, 0x48,
        0xCD, 0xC9, 0xC9, 0x07, 0x00, 0x86, 0xA6, 0x10, 0x36, 0x05, 0x00, 0x00, 0x00
    };

    auto inflated = mvt::inflate(hello);
    check(inflated.has_value(), "a gzip stream inflates");
    if (inflated)
    {
        const std::string text(inflated->begin(), inflated->end());
        check(text == "hello", "to the bytes it was made from");
    }

    check(!mvt::inflate({}).has_value(), "an empty buffer does not inflate");

    // Truncated: the header is intact and the data is not. zlib reports
    // Z_BUF_ERROR with nothing produced, which a naive loop spins on forever.
    const std::vector<std::uint8_t> cut(hello.begin(), hello.begin() + 14);
    check(!mvt::inflate(cut).has_value(), "a truncated stream is refused rather than looped on");

    // Uncompressed input is an error, not a pass-through: silently returning it
    // would make a corrupt tile and a plain tile indistinguishable here.
    const std::vector<std::uint8_t> plain { 0x1A, 0x02, 0x03 };
    check(!mvt::inflate(plain).has_value(), "uncompressed input is refused by inflate()");
    check(mvt::inflateIfCompressed(plain).has_value(),
          "but inflateIfCompressed passes it through");

    auto passed = mvt::inflateIfCompressed(plain);
    check(passed && *passed == plain, "unchanged");

    auto sniffed = mvt::inflateIfCompressed(hello);
    check(sniffed && std::string(sniffed->begin(), sniffed->end()) == "hello",
          "and inflates the compressed one");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_an_empty_tile_is_empty_not_an_error();
    test_layers_keys_values_and_features_round_trip();
    test_the_value_alternatives_decode();
    test_unknown_fields_are_skipped_not_rejected();
    test_unpacked_tags_decode_too();

    test_an_absent_extent_defaults_to_4096();
    test_a_non_default_extent_is_carried();
    test_a_zero_extent_is_refused();

    test_coordinates_are_absolute_after_delta_decoding();
    test_coordinates_outside_the_extent_survive();
    test_a_multipoint_moveto_makes_one_part_per_point();
    test_a_multi_part_linestring_splits_on_moveto();

    test_ring_winding_distinguishes_a_hole_from_an_island();
    test_a_polygon_with_a_hole_decodes_as_two_rings();

    test_malformed_geometry_is_refused();
    test_a_polygon_ring_that_is_too_small_is_refused();
    test_out_of_range_tag_indices_are_refused();
    test_an_odd_tag_count_is_refused();
    test_a_truncated_tile_is_refused();
    test_a_still_compressed_tile_says_so();

    test_inflate_round_trips_and_refuses_rubbish();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all vector tile decode checks passed");
    return 0;
}
