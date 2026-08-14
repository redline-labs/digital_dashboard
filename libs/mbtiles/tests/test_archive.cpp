// SPDX-License-Identifier: GPL-3.0-or-later
//
// Reading tiles out of an .mbtiles archive.
//
// The centrepiece is test_xyz_requests_read_tms_rows(). Every other bug in this
// library announces itself -- a bad path fails to open, a broken query returns
// an error. A wrong row flip returns a real tile, from the wrong hemisphere,
// and the map renders perfectly. The only way to catch it is to write the
// archive in TMS and ask in XYZ, which is what the builder exists for: if both
// sides of a test used the same helper to convert, the test would agree with
// the bug.
//
// Mutation check for that test: invert the flip in src/archive.cpp
// (`side - 1U - y` -> `y`) and it must fail.

#include "mbtiles/archive.h"

#include "archive_builder.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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
using mbtiles_test::Layout;
using mbtiles_test::TempDir;

// ============================================================================
// Opening
// ============================================================================

void test_a_missing_file_is_not_found()
{
    const TempDir dir("open");
    auto archive = mbtiles::Archive::open(dir.file("nope.mbtiles"));
    check(!archive.has_value(), "a missing archive does not open");
    if (!archive)
    {
        check(archive.error().kind == mbtiles::Error::Kind::NotFound,
              "a missing archive reports NotFound");
    }
}

void test_a_missing_file_is_not_silently_created()
{
    // SQLITE_OPEN_CREATE is what makes this worth a test. With it, a typo in a
    // config path produces an empty database, an archive that opens, and a map
    // that is blank forever with nothing logged.
    const TempDir dir("nocreate");
    const auto path = dir.file("nope.mbtiles");
    (void)mbtiles::Archive::open(path);
    check(!std::filesystem::exists(path), "opening a missing archive creates no file");
}

void test_a_directory_is_not_an_archive()
{
    const TempDir dir("isdir");
    auto archive = mbtiles::Archive::open(dir.path());
    check(!archive.has_value(), "a directory does not open as an archive");
}

void test_a_non_sqlite_file_is_rejected()
{
    const TempDir dir("notsqlite");
    const auto path = dir.file("junk.mbtiles");
    {
        std::ofstream out(path, std::ios::binary);
        out << "this is not a database, it is a sentence";
    }

    auto archive = mbtiles::Archive::open(path);
    check(!archive.has_value(), "a text file does not open as an archive");
}

void test_a_database_without_tiles_is_rejected()
{
    const TempDir dir("notiles");
    const auto path = dir.file("empty.mbtiles");
    check(Builder(path).withoutTiles().meta("name", "x").build().empty(), "built a tileless db");

    auto archive = mbtiles::Archive::open(path);
    check(!archive.has_value(), "a database with no tiles does not open");
    if (!archive)
    {
        check(archive.error().kind == mbtiles::Error::Kind::NotAnArchive,
              "a database with no tiles reports NotAnArchive");
    }
}

void test_a_database_without_metadata_is_rejected()
{
    const TempDir dir("nometa");
    const auto path = dir.file("nometa.mbtiles");
    check(Builder(path).withoutMetadata().build().empty(), "built a metadata-less db");

    auto archive = mbtiles::Archive::open(path);
    check(!archive.has_value(), "a database with no metadata table does not open");
}

// ============================================================================
// The flip
// ============================================================================

void test_xyz_requests_read_tms_rows()
{
    const TempDir dir("flip");
    const auto path = dir.file("flip.mbtiles");

    // One tile per zoom, each at TMS row 0 -- the SOUTHERNMOST row. In XYZ that
    // is the LAST row, y = 2^z - 1.
    Builder builder(path);
    builder.meta("name", "flip").meta("format", "pbf");
    for (int z = 1; z <= 6; ++z)
    {
        builder.tmsTile(z, 0, 0, gzipBlob(static_cast<std::uint8_t>(z)));
    }
    check(builder.build().empty(), "built the flip archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the flip archive opens");
    if (!archive)
    {
        return;
    }

    for (std::uint8_t z = 1; z <= 6; ++z)
    {
        const std::uint32_t southern = (1U << z) - 1U;

        auto found = archive->tile(z, 0, southern);
        check(found.has_value() && found->has_value(),
              "z" + std::to_string(z) + ": TMS row 0 is XYZ y=" + std::to_string(southern));
        if (found && *found)
        {
            check((*found)->data.back() == z,
                  "z" + std::to_string(z) + ": the tile returned is the one stored");
        }

        // And the un-flipped coordinate must find NOTHING. Without this half,
        // a flip that is a no-op still passes: TMS row 0 and XYZ y 0 coincide
        // at z=0, and a test that only checked the positive case at low zoom
        // would be satisfied by the bug.
        auto absent = archive->tile(z, 0, 0);
        check(absent.has_value() && !absent->has_value(),
              "z" + std::to_string(z) + ": XYZ y=0 is the northern row and is empty");
    }
}

