// SPDX-License-Identifier: GPL-3.0-or-later
//
// What the node runs on: DEFLEC2022 when asked and the files are there,
// normal gravity when it is switched off, and normal gravity -- flagged
// unhealthy, with the file and the reason -- when it is asked for and cannot
// be loaded. A missing model must not stop the node; it must not be silent.

#include "gravity_model.h"

#include "geodesy/geodetic.h"

#include <spdlog/spdlog.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{

int failures = 0;
void check(bool ok, const std::string& what)
{
    if (!ok)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

namespace fs = std::filesystem;

// A small NGS .bin over southern California, value v everywhere.
void writeGrid(const fs::path& path, float v)
{
    std::vector<unsigned char> b;
    const auto put = [&](const auto& x) {
        unsigned char raw[sizeof x];
        std::memcpy(raw, &x, sizeof x);
        b.insert(b.end(), raw, raw + sizeof x);
    };
    put(32.0);
    put(240.0);
    put(1.0 / 60.0);
    put(1.0 / 60.0);
    put(std::int32_t{61});
    put(std::int32_t{61});
    put(std::int32_t{1});
    for (int k = 0; k < 61 * 61; ++k) put(v);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

}  // namespace

int main()
{
    const fs::path dir = fs::temp_directory_path() / ("state_estimator_test_gravity_" + std::to_string(::getpid()));
    fs::create_directories(dir);

    state_estimator::NodeConfig off;
    off.gravity.deflection = false;
    const auto a = state_estimator::loadGravity(off);
    check(!a.model && a.healthy, "switched off: normal gravity, and that is healthy");

    state_estimator::NodeConfig missing;
    missing.gravity.modelDir = (dir / "nowhere").string();
    const auto b = state_estimator::loadGravity(missing);
    check(!b.model && !b.healthy, "asked for and missing: normal gravity, flagged");
    check(b.summary.find("nowhere") != std::string::npos && b.summary.find("no such file") != std::string::npos,
          "naming the file and the reason: " + b.summary);

    writeGrid(dir / "SDEFLEC2022.NA.eta.beta_v0a.bin", 20.0F);
    writeGrid(dir / "SDEFLEC2022.NA.xi.beta_v0a.bin", 10.0F);
    state_estimator::NodeConfig found;
    found.gravity.modelDir = dir.string();
    const auto c = state_estimator::loadGravity(found);
    check(c.model && c.healthy, "found: a model: " + c.summary);
    constexpr double kRad = std::numbers::pi / 180.0;
    const auto inside = geodesy::llhToEcef(geodesy::Llh<double>{32.5 * kRad, -119.5 * kRad, 0.0});
    const auto outside = geodesy::llhToEcef(geodesy::Llh<double>{45.0 * kRad, -119.5 * kRad, 0.0});
    check(c.model && c.model->refinesNormalAt(inside) && !c.model->refinesNormalAt(outside),
          "which leans gravity inside its grid and not outside");

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("gravity model: all passed");
    return 0;
}
