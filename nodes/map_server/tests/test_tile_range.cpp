// SPDX-License-Identifier: GPL-3.0-or-later
//
// Which tile coordinates an archive could hold, and why not when it could not.
//
// Three lines of comparisons, and a `unit` test for them because the client's
// whole zoom behaviour hangs off telling the answers apart. Getting notFound
// and outOfRange the wrong way round does not fail loudly: the map simply goes
// blank the moment anyone zooms past the archive, or -- worse the other way --
// a client retries an uncovered coordinate a level up, and again, and again,
// all the way to zero, once per tile, over ground that has nothing on it.

#include "tilesets.h"

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

using map_server::checkTileRange;
using map_server::TileRangeCheck;

mbtiles::Metadata range(std::uint8_t minzoom, std::uint8_t maxzoom)
{
    mbtiles::Metadata meta;
    meta.minzoom = minzoom;
    meta.maxzoom = maxzoom;
    return meta;
}

void test_a_level_the_archive_has_is_in_range()
{
    const mbtiles::Metadata meta = range(0, 14);

    check(checkTileRange(meta, 14, 2828, 6562) == TileRangeCheck::InRange,
          "the archive's deepest level is in range");
    check(checkTileRange(meta, 0, 0, 0) == TileRangeCheck::InRange,
          "and so is its shallowest");
    check(checkTileRange(meta, 7, 20, 50) == TileRangeCheck::InRange,
          "and so is one in the middle");

    // The point of InRange: it says the archive COVERS that level, not that
    // there is anything there. Most of the pyramid is empty and that miss is
    // the archive's answer to give, as notFound, and it is final.
    check(checkTileRange(meta, 14, 0, 0) == TileRangeCheck::InRange,
          "a covered level over ground the archive has nothing on is still in range -- "
          "absence is the archive's answer, not this function's");
}

void test_a_level_the_archive_lacks_is_out_of_range()
{
    const mbtiles::Metadata meta = range(6, 14);

    check(checkTileRange(meta, 15, 100, 100) == TileRangeCheck::OutOfRange,
          "one level past maxzoom is out of range");
    check(checkTileRange(meta, 22, 100, 100) == TileRangeCheck::OutOfRange,
          "and so is the deepest coordinate there is");
    check(checkTileRange(meta, 5, 10, 10) == TileRangeCheck::OutOfRange,
          "and so is one level shallower than minzoom");

    // Both edges included, or the client loses a level at each end of every
    // archive and never finds out.
    check(checkTileRange(meta, 14, 100, 100) == TileRangeCheck::InRange,
          "maxzoom itself is IN range");
    check(checkTileRange(meta, 6, 10, 10) == TileRangeCheck::InRange,
          "and so is minzoom itself");
}

void test_a_coordinate_that_is_not_a_tile_is_a_bad_request()
{
    const mbtiles::Metadata meta = range(0, 22);

    // NOT OutOfRange, even though it is numerically past everything. A client
    // told "out of range" retries a level up; a client told "bad request" has
    // a bug and should hear about it.
    check(checkTileRange(meta, 23, 0, 0) == TileRangeCheck::BadRequest,
          "a zoom past the projection is a bad request, not an out-of-range one");

    // x/y outside 2^z. Screened BEFORE the shift that computes 2^z, because at
    // z=40 that shift is undefined behaviour rather than a large number.
    check(checkTileRange(meta, 1, 2, 0) == TileRangeCheck::BadRequest,
          "an x outside 2^z is a bad request");
    check(checkTileRange(meta, 1, 0, 2) == TileRangeCheck::BadRequest,
          "and so is a y");
    check(checkTileRange(meta, 1, 1, 1) == TileRangeCheck::InRange,
          "while the last valid tile of a level is fine");

    check(checkTileRange(meta, 200, 0, 0) == TileRangeCheck::BadRequest,
          "a wildly out of range zoom is caught before anything shifts by it");
}

void test_an_archive_of_one_level_still_answers()
{
    // A single-level archive is a real thing -- an overview tileset built at
    // one zoom -- and an off-by-one at either edge would make it answer
    // nothing at all.
    const mbtiles::Metadata meta = range(10, 10);

    check(checkTileRange(meta, 10, 5, 5) == TileRangeCheck::InRange, "its one level is in range");
    check(checkTileRange(meta, 9, 5, 5) == TileRangeCheck::OutOfRange, "one above is not");
    check(checkTileRange(meta, 11, 5, 5) == TileRangeCheck::OutOfRange, "one below is not");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_level_the_archive_has_is_in_range();
    test_a_level_the_archive_lacks_is_out_of_range();
    test_a_coordinate_that_is_not_a_tile_is_a_bad_request();
    test_an_archive_of_one_level_still_answers();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all checks passed");
    return 0;
}