void test_the_flip_is_symmetric_across_the_pyramid()
{
    // A tile at the northern edge, to catch a flip that is off by one in the
    // other direction. TMS row 2^z-1 is XYZ y=0.
    const TempDir dir("flipnorth");
    const auto path = dir.file("north.mbtiles");

    constexpr std::uint8_t kZ = 5;
    const std::uint32_t topTmsRow = (1U << kZ) - 1U;
    check(Builder(path)
              .meta("name", "north")
              .tmsTile(kZ, 3, static_cast<int>(topTmsRow), gzipBlob(0xAA))
              .build()
              .empty(),
          "built the northern archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the northern archive opens");
    if (!archive)
    {
        return;
    }

    auto found = archive->tile(kZ, 3, 0);
    check(found.has_value() && found->has_value(), "the northernmost TMS row is XYZ y=0");

    auto absent = archive->tile(kZ, 3, topTmsRow);
    check(absent.has_value() && !absent->has_value(),
          "XYZ y=2^z-1 is the southern row and is empty");
}

// ============================================================================
// Missing tiles and bad coordinates
// ============================================================================

void test_a_missing_tile_is_empty_not_an_error()
{
    // Most of the pyramid is empty in any real archive, so this is the common
    // path, not the exceptional one. Reporting it as an error would have the
    // client log and retry for every tile of ocean.
    const TempDir dir("missing");
    const auto path = dir.file("sparse.mbtiles");
    check(Builder(path).meta("name", "sparse").tmsTile(4, 1, 1, gzipBlob(1)).build().empty(),
          "built the sparse archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the sparse archive opens");
    if (!archive)
    {
        return;
    }

    auto tile = archive->tile(4, 9, 9);
    check(tile.has_value(), "a missing tile is not an error");
    check(tile.has_value() && !tile->has_value(), "a missing tile is an empty optional");
}

void test_coordinates_outside_the_pyramid_are_refused()
{
    const TempDir dir("range");
    const auto path = dir.file("range.mbtiles");
    check(Builder(path).meta("name", "range").tmsTile(2, 0, 0, gzipBlob(1)).build().empty(),
          "built the range archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the range archive opens");
    if (!archive)
    {
        return;
    }

    // At z=2 there are 4x4 tiles, so 4 is one past the end in both axes.
    auto badX = archive->tile(2, 4, 0);
    check(!badX.has_value(), "x = 2^z is refused");
    if (!badX)
    {
        check(badX.error().kind == mbtiles::Error::Kind::InvalidArgument,
              "an out-of-range x reports InvalidArgument");
    }

    auto badY = archive->tile(2, 0, 4);
    check(!badY.has_value(), "y = 2^z is refused");

    auto badZ = archive->tile(32, 0, 0);
    check(!badZ.has_value(), "a zoom past 31 is refused rather than shifting by 32");

    // The boundary itself must still work. An off-by-one here would silently
    // drop the last row and column of every zoom.
    auto edge = archive->tile(2, 3, 3);
    check(edge.has_value(), "x = 2^z - 1 is accepted");
}

// ============================================================================
// Layout
// ============================================================================

