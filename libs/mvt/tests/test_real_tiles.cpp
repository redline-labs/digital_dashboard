// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoder against tiles nobody in this repository wrote.
//
// test_decode.cpp builds its own bytes, and however carefully that builder is
// written longhand from the spec, it is still one reading of the spec checking
// another. This test is the other half of the argument: it opens the archive
// named in configs/map_server.yaml and decodes real tilemaker output.
//
// It SKIPS, loudly, when the archive is not there -- the file is 383 MB and is
// not in the repository, so a fresh checkout must still pass. That makes it a
// weaker test than one with a committed fixture, and the trade is deliberate:
// the alternative is committing a fixture extracted with this same decoder,
// which would prove nothing it does not already assume.

#include "mvt/decode.h"
#include "mvt/gzip.h"

#include "mbtiles/archive.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <filesystem>
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

// Where the bench archive lives. Overridable so this is runnable against
// another file without an edit.
std::filesystem::path archivePath()
{
    if (const char* fromEnv = std::getenv("MVT_TEST_ARCHIVE"); fromEnv != nullptr)
    {
        return fromEnv;
    }
    return "/Users/ryan/Documents/map_data/socal-260813.mbtiles";
}

// Slippy tile containing Irvine, CA (33.6865966, -117.8557874) at z14. Worked
// out by hand from the Web Mercator formula rather than computed here, so the
// projection code cannot make this test agree with it.
constexpr std::uint8_t kZ = 14;
constexpr std::uint32_t kX = 2828;
constexpr std::uint32_t kY = 6562;

void test_a_real_tile_decodes(const mbtiles::Archive& archive)
{
    auto stored = archive.tile(kZ, kX, kY);
    check(stored.has_value() && stored->has_value(), "the Irvine tile is in the archive");
    if (!stored || !*stored)
    {
        return;
    }

    check((*stored)->encoding == mbtiles::Encoding::Gzip, "and is stored gzipped");

    auto raw = mvt::inflateIfCompressed((*stored)->data);
    check(raw.has_value(), "it inflates");
    if (!raw)
    {
        SPDLOG_ERROR("  {}", mvt::to_string(raw.error()));
        return;
    }
    check(raw->size() > (*stored)->data.size(), "to something larger than it was compressed");

    auto tile = mvt::decode(*raw);
    check(tile.has_value(), "and decodes");
    if (!tile)
    {
        SPDLOG_ERROR("  {}", mvt::to_string(tile.error()));
        return;
    }

    check(!tile->layers.empty(), "into at least one layer");

    // The layers the archive's own metadata says it contains. Naming them
    // pins that the decoder reads real tilemaker output, not merely that it
    // reads something.
    const mvt::Layer* transportation = tile->layer("transportation");
    check(transportation != nullptr, "the transportation layer is present");

    if (transportation != nullptr)
    {
        check(transportation->extent == 4096, "with the usual 4096 extent");
        check(!transportation->features.empty(), "and roads in it");
        check(!transportation->keys.empty(), "and attribute keys");

        // Irvine has motorways through it -- the 5, the 405, the 133. If the
        // tag indices or the value table were being read wrongly, `class`
        // would come back empty or as the wrong string.
        int classified = 0;
        for (const mvt::Feature& feature : transportation->features)
        {
            if (!transportation->attributeText(feature, "class").empty())
            {
                ++classified;
            }
        }
        check(classified > 0, "and features whose `class` attribute resolves");
    }
}

