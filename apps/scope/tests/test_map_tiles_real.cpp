// SPDX-License-Identifier: GPL-3.0-or-later
//
// TileReader against the archive that is actually on the bench, when it is there.
//
// scope_test_map_tiles proves the mechanism on archives it writes itself; this
// proves it survives a real tile -- hundreds of layers' worth of geometry
// through the same tessellator the dashboard uses, out of a 400 MB file, with
// the real zoom range. The file is far too large to commit, so this is opt-in:
// SCOPE_TEST_ARCHIVE, which CMake sets from REDLINE_MAP_DATA_DIR, and a loud
// skip without it. Same arrangement as mvt_test_real_tiles.
//
// There is no default path. One under ~/Documents made the unit test hang in
// open() on a host without macOS privacy consent for that folder.

#include "map_panel/tile_reader.h"

#include <spdlog/spdlog.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

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

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::off);

    const char* fromEnv = std::getenv("SCOPE_TEST_ARCHIVE");
    const std::filesystem::path path = fromEnv != nullptr ? fromEnv : "";
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec))
    {
        std::fprintf(stderr, "SKIPPED: no archive at '%s'\n", path.string().c_str());
        std::fprintf(stderr,
                     "Configure with -DREDLINE_MAP_DATA_DIR=<dir>, or set SCOPE_TEST_ARCHIVE, to run this.\n");
        return PROJECT_TEST_SKIP_CODE;
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
    expect(out.size() == 1 && out[0].geometry != nullptr && !out[0].geometry->vertices.empty(),
           "a city's worth of geometry came out of it");
    expect(out[0].labels != nullptr && !out[0].labels->empty(), "and its labels were extracted");

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