void test_the_deduplicated_view_layout_reads_the_same()
{
    // mb-util and tippecanoe write `tiles` as a VIEW over map+images. Nothing
    // in the library may notice, which is why it queries `tiles` and never
    // interrogates sqlite_master.
    const TempDir dir("view");
    const auto path = dir.file("view.mbtiles");
    check(Builder(path)
              .layout(Layout::DeduplicatedView)
              .meta("name", "dedup")
              .meta("format", "pbf")
              .tmsTile(3, 2, 1, gzipBlob(0x42))
              .build()
              .empty(),
          "built the deduplicated archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "a map/images archive opens");
    if (!archive)
    {
        SPDLOG_ERROR("  {}", mbtiles::to_string(archive.error()));
        return;
    }

    // TMS row 1 at z=3 is XYZ y = 8 - 1 - 1 = 6.
    auto tile = archive->tile(3, 2, 6);
    check(tile.has_value() && tile->has_value(), "a tile reads out of the view layout");
    if (tile && *tile)
    {
        check((*tile)->data.back() == 0x42, "the view layout returns the right blob");
        check((*tile)->encoding == mbtiles::Encoding::Gzip, "the view layout sniffs encoding too");
    }
}

// ============================================================================
// Encoding
// ============================================================================

void test_encoding_is_sniffed_from_the_blob()
{
    const TempDir dir("encoding");
    const auto path = dir.file("encoding.mbtiles");

    // Row 0 gzip, row 1 raw protobuf. The archive's metadata says nothing about
    // compression, because the format has nowhere to say it -- which is the
    // whole reason this is sniffed.
    check(Builder(path)
              .meta("name", "enc")
              .tmsTile(2, 0, 0, gzipBlob(0x01))
              .tmsTile(2, 0, 1, { 0x1A, 0x2F, 0x78, 0x02 })
              .build()
              .empty(),
          "built the encoding archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the encoding archive opens");
    if (!archive)
    {
        return;
    }

    auto gzipped = archive->tile(2, 0, 3);
    check(gzipped && *gzipped && (*gzipped)->encoding == mbtiles::Encoding::Gzip,
          "a gzip blob is reported as gzip");

    auto raw = archive->tile(2, 0, 2);
    check(raw && *raw && (*raw)->encoding == mbtiles::Encoding::Identity,
          "a raw blob is reported as identity");
}

void test_an_empty_tile_is_present_and_empty()
{
    // A zero-byte tile is a legitimate thing for an archive to store for an
    // empty area, and is NOT the same as an absent one. The socal archive has
    // 23-byte tiles all over it for the same reason.
    const TempDir dir("emptytile");
    const auto path = dir.file("empty.mbtiles");
    check(Builder(path).meta("name", "e").tmsTile(1, 0, 0, {}).build().empty(),
          "built the empty-tile archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the empty-tile archive opens");
    if (!archive)
    {
        return;
    }

    auto tile = archive->tile(1, 0, 1);
    check(tile.has_value() && tile->has_value(), "a zero-length tile is present");
    check(tile && *tile && (*tile)->data.empty(), "a zero-length tile is empty");
}

// ============================================================================
// Concurrency
// ============================================================================

void test_concurrent_reads_return_the_right_tiles()
{
    // nodes/map_server answers zenoh queries on several RX threads against one
    // Archive. One prepared statement has one cursor, so without the lock two
    // threads step it at once and hand each other the wrong tile -- which is
    // not a crash, and not a blank map, just wrong geography.
    const TempDir dir("threads");
    const auto path = dir.file("threads.mbtiles");

    constexpr std::uint8_t kZ = 6;
    constexpr int kTiles = 32;

    Builder builder(path);
    builder.meta("name", "threads");
    for (int i = 0; i < kTiles; ++i)
    {
        builder.tmsTile(kZ, i, i, gzipBlob(static_cast<std::uint8_t>(i)));
    }
    check(builder.build().empty(), "built the concurrent archive");

    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "the concurrent archive opens");
    if (!archive)
    {
        return;
    }

    std::atomic<int> wrong { 0 };
    std::atomic<int> errored { 0 };
    std::vector<std::thread> threads;
    threads.reserve(8);

    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back([&archive, &wrong, &errored]() {
            for (int pass = 0; pass < 64; ++pass)
            {
                for (int i = 0; i < kTiles; ++i)
                {
                    const std::uint32_t y = (1U << kZ) - 1U - static_cast<std::uint32_t>(i);
                    auto tile = archive->tile(kZ, static_cast<std::uint32_t>(i), y);
                    if (!tile)
                    {
                        ++errored;
                        continue;
                    }
                    if (!*tile || (*tile)->data.back() != static_cast<std::uint8_t>(i))
                    {
                        ++wrong;
                    }
                }
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    check(errored.load() == 0, "concurrent reads produce no errors");
    check(wrong.load() == 0, "concurrent reads never return another thread's tile");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_missing_file_is_not_found();
    test_a_missing_file_is_not_silently_created();
    test_a_directory_is_not_an_archive();
    test_a_non_sqlite_file_is_rejected();
    test_a_database_without_tiles_is_rejected();
    test_a_database_without_metadata_is_rejected();

    test_xyz_requests_read_tms_rows();
    test_the_flip_is_symmetric_across_the_pyramid();

    test_a_missing_tile_is_empty_not_an_error();
    test_coordinates_outside_the_pyramid_are_refused();

    test_the_deduplicated_view_layout_reads_the_same();

    test_encoding_is_sniffed_from_the_blob();
    test_an_empty_tile_is_present_and_empty();

    test_concurrent_reads_return_the_right_tiles();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all mbtiles archive checks passed");
    return 0;
}
