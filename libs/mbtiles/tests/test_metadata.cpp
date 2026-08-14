// SPDX-License-Identifier: GPL-3.0-or-later
//
// The metadata table, and the TileJSON built out of it.
//
// The mbtiles spec makes almost everything here optional, so the interesting
// cases are all absence and malformation rather than the happy path. The rule
// this file pins is that a field which did not parse stays ABSENT rather than
// becoming a default: an absent bounding box makes a client ask, a bounding box
// of [0,0,0,0] makes it fly the camera to null island and render nothing, which
// looks like a broken tile pipeline.

#include "mbtiles/archive.h"
#include "mbtiles/compression.h"
#include "mbtiles/metadata.h"

#include "archive_builder.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

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

using mbtiles_test::Builder;
using mbtiles_test::gzipBlob;
using mbtiles_test::TempDir;

// ============================================================================
// parseNumberList
// ============================================================================

void test_number_lists_parse_or_do_not()
{
    const auto bounds = mbtiles::parseNumberList("-121.44,32.34,-116.0,35.81", 4);
    check(bounds.has_value(), "a four-element bounds parses");
    if (bounds)
    {
        check(bounds->size() == 4, "it has four elements");
        check((*bounds)[0] < -121.4 && (*bounds)[0] > -121.5, "the west edge round-trips");
        check((*bounds)[3] > 35.8 && (*bounds)[3] < 35.82, "the north edge round-trips");
    }

    check(!mbtiles::parseNumberList("1,2,3", 4).has_value(),
          "three numbers is not a bounding box");
    check(!mbtiles::parseNumberList("1,2,3,4,5", 4).has_value(),
          "five numbers is not a bounding box");
    check(!mbtiles::parseNumberList("", 4).has_value(), "an empty string is not a bounding box");
    check(!mbtiles::parseNumberList("1,2,north,4", 4).has_value(),
          "a word among the numbers is refused");

    // strtod would happily read "32.3" out of "32.3deg" and leave the rest.
    // That is exactly the kind of half-read that produces a plausible wrong
    // map, so trailing junk is refused.
    check(!mbtiles::parseNumberList("1,2,3,4deg", 4).has_value(), "trailing junk is refused");
    check(!mbtiles::parseNumberList("1,2,3,", 4).has_value(), "a trailing comma is refused");
    check(!mbtiles::parseNumberList("1,2,3,nan", 4).has_value(), "NaN is refused");
    check(!mbtiles::parseNumberList("1,2,3,inf", 4).has_value(), "infinity is refused");

    // Whitespace after a number is how several writers format this.
    check(mbtiles::parseNumberList("1, 2, 3", 3).has_value(), "spaces after commas are fine");

    check(mbtiles::parseNumberList("-60.72,34.07,7", 3).has_value(), "a three-element center parses");
}

// ============================================================================
// Reading the table
// ============================================================================

void test_the_documented_fields_are_read()
{
    const TempDir dir("meta");
    const auto path = dir.file("meta.mbtiles");
    check(Builder(path)
              .meta("name", "Tilemaker to OpenMapTiles schema")
              .meta("format", "pbf")
              .meta("version", "3.0")
              .meta("description", "a description")
              .meta("attribution", "OpenStreetMap")
              .meta("type", "baselayer")
              .meta("minzoom", "0")
              .meta("maxzoom", "14")
              .meta("bounds", "-121.44,32.34,-116.0,35.81")
              .meta("center", "-118.7,34.07,7")
              .meta("json", R"({"vector_layers":[{"id":"water","fields":{}}]})")
              .meta("something_else", "kept")
              .tmsTile(1, 0, 0, gzipBlob(1))
              .build()
              .empty(),
          "built the metadata archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the metadata archive opens");
    if (!archive)
    {
        return;
    }

    const auto& meta = archive->metadata();
    check(meta.name == "Tilemaker to OpenMapTiles schema", "name is read");
    check(meta.format == "pbf", "format is read");
    check(meta.version == "3.0", "version is read");
    check(meta.attribution == "OpenStreetMap", "attribution is read");
    check(meta.type == "baselayer", "type is read");
    check(meta.minzoom == 0, "minzoom is read");
    check(meta.maxzoom == 14, "maxzoom is read");
    check(meta.bounds.size() == 4, "bounds is read");
    check(meta.center.size() == 3, "center is read");
    check(!meta.json.empty(), "json is read");

    // The spec explicitly allows rows nobody has heard of. Dropping them would
    // make this a lossy read of a format that promises it is not.
    const bool keptExtra =
        std::any_of(meta.extra.begin(), meta.extra.end(),
                    [](const auto& row) { return row.first == "something_else"; });
    check(keptExtra, "an unrecognised row is kept rather than dropped");
}

