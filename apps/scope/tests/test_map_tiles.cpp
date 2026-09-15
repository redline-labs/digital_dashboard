// SPDX-License-Identifier: GPL-3.0-or-later
//
// TileReader against a real .mbtiles written by mbtiles::Writer.
//
// Every case here is one that draws a plausible map rather than failing:
//
//   * the XYZ/TMS flip. A second flip renders beautifully, mirrored about the
//     equator. It does not throw, log, or return nothing.
//   * absent vs failed. An absent tile is cached empty so it is never asked for
//     twice; caching a FAILED one would make a transient read error permanent
//     for the life of the panel.
//   * an unopenable archive. Dropping it would make a permissions problem look
//     like a tileset that was never configured, and those are different fixes.
//   * the zoom range, known at open. The served path has to learn this over the
//     wire; here it is a metadata read, and if it were wrong the caller would
//     clamp to the wrong level and ask for tiles the archive cannot have.
//
// No Qt widgets, no bus, no GPU: the tessellator and the label extractor are
// pure, so this is a unit test.

#include "map_panel/tile_reader.h"

#include "mbtiles/writer.h"
#include "mvt/encode.h"
#include "mvt/tile.h"

#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

std::filesystem::path tempDir()
{
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "scope_test_map_tiles";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

// A tile with one water polygon covering the whole square. Water because it
// tessellates to a fill with no zoom threshold to trip over, so a tile that
// arrives always produces geometry.
std::vector<std::uint8_t> waterTile()
{
    mvt::Layer layer;
    layer.name = "water";
    layer.extent = 4096;

    mvt::Feature feature;
    feature.type = mvt::GeomType::Polygon;
    feature.rings = {{{0, 0}, {4096, 0}, {4096, 4096}, {0, 4096}}};
    layer.features.push_back(feature);

    mvt::Tile tile;
    tile.layers.push_back(layer);

    auto encoded = mvt::encode(tile);
    return encoded ? *encoded : std::vector<std::uint8_t>{};
}

// Writes an archive holding exactly the tiles listed, declaring z0-z14.
std::string writeArchive(const std::filesystem::path& path,
                         const std::vector<map_render::TileId>& tiles)
{
    auto writer = mbtiles::Writer::create(path);
    if (!writer)
    {
        return {};
    }
    (void)writer->setMetadata("name", "test");
    (void)writer->setMetadata("format", "pbf");
    (void)writer->setMetadata("minzoom", "0");
    (void)writer->setMetadata("maxzoom", "14");

    const std::vector<std::uint8_t> blob = waterTile();
    for (const map_render::TileId& id : tiles)
    {
        (void)writer->put(id.z, id.x, id.y, blob);
    }
    (void)writer->finish();
    return path.string();
}

// Requests, then drains until the expected number of tiles has landed or the
// deadline passes. Polling rather than a condition variable because drain() is
// the GUI-thread contract and this stands in for the paint pass.
std::size_t requestAndDrain(scope::TileReader& reader,
                            const std::vector<map_render::TileId>& wanted, std::size_t expected)
{
    reader.request(wanted);

    std::size_t drained = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (drained < expected && std::chrono::steady_clock::now() < deadline)
    {
        drained += reader.drain();
        if (drained < expected)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    return drained;
}

void testAnUnopenableArchiveIsKeptWithItsReason()
{
    const auto dir = tempDir();
    scope::TileReader reader((dir / "nothing-here.mbtiles").string(), MapStyle_t{}, {});

    expect(!reader.ok(), "a missing archive leaves the reader not ok");
    expect(!reader.error().empty(), "and says why");

    // Must be inert rather than crash: a panel holds one of these and paints.
    std::vector<map_render::CachedTile> out;
    reader.request({{14, 2828, 6562}});
    expect(reader.drain() == 0, "a reader with no archive drains nothing");
    reader.ready({{14, 2828, 6562}}, out);
    expect(out.size() == 1 && !out[0], "and reports the tile as not drawable");
}

void testTheZoomRangeIsKnownAtOpen()
{
    const auto dir = tempDir();
    const std::string path = writeArchive(dir / "a.mbtiles", {{14, 2828, 6562}});

    scope::TileReader reader(path, MapStyle_t{}, {});
    expect(reader.ok(), "the archive opens");
    expect(reader.zoomRange().min == 0 && reader.zoomRange().max == 14,
           "the zoom range is available immediately, with no request and no reply");
}

void testATilePresentInTheArchiveArrivesAndIsDrawable()
{
    const auto dir = tempDir();
    const map_render::TileId irvine{14, 2828, 6562};
    const std::string path = writeArchive(dir / "a.mbtiles", {irvine});

    std::atomic<int> notified{0};
    scope::TileReader reader(path, MapStyle_t{}, [&notified]() { ++notified; });

    expect(requestAndDrain(reader, {irvine}, 1) == 1, "the tile arrives");
    expect(reader.drawable(irvine), "and carries geometry worth drawing");
    expect(notified.load() > 0, "and the ready callback fired");

    std::vector<map_render::CachedTile> out;
    reader.ready({irvine}, out);
    expect(out.size() == 1 && out[0] && out[0].geometry != nullptr,
           "ready() hands back the geometry");
    expect(out[0].bytes > 0, "and it was weighed for the cache budget");

    const scope::TileReaderStats stats = reader.stats();
    expect(stats.decoded == 1, "one tile decoded");
    expect(stats.absent == 0 && stats.failed == 0, "and nothing absent or failed");
}

// THE FLIP. mbtiles stores TMS rows, everything else here is XYZ, and the
// conversion happens in exactly one place -- mbtiles::Archive::tile(). Applied
// twice, or not at all, this asks for Irvine and gets a tile from the wrong
// hemisphere, which renders perfectly.
void testCoordinatesAreXyzEndToEnd()
{
    const auto dir = tempDir();
    const map_render::TileId irvine{14, 2828, 6562};
    // The row this tile occupies in TMS, i.e. the y that must NOT answer.
    const map_render::TileId mirrored{14, 2828, (1u << 14) - 1 - 6562};
    const std::string path = writeArchive(dir / "a.mbtiles", {irvine});

    scope::TileReader reader(path, MapStyle_t{}, {});
    expect(requestAndDrain(reader, {irvine, mirrored}, 2) == 2, "both answers come back");
    expect(reader.drawable(irvine), "the XYZ coordinate that was written is drawable");
    expect(!reader.drawable(mirrored),
           "its TMS mirror is NOT -- a double flip would make this one answer instead");
}

// Absent is the common answer -- most of the pyramid is empty -- and it must be
// cached, or the caller re-requests every hole on every paint forever.
void testAnAbsentTileIsCachedSoItIsAskedForOnce()
{
    const auto dir = tempDir();
    const map_render::TileId present{14, 2828, 6562};
    const map_render::TileId missing{14, 2829, 6562};
    const std::string path = writeArchive(dir / "a.mbtiles", {present});

    scope::TileReader reader(path, MapStyle_t{}, {});
    expect(requestAndDrain(reader, {missing}, 1) == 1, "the absent answer lands");
    expect(!reader.drawable(missing), "an absent tile is not drawable");
    expect(reader.stats().absent == 1, "and is counted as absent, not failed");

    const std::uint64_t requestedOnce = reader.stats().requested;
    reader.request({missing});
    reader.request({missing});
    expect(reader.stats().requested == requestedOnce,
           "asking again does not re-queue it: absence is cached");
}

// A tile whose bytes are not a vector tile at all. This is the case that must
// NOT be cached: absence is a fact about a coordinate and is worth remembering
// forever, but a failure is a fault, and caching it would make a transient
// error permanent for the life of the panel.
void testAFailedDecodeIsCountedAndNotCached()
{
    const auto dir = tempDir();
    const auto path = dir / "a.mbtiles";
    const map_render::TileId broken{14, 2828, 6562};

    {
        auto writer = mbtiles::Writer::create(path);
        (void)writer->setMetadata("name", "test");
        (void)writer->setMetadata("format", "pbf");
        (void)writer->setMetadata("minzoom", "0");
        (void)writer->setMetadata("maxzoom", "14");
        // Not gzip, not a protobuf: a field header naming a wire type that does
        // not exist, so the reader rejects it rather than reading past the end.
        const std::vector<std::uint8_t> garbage{0xFF, 0xFF, 0xFF, 0xFF};
        (void)writer->put(broken.z, broken.x, broken.y, garbage);
        (void)writer->finish();
    }

    scope::TileReader reader(path.string(), MapStyle_t{}, {});
    reader.request({broken});

    // Nothing reaches the mailbox, so wait on the counter instead of on drain.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (reader.stats().failed == 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    expect(reader.stats().failed == 1, "an undecodable tile is counted as failed");
    expect(reader.stats().absent == 0, "and NOT as absent -- they are different faults");
    expect(reader.drain() == 0, "a failed tile puts nothing in the mailbox");
    expect(!reader.drawable(broken), "and is not drawable");

    // The point of not caching it: it can be asked for again.
    const std::uint64_t before = reader.stats().requested;
    reader.request({broken});
    expect(reader.stats().requested > before,
           "a failed tile is asked for again -- it was not cached as a permanent absence");
}

void testRepeatedRequestsDoNotRequeueAnInFlightTile()
{
    const auto dir = tempDir();
    std::vector<map_render::TileId> tiles;
    for (std::uint32_t x = 0; x < 8; ++x)
    {
        tiles.push_back({14, 2820 + x, 6562});
    }
    const std::string path = writeArchive(dir / "a.mbtiles", tiles);

    scope::TileReader reader(path, MapStyle_t{}, {});
    reader.request(tiles);
    const std::uint64_t first = reader.stats().requested;
    reader.request(tiles);
    expect(reader.stats().requested == first,
           "a second identical request adds nothing while the first is in flight");

    expect(requestAndDrain(reader, tiles, tiles.size()) == tiles.size(), "all of them arrive");
}

// A request larger than one queue entry has to be split, and every tile in it
// has to arrive -- an off-by-one in the split silently drops the tail, which
// reads as a slow disk.
void testARequestLargerThanTheBatchUnitIsFullyServed()
{
    const auto dir = tempDir();
    std::vector<map_render::TileId> tiles;
    for (std::uint32_t x = 0; x < 100; ++x)
    {
        tiles.push_back({14, 2800 + x, 6562});
    }
    const std::string path = writeArchive(dir / "a.mbtiles", tiles);

    scope::TileReader reader(path, MapStyle_t{}, {});
    expect(requestAndDrain(reader, tiles, tiles.size()) == tiles.size(),
           "every tile of an over-long request arrives");
    expect(reader.stats().decoded == tiles.size(), "and all of them decoded");
}

// The destructor tears the thread down first, while everything its loop touches
// is still alive. ~TileSource learned this class of bug the hard way.
void testDestructionWhileWorkIsQueuedIsClean()
{
    const auto dir = tempDir();
    std::vector<map_render::TileId> tiles;
    for (std::uint32_t x = 0; x < 200; ++x)
    {
        tiles.push_back({14, 2700 + x, 6562});
    }
    const std::string path = writeArchive(dir / "a.mbtiles", tiles);

    {
        scope::TileReader reader(path, MapStyle_t{}, {});
        reader.request(tiles);
        // No drain, no wait: destruction happens with work still queued and
        // very likely a batch mid-flight.
    }
    expect(true, "destroying a reader with work in flight does not hang or crash");
}

// Against the archive that is actually on the bench, when it is there.
//
// The synthetic cases above prove the mechanism; this proves it survives a real
// tile -- hundreds of layers' worth of geometry through the same tessellator
// the dashboard uses, out of a 400 MB file, with the real zoom range. It SKIPS,
// loudly, when the archive is absent: the file is far too large to commit and a
// fresh checkout must still pass. Same arrangement as mvt_test_real_tiles.
std::filesystem::path realArchivePath()
{
    if (const char* fromEnv = std::getenv("SCOPE_TEST_ARCHIVE"); fromEnv != nullptr)
    {
        return fromEnv;
    }
    return "/Users/ryan/Documents/map_data/socal.mbtiles";
}

void testTheRealArchiveIfItIsThere()
{
    const std::filesystem::path path = realArchivePath();
    if (!std::filesystem::exists(path))
    {
        std::fprintf(stderr, "SKIPPED: no archive at %s\n", path.string().c_str());
        std::fprintf(stderr, "Set SCOPE_TEST_ARCHIVE to point at an .mbtiles to run this.\n");
        return;
    }

    // Irvine at z14, worked out by hand from the Web Mercator formula -- the
    // same anchor libs/mvt uses, so the projection cannot make this agree with
    // itself. A wrong flip or a wrong projection names empty ocean.
    const map_render::TileId irvine{14, 2828, 6562};

    scope::TileReader reader(path.string(), MapStyle_t{}, {});
    expect(reader.ok(), "the real archive opens");
    expect(reader.zoomRange().max >= 14, "and reaches at least z14");

    expect(requestAndDrain(reader, {irvine}, 1) == 1, "the Irvine tile arrives");
    expect(reader.drawable(irvine), "and tessellates to something worth drawing");

    std::vector<map_render::CachedTile> out;
    reader.ready({irvine}, out);
    expect(out.size() == 1 && out[0].geometry != nullptr &&
               !out[0].geometry->vertices.empty(),
           "a city's worth of geometry came out of it");
    expect(out[0].labels != nullptr && !out[0].labels->empty(),
           "and its labels were extracted");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    testAnUnopenableArchiveIsKeptWithItsReason();
    testTheZoomRangeIsKnownAtOpen();
    testATilePresentInTheArchiveArrivesAndIsDrawable();
    testCoordinatesAreXyzEndToEnd();
    testAnAbsentTileIsCachedSoItIsAskedForOnce();
    testAFailedDecodeIsCountedAndNotCached();
    testRepeatedRequestsDoNotRequeueAnInFlightTile();
    testARequestLargerThanTheBatchUnitIsFullyServed();
    testDestructionWhileWorkIsQueuedIsClean();

    testTheRealArchiveIfItIsThere();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
