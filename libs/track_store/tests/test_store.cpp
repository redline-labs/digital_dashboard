// SPDX-License-Identifier: GPL-3.0-or-later
//
// The catalogue that shares a file with the tiles.
//
// The whole design rests on two claims, and BOTH fail silently if they are
// wrong:
//
//   1. extra tables are invisible to mbtiles::Archive. If they are not, adding
//      a catalogue breaks the map for everyone, everywhere, at once;
//   2. a file whose tiles and catalogue came from different builds is refused.
//      If it is not, the archive opens, the tiles draw, and a lap distance is
//      measured against a centreline from another ingest run -- which renders
//      perfectly and is wrong.
//
// So the test writes a real archive with mbtiles::Writer, appends a catalogue
// to it, and then reads it back through BOTH libraries. No fixture file, for
// the reason libs/mbtiles/tests gives: a checked-in binary is a thing nobody
// can review.

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <spdlog/spdlog.h>
#include <sqlite3.h>

#include "mbtiles/archive.h"
#include "mbtiles/writer.h"
#include "track_store/store.h"

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

std::filesystem::path scratch(const std::string& name)
{
    return std::filesystem::temp_directory_path() / ("track_store_test_" + name + ".mbtiles");
}

// An archive with one real tile in it, exactly as map_build would leave it.
bool writeArchive(const std::filesystem::path& path)
{
    std::filesystem::remove(path);
    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        return false;
    }
    const std::vector<std::uint8_t> tile { 0x1f, 0x8b, 0x08, 0x00, 0x00 };
    if (!writer->put(14, 2650, 6400, tile) || !writer->setMetadata("name", "tracks") ||
        !writer->setMetadata("format", "pbf") || !writer->setMetadata("minzoom", "8") ||
        !writer->setMetadata("maxzoom", "14"))
    {
        return false;
    }
    return writer->finish().has_value();
    // The Writer is destroyed here, which is what CLOSES the database.
    // Appending before this point is two write handles on one file.
}

track_store::TrackRecord sampleRecord()
{
    track_store::TrackRecord record;
    record.id = "Laguna_Seca";
    record.name = "Laguna Seca";
    record.circuit = "Laguna Seca";
    record.venueId = "Laguna_Seca";
    record.west = -121.7597;
    record.south = 36.5798;
    record.east = -121.7480;
    record.north = 36.5900;
    record.centerlineLengthM = 3584.2;
    record.publishedLengthM = 3600.0;
    record.medianWidthM = 13.3;
    record.principalAxisDeg = 42.5;
    record.hasCenterline = true;
    record.closed = true;
    record.combo = false;
    record.quality = track_store::Quality::Ok;
    record.outlinePoints = 569;
    record.gate.source = track_store::GateSource::DataDrop;
    record.gate.centreLatE7 = 365864577;
    record.gate.centreLonE7 = -1217566418;
    record.gate.leftLatE7 = 365864900;
    record.gate.leftLonE7 = -1217566000;
    record.gate.rightLatE7 = 365864200;
    record.gate.rightLonE7 = -1217566800;
    record.gate.centerlineOffsetCm = 137;
    record.gate.widthM = 13.9;
    return record;
}

void test_a_catalogue_is_invisible_to_the_tile_reader()
{
    const auto path = scratch("invisible");
    if (!writeArchive(path))
    {
        check(false, "the fixture archive was written");
        return;
    }

    {
        auto writer = track_store::Writer::append(path, "2026-08-17T10:00:00Z-abc123");
        check(writer.has_value(), "a catalogue can be appended to a finished archive");
        if (!writer)
        {
            return;
        }
        check(writer->put(sampleRecord()).has_value(), "and a record written into it");
        check(writer->finish().has_value(), "and committed");
    }

    // THE CLAIM. mbtiles::Archive must not notice any of that.
    auto archive = mbtiles::Archive::open(path);
    check(archive.has_value(), "mbtiles::Archive still opens the archive");
    if (archive)
    {
        auto tile = archive->tile(14, 2650, 6400);
        check(tile.has_value() && tile->has_value(),
              "and still returns the tile that was in it");
        check(archive->metadata().name == "tracks", "with its metadata intact");
    }

    std::filesystem::remove(path);
}