void test_absent_optional_fields_stay_absent()
{
    const TempDir dir("sparse_meta");
    const auto path = dir.file("sparse.mbtiles");
    check(Builder(path).meta("name", "bare").tmsTile(2, 0, 0, gzipBlob(1)).build().empty(),
          "built the bare archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "an archive with only `name` opens");
    if (!archive)
    {
        return;
    }

    const auto& meta = archive->metadata();
    check(meta.bounds.empty(), "an absent bounds is empty, not [0,0,0,0]");
    check(meta.center.empty(), "an absent center is empty, not null island");
    check(meta.format.empty(), "an absent format is empty rather than guessed");
}

void test_a_malformed_bounds_is_dropped_whole()
{
    const TempDir dir("badbounds");
    const auto path = dir.file("bad.mbtiles");
    check(Builder(path)
              .meta("name", "bad")
              .meta("bounds", "-121.44,32.34,-116.0")
              .meta("center", "not,a,center")
              .tmsTile(1, 0, 0, gzipBlob(1))
              .build()
              .empty(),
          "built the malformed-bounds archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "a malformed bounds does not stop the archive opening");
    if (!archive)
    {
        return;
    }

    check(archive->metadata().bounds.empty(), "a three-element bounds is dropped entirely");
    check(archive->metadata().center.empty(), "an unparseable center is dropped entirely");
}

void test_zoom_bounds_fall_back_to_the_tiles()
{
    const TempDir dir("zoomfallback");
    const auto path = dir.file("zoom.mbtiles");
    check(Builder(path)
              .meta("name", "zoom")
              .tmsTile(3, 0, 0, gzipBlob(1))
              .tmsTile(7, 0, 0, gzipBlob(2))
              .tmsTile(5, 0, 0, gzipBlob(3))
              .build()
              .empty(),
          "built the zoom-fallback archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "an archive without zoom metadata opens");
    if (!archive)
    {
        return;
    }

    check(archive->metadata().minzoom == 3, "minzoom comes from MIN(zoom_level)");
    check(archive->metadata().maxzoom == 7, "maxzoom comes from MAX(zoom_level)");
}

void test_a_nonsense_zoom_is_ignored_rather_than_believed()
{
    const TempDir dir("badzoom");
    const auto path = dir.file("badzoom.mbtiles");
    check(Builder(path)
              .meta("name", "badzoom")
              .meta("minzoom", "zero")
              .meta("maxzoom", "99")
              .tmsTile(2, 0, 0, gzipBlob(1))
              .tmsTile(4, 0, 0, gzipBlob(2))
              .build()
              .empty(),
          "built the nonsense-zoom archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "a nonsense zoom does not stop the archive opening");
    if (!archive)
    {
        return;
    }

    // Both rows are refused -- "zero" is not a number and 99 is past what a
    // tile pyramid can address -- so both fall back to the tiles.
    check(archive->metadata().minzoom == 2, "an unparseable minzoom falls back to the tiles");
    check(archive->metadata().maxzoom == 4, "an out-of-range maxzoom falls back to the tiles");
}

void test_inverted_zoom_bounds_are_swapped()
{
    // An archive claiming minzoom 14, maxzoom 0 would make every range check
    // downstream reject everything. Swapping is the only reading that leaves
    // it usable; refusing to open turns one bad row into no map at all.
    const TempDir dir("invzoom");
    const auto path = dir.file("inv.mbtiles");
    check(Builder(path)
              .meta("name", "inv")
              .meta("minzoom", "14")
              .meta("maxzoom", "2")
              .tmsTile(3, 0, 0, gzipBlob(1))
              .build()
              .empty(),
          "built the inverted-zoom archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "an inverted zoom range opens");
    if (!archive)
    {
        return;
    }

    check(archive->metadata().minzoom == 2, "the smaller of the two becomes minzoom");
    check(archive->metadata().maxzoom == 14, "the larger of the two becomes maxzoom");
}

// ============================================================================
// TileJSON
// ============================================================================

void test_tilejson_carries_what_a_style_needs()
{
    const TempDir dir("tilejson");
    const auto path = dir.file("tj.mbtiles");
    check(Builder(path)
              .meta("name", "socal")
              .meta("format", "pbf")
              .meta("minzoom", "0")
              .meta("maxzoom", "14")
              .meta("bounds", "-121.44,32.34,-116.0,35.81")
              .meta("center", "-118.7,34.07,7")
              .meta("attribution", "OpenStreetMap contributors")
              .meta("json", R"({"vector_layers":[{"id":"water","fields":{}},{"id":"road","fields":{}}]})")
              .tmsTile(1, 0, 0, gzipBlob(1))
              .build()
              .empty(),
          "built the tilejson archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the tilejson archive opens");
    if (!archive)
    {
        return;
    }

    const auto doc =
        nlohmann::json::parse(archive->tileJson("redline://tile/socal/{z}/{x}/{y}.pbf"), nullptr,
                              false);
    check(!doc.is_discarded() && doc.is_object(), "the TileJSON parses as an object");
    if (doc.is_discarded() || !doc.is_object())
    {
        return;
    }

    check(doc.value("tilejson", "") == "2.0.0", "it declares TileJSON 2.0.0");

    // xyz, not tms. The URL template hands the client slippy coordinates
    // because the flip already happened; saying "tms" here would have the
    // client flip them again.
    check(doc.value("scheme", "") == "xyz", "it declares the xyz scheme");

    check(doc.contains("tiles") && doc["tiles"].is_array() && doc["tiles"].size() == 1,
          "it carries exactly the tile URL it was given");
    if (doc.contains("tiles") && doc["tiles"].is_array() && !doc["tiles"].empty())
    {
        check(doc["tiles"][0] == "redline://tile/socal/{z}/{x}/{y}.pbf",
              "the tile URL template is passed through verbatim");
    }

    check(doc.value("minzoom", -1) == 0, "minzoom is carried");
    check(doc.value("maxzoom", -1) == 14, "maxzoom is carried");
    check(doc.contains("bounds") && doc["bounds"].size() == 4, "bounds is carried");
    check(doc.contains("center") && doc["center"].size() == 3, "center is carried");

    // THE field that makes a vector style work. Without vector_layers a client
    // loads the source, requests tiles, and draws nothing, because no layer in
    // the style matches a source-layer it knows about.
    check(doc.contains("vector_layers") && doc["vector_layers"].is_array() &&
              doc["vector_layers"].size() == 2,
          "vector_layers is merged in from the json column");
}

void test_a_malformed_json_column_does_not_take_the_map_down()
{
    const TempDir dir("badjson");
    const auto path = dir.file("badjson.mbtiles");
    check(Builder(path)
              .meta("name", "badjson")
              .meta("format", "pbf")
              .meta("json", "{not valid json at all")
              .tmsTile(1, 0, 0, gzipBlob(1))
              .build()
              .empty(),
          "built the malformed-json archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "a malformed json column does not stop the archive opening");
    if (!archive)
    {
        return;
    }

    const auto doc = nlohmann::json::parse(archive->tileJson("redline://tile/x/{z}/{x}/{y}.pbf"),
                                           nullptr, false);
    check(!doc.is_discarded() && doc.is_object(),
          "the TileJSON is still a valid document");
    check(doc.contains("tiles"), "the tile URL survives a malformed json column");
    check(!doc.contains("vector_layers"), "and no vector_layers is invented");
}

void test_absent_bounds_are_absent_from_the_tilejson_too()
{
    const TempDir dir("tjbare");
    const auto path = dir.file("bare.mbtiles");
    check(Builder(path).meta("name", "bare").tmsTile(1, 0, 0, gzipBlob(1)).build().empty(),
          "built the bare tilejson archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the bare tilejson archive opens");
    if (!archive)
    {
        return;
    }

    const auto doc = nlohmann::json::parse(archive->tileJson("redline://tile/x/{z}/{x}/{y}.pbf"),
                                           nullptr, false);
    check(!doc.contains("bounds"), "an absent bounds does not appear as [0,0,0,0]");
    check(!doc.contains("center"), "an absent center does not appear as null island");
}

// ============================================================================
// Compression sniffing
// ============================================================================

void test_container_magic_is_recognised()
{
    const std::vector<std::uint8_t> gzip { 0x1F, 0x8B, 0x08, 0x00 };
    const std::vector<std::uint8_t> zstd { 0x28, 0xB5, 0x2F, 0xFD, 0x00 };
    const std::vector<std::uint8_t> zlib { 0x78, 0x9C, 0x00, 0x00 };
    const std::vector<std::uint8_t> pbf { 0x1A, 0x09, 0x78, 0x02 };
    const std::vector<std::uint8_t> png { 0x89, 0x50, 0x4E, 0x47 };

    check(mbtiles::sniff(gzip) == mbtiles::Encoding::Gzip, "gzip magic is recognised");
    check(mbtiles::sniff(zstd) == mbtiles::Encoding::Zstd, "zstd magic is recognised");
    check(mbtiles::sniff(zlib) == mbtiles::Encoding::Deflate, "a zlib header is recognised");
    check(mbtiles::sniff(pbf) == mbtiles::Encoding::Identity, "raw protobuf is left alone");
    check(mbtiles::sniff(png) == mbtiles::Encoding::Identity, "a PNG is left alone");
    check(mbtiles::sniff({}) == mbtiles::Encoding::Identity, "an empty blob is left alone");

    // 0x78 begins plenty of legitimate protobuf, so the checksum rule is what
    // stops a raw vector tile being mistaken for a zlib stream. 0x78 0x00 is
    // not a multiple of 31.
    const std::vector<std::uint8_t> notZlib { 0x78, 0x00, 0x00, 0x00 };
    check(mbtiles::sniff(notZlib) == mbtiles::Encoding::Identity,
          "a 0x78 that fails the zlib checksum is not deflate");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_number_lists_parse_or_do_not();

    test_the_documented_fields_are_read();
    test_absent_optional_fields_stay_absent();
    test_a_malformed_bounds_is_dropped_whole();
    test_zoom_bounds_fall_back_to_the_tiles();
    test_a_nonsense_zoom_is_ignored_rather_than_believed();
    test_inverted_zoom_bounds_are_swapped();

    test_tilejson_carries_what_a_style_needs();
    test_a_malformed_json_column_does_not_take_the_map_down();
    test_absent_bounds_are_absent_from_the_tilejson_too();

    test_container_magic_is_recognised();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all mbtiles metadata checks passed");
    return 0;
}