void test_every_geometry_in_the_tile_is_sane(const mbtiles::Archive& archive)
{
    auto stored = archive.tile(kZ, kX, kY);
    if (!stored || !*stored)
    {
        return;
    }
    auto raw = mvt::inflateIfCompressed((*stored)->data);
    if (!raw)
    {
        return;
    }
    auto tile = mvt::decode(*raw);
    if (!tile)
    {
        return;
    }

    int points = 0;
    int lines = 0;
    int polygons = 0;
    int emptyGeometry = 0;
    int exteriorRings = 0;
    int holes = 0;
    int outsideExtent = 0;

    for (const mvt::Layer& layer : tile->layers)
    {
        for (const mvt::Feature& feature : layer.features)
        {
            if (feature.rings.empty())
            {
                ++emptyGeometry;
            }

            switch (feature.type)
            {
                case mvt::GeomType::Point:
                    ++points;
                    break;
                case mvt::GeomType::LineString:
                    ++lines;
                    break;
                case mvt::GeomType::Polygon:
                    ++polygons;
                    for (const auto& ring : feature.rings)
                    {
                        if (mvt::isExteriorRing(ring))
                        {
                            ++exteriorRings;
                        }
                        else
                        {
                            ++holes;
                        }
                    }
                    break;
                case mvt::GeomType::Unknown:
                    break;
            }

            for (const auto& ring : feature.rings)
            {
                for (const mvt::Point& point : ring)
                {
                    if (point.x < 0 || point.y < 0 ||
                        point.x > static_cast<std::int32_t>(layer.extent) ||
                        point.y > static_cast<std::int32_t>(layer.extent))
                    {
                        ++outsideExtent;
                    }
                }
            }
        }
    }

    SPDLOG_INFO("z{}/{}/{}: {} layers, {} points, {} lines, {} polygons "
                "({} exterior rings, {} holes), {} vertices outside the extent",
                kZ, kX, kY, tile->layers.size(), points, lines, polygons, exteriorRings, holes,
                outsideExtent);

    check(lines > 0, "a z14 tile over a city has lines in it");
    check(polygons > 0, "and polygons");
    check(emptyGeometry == 0, "and no feature with no geometry at all");
    check(exteriorRings > 0, "and at least one exterior ring");

    // Not an assertion that there ARE buffered vertices -- a tile need not have
    // any -- but a record that the decoder kept whatever it found rather than
    // clamping. The count above is what a future regression would change.
    SPDLOG_INFO("(vertices outside 0..extent are expected and are kept, not clamped)");
}

void test_a_sweep_of_tiles_decodes(const mbtiles::Archive& archive)
{
    // A single tile proves the format is read; a few hundred prove there is no
    // case in real data the decoder chokes on. This is the closest thing here
    // to differential testing, and the archive is the differential.
    int attempted = 0;
    int decoded = 0;
    int missing = 0;
    std::string firstFailure;

    for (std::uint8_t z = 8; z <= 14; ++z)
    {
        // A small window around Irvine at each zoom, scaled from the z14
        // coordinates.
        const std::uint32_t centreX = kX >> (14 - z);
        const std::uint32_t centreY = kY >> (14 - z);

        for (std::uint32_t dx = 0; dx < 4; ++dx)
        {
            for (std::uint32_t dy = 0; dy < 4; ++dy)
            {
                ++attempted;
                auto stored = archive.tile(z, centreX + dx, centreY + dy);
                if (!stored || !*stored || (*stored)->data.empty())
                {
                    ++missing;
                    continue;
                }

                auto raw = mvt::inflateIfCompressed((*stored)->data);
                if (!raw)
                {
                    if (firstFailure.empty())
                    {
                        firstFailure = "z" + std::to_string(z) + " inflate: " +
                                       mvt::to_string(raw.error());
                    }
                    continue;
                }

                auto tile = mvt::decode(*raw);
                if (!tile)
                {
                    if (firstFailure.empty())
                    {
                        firstFailure = "z" + std::to_string(z) + "/" +
                                       std::to_string(centreX + dx) + "/" +
                                       std::to_string(centreY + dy) + ": " +
                                       mvt::to_string(tile.error());
                    }
                    continue;
                }
                ++decoded;
            }
        }
    }

    SPDLOG_INFO("swept {} tiles: {} decoded, {} absent from the archive", attempted, decoded,
                missing);
    check(firstFailure.empty(), "every present tile in the sweep decoded: " + firstFailure);
    check(decoded > 20, "and the sweep found a useful number of tiles to decode");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    const std::filesystem::path path = archivePath();

    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        // Skipping, loudly. The alternative -- failing -- would make a fresh
        // checkout red for a file that is deliberately not in the repository.
        SPDLOG_WARN("SKIPPED: no archive at {}", path.string());
        SPDLOG_WARN("Set MVT_TEST_ARCHIVE to point at an .mbtiles to run this.");
        return 0;
    }

    auto archive = mbtiles::Archive::open(path);
    if (!archive)
    {
        SPDLOG_ERROR("{} is there and will not open: {}", path.string(),
                     mbtiles::to_string(archive.error()));
        return 1;
    }

    SPDLOG_INFO("decoding real tiles from {}", path.string());

    test_a_real_tile_decodes(*archive);
    test_every_geometry_in_the_tile_is_sane(*archive);
    test_a_sweep_of_tiles_decodes(*archive);

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all real tile checks passed");
    return 0;
}