void test_every_field_survives_the_round_trip()
{
    const auto path = scratch("roundtrip");
    if (!writeArchive(path))
    {
        check(false, "the fixture archive was written");
        return;
    }

    const auto original = sampleRecord();
    {
        auto writer = track_store::Writer::append(path, "build-1");
        if (!writer)
        {
            check(false, "append");
            return;
        }
        (void)writer->put(original);
        (void)writer->finish();
    }

    auto store = track_store::Store::open(path);
    check(store.has_value(), "the catalogue opens");
    if (!store)
    {
        return;
    }
    check(store->buildId() == "build-1", "the build id comes back");
    check(store->tracks().size() == 1, "one track");

    const auto* found = store->find("Laguna_Seca");
    check(found != nullptr, "and it is findable by id");
    if (found == nullptr)
    {
        return;
    }

    check(found->name == original.name && found->circuit == original.circuit &&
              found->venueId == original.venueId,
          "the names survive");
    check(found->west == original.west && found->north == original.north, "the bounds survive");
    check(found->quality == track_store::Quality::Ok, "the quality verdict survives");
    check(found->hasCenterline && found->closed && !found->combo, "the flags survive");
    check(found->outlinePoints == 569, "the point count survives");
    // Signed and near the int32 limits: a longitude of -121.75 is -1217566418
    // in 1e-7 degrees, and a column read as unsigned would put the gate in the
    // Pacific east of Japan.
    check(found->gate.centreLonE7 == original.gate.centreLonE7,
          "a NEGATIVE longitude survives as a negative number");
    check(found->gate.centerlineOffsetCm == 137, "the gate offset survives");
    check(found->gate.source == track_store::GateSource::DataDrop, "the gate source survives");
    check(found->gate.present(), "and the gate reports itself present");

    check(store->find("nothing_here") == nullptr, "an unknown id is absent, not a crash");

    std::filesystem::remove(path);
}

void test_geometry_blobs_round_trip_exactly()
{
    const auto path = scratch("blobs");
    if (!writeArchive(path))
    {
        check(false, "the fixture archive was written");
        return;
    }

    // Interleaved lat/lon, with a negative longitude and a value that needs all
    // 32 bits.
    const std::vector<std::int32_t> ring { 365864577, -1217566418, 365864000, -1217560000,
                                           365860000, -1217500000 };
    const std::vector<std::uint32_t> distances { 0, 250, 500, 358420 };
    const std::vector<std::uint16_t> halfWidths { 650, 700, 715, 690 };

    {
        auto writer = track_store::Writer::append(path, "build-1");
        if (!writer)
        {
            check(false, "append");
            return;
        }
        (void)writer->put(sampleRecord());
        check(writer->putGeometry("Laguna_Seca", track_store::GeometryKind::Centerline, ring)
                  .has_value(),
              "a coordinate blob is accepted");
        check(writer
                  ->putGeometry("Laguna_Seca",
                                track_store::GeometryKind::CenterlineDistanceCm, distances)
                  .has_value(),
              "a distance blob is accepted");
        check(writer
                  ->putGeometry("Laguna_Seca", track_store::GeometryKind::HalfWidthCm, halfWidths)
                  .has_value(),
              "a half-width blob is accepted");
        // The kind and the value width must agree, or a uint16 array read as
        // int32 halves its length and doubles every value.
        check(!writer->putGeometry("Laguna_Seca", track_store::GeometryKind::Centerline, halfWidths)
                   .has_value(),
              "and a blob written under the wrong kind is REFUSED");
        (void)writer->finish();
    }

    auto store = track_store::Store::open(path);
    if (!store)
    {
        check(false, "the catalogue opens");
        return;
    }

    auto centerline = store->geometry("Laguna_Seca", track_store::GeometryKind::Centerline);
    check(centerline.has_value() && centerline->has_value(), "the centreline comes back");
    if (centerline && centerline->has_value())
    {
        check((*centerline)->asCoords() == ring, "bit for bit");
        check((*centerline)->valueCount() == ring.size(), "with the right value count");
        check((*centerline)->asUint16().empty(),
              "and refuses to reinterpret itself as another width");
    }

    auto distance =
        store->geometry("Laguna_Seca", track_store::GeometryKind::CenterlineDistanceCm);
    if (distance && distance->has_value())
    {
        check((*distance)->asUint32() == distances, "the distances come back bit for bit");
    }

    auto widths = store->geometry("Laguna_Seca", track_store::GeometryKind::HalfWidthCm);
    if (widths && widths->has_value())
    {
        check((*widths)->asUint16() == halfWidths, "the half widths come back bit for bit");
    }

    // A track that failed the QA gate has an outline and no centreline. That is
    // a normal answer, not an error, and the two must be distinguishable.
    auto absent = store->geometry("Laguna_Seca", track_store::GeometryKind::InnerRing);
    check(absent.has_value() && !absent->has_value(),
          "a geometry that was never written is ABSENT, not an error");

    std::filesystem::remove(path);
}

void test_a_build_id_mismatch_is_refused()
{
    const auto path = scratch("mismatch");
    if (!writeArchive(path))
    {
        check(false, "the fixture archive was written");
        return;
    }

    {
        auto writer = track_store::Writer::append(path, "build-1");
        if (!writer)
        {
            check(false, "append");
            return;
        }
        (void)writer->put(sampleRecord());
        (void)writer->finish();
    }

    check(track_store::Store::open(path).has_value(), "a matched pair opens");

    // Now make the two halves disagree, which is what a half-finished or
    // half-copied build leaves behind.
    {
        sqlite3* db = nullptr;
        sqlite3_open_v2(path.string().c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
        sqlite3_exec(db,
                     "INSERT OR REPLACE INTO metadata (name, value) "
                     "VALUES ('build_id', 'build-2')",
                     nullptr, nullptr, nullptr);
        sqlite3_close(db);
    }

    auto reopened = track_store::Store::open(path);
    check(!reopened.has_value(), "a mismatched pair is REFUSED");
    if (!reopened)
    {
        check(reopened.error().kind == track_store::Error::Kind::BuildMismatch,
              "and says why (" + track_store::to_string(reopened.error()) + ")");
    }

    std::filesystem::remove(path);
}

void test_an_ordinary_basemap_has_no_catalogue()
{
    // The normal state of socal.mbtiles. A server configured with one wants to
    // hear "there is no catalogue here", not "this file is broken".
    const auto path = scratch("plain");
    if (!writeArchive(path))
    {
        check(false, "the fixture archive was written");
        return;
    }

    auto store = track_store::Store::open(path);
    check(!store.has_value(), "an archive with no track tables does not open as a catalogue");
    if (!store)
    {
        check(store.error().kind == track_store::Error::Kind::NoCatalogue,
              "and is reported as absent rather than broken");
    }

    check(!track_store::Store::open(scratch("does_not_exist")).has_value(),
          "and a missing file fails rather than being created empty");

    std::filesystem::remove(path);
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::warn);
    spdlog::set_pattern("[%^%l%$] %v");

    test_a_catalogue_is_invisible_to_the_tile_reader();
    test_every_field_survives_the_round_trip();
    test_geometry_blobs_round_trip_exactly();
    test_a_build_id_mismatch_is_refused();
    test_an_ordinary_basemap_has_no_catalogue();

    spdlog::set_level(spdlog::level::info);
    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all track store checks passed");
    return 0;
}
