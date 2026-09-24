// SPDX-License-Identifier: GPL-3.0-or-later
//
// The reader and the interpolation off the happy path, on synthetic grids
// written in NGS's own .bin format: every malformed file -- and a Git LFS
// pointer where the model should be -- refused with its reason; the
// interpolation exact on nodes, exact for a linear field everywhere including
// within a node of an edge (NGS's linear-extrapolation padding), exact for a
// quadratic inside; nothing outside; longitude in any turn. And the gravity
// the model makes.

#include "deflec/model.h"

#include "geodesy/geodetic.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <numbers>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

namespace fs = std::filesystem;
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

struct TempDir {
    fs::path path = fs::temp_directory_path() / ("deflec_test_grid_" + std::to_string(::getpid()));
    TempDir() { fs::create_directories(path); }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

template <typename T>
void put(std::vector<unsigned char>& b, const T& v) {
    unsigned char raw[sizeof(T)];
    std::memcpy(raw, &v, sizeof(T));
    b.insert(b.end(), raw, raw + sizeof(T));
}

// An NGS .bin: south and west edges and spacing in degrees, rows x cols
// nodes, value f(lat, lon).
std::vector<unsigned char> ngs(double south, double west, double d, std::int32_t rows, std::int32_t cols,
                               const std::function<double(double, double)>& f) {
    std::vector<unsigned char> b;
    put(b, south);
    put(b, west);
    put(b, d);
    put(b, d);
    put(b, rows);
    put(b, cols);
    put(b, std::int32_t{1});
    for (std::int32_t i = 0; i < rows; ++i)
        for (std::int32_t j = 0; j < cols; ++j) put(b, static_cast<float>(f(south + d * i, west + d * j)));
    return b;
}

fs::path write(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                static_cast<std::streamsize>(bytes.size()));
    return path;
}

deflec::LoadError errorOf(const fs::path& p) {
    const auto g = deflec::NgsGrid::open(p);
    return g ? static_cast<deflec::LoadError>(-1) : g.error().error;
}

constexpr double kD = 1.0 / 60.0;  // 1', as NGS's grids

void testRefusals(const fs::path& dir) {
    const auto one = [](double, double) { return 1.0; };
    const auto good = ngs(36.0, 242.0, kD, 5, 6, one);
    check(deflec::NgsGrid::open(write(dir / "good.bin", good)).has_value(), "a well-formed grid opens");
    check(errorOf(dir / "missing.bin") == deflec::LoadError::not_found, "a missing file");
    {
        const std::string pointer =
            "version https://git-lfs.github.com/spec/v1\noid sha256:5b1ffadf045b2549749f042281f51d4b1bf05081a2dd95c210f6bbbe879e98fb\nsize 233344848\n";
        write(dir / "pointer.bin", {pointer.begin(), pointer.end()});
        const auto g = deflec::NgsGrid::open(dir / "pointer.bin");
        check(!g && g.error().error == deflec::LoadError::lfs_pointer, "a Git LFS pointer is named as one");
        check(!g && g.error().message.find("git lfs pull") != std::string::npos, "and says how to fix it");
    }
    auto b = good;
    b.resize(40);
    check(errorOf(write(dir / "short.bin", b)) == deflec::LoadError::too_short, "a truncated header");
    b = good;
    b.pop_back();
    check(errorOf(write(dir / "short_data.bin", b)) == deflec::LoadError::size_mismatch, "values one byte short");
    b = good;
    b.push_back(0);
    check(errorOf(write(dir / "long_data.bin", b)) == deflec::LoadError::size_mismatch, "and one byte long");
    check(errorOf(write(dir / "zero_spacing.bin", ngs(36.0, 242.0, 0.0, 5, 6, one))) == deflec::LoadError::bad_header,
          "zero spacing");
    check(errorOf(write(dir / "one_row.bin", ngs(36.0, 242.0, kD, 1, 6, one))) == deflec::LoadError::bad_header,
          "a single row cannot interpolate");
    check(errorOf(write(dir / "pole.bin", ngs(89.99, 242.0, kD, 5, 6, one))) == deflec::LoadError::bad_header,
          "rows running past the pole");
    check(errorOf(write(dir / "nan.bin", ngs(std::nan(""), 242.0, kD, 5, 6, one))) == deflec::LoadError::bad_header,
          "a NaN edge");
    {
        // A header claiming billions of values in a small file is a size
        // mismatch, not an allocation or an out-of-bounds read.
        auto huge = good;
        const std::int32_t big = 2'000'000'000;
        std::memcpy(huge.data() + 32, &big, 4);
        check(errorOf(write(dir / "huge.bin", huge)) != static_cast<deflec::LoadError>(-1), "an absurd row count");
    }

    // A model whose two components are on different grids.
    const fs::path mdir = dir / "model";
    fs::create_directories(mdir);
    write(mdir / deflec::Model::kEtaFile, good);
    write(mdir / deflec::Model::kXiFile, ngs(36.0, 242.0, kD, 5, 7, one));
    const auto m = deflec::Model::open(mdir);
    check(!m && m.error().error == deflec::LoadError::extent_differs, "components on different grids");
}

void testInterpolation(const fs::path& dir) {
    // Exactly representable in float32 at the nodes, so any error is the
    // interpolation's.
    const auto linear = [](double lat, double lon) { return 3.0 + 2.0 * (lat - 36.0) * 60.0 - 1.5 * (lon - 242.0) * 60.0; };
    const auto g = *deflec::NgsGrid::open(write(dir / "linear.bin", ngs(36.0, 242.0, kD, 8, 9, linear)));
    double worst_edge = 0.0, worst_inside = 0.0;
    for (double fy = 0.0; fy <= 7.0; fy += 0.137)
        for (double fx = 0.0; fx <= 8.0; fx += 0.113) {
            const double lat = 36.0 + fy * kD, lon = 242.0 + fx * kD;
            const auto v = g.at(lat, lon);
            if (!v) {
                check(false, "inside the grid answers");
                continue;
            }
            const double e = std::fabs(*v - linear(lat, lon));
            const bool edge = fy < 1.0 || fy > 6.0 || fx < 1.0 || fx > 7.0;
            (edge ? worst_edge : worst_inside) = std::max(edge ? worst_edge : worst_inside, e);
        }
    check(worst_inside < 1e-9, "a linear field is exact inside");
    check(worst_edge < 1e-9, "and within a node of the edge, where the window is padded by linear extrapolation");

    const auto quad = [](double lat, double lon) {
        const double y = (lat - 36.0) * 60.0, x = (lon - 242.0) * 60.0;
        return 0.25 * y * y - 0.5 * x * y + 0.75 * x * x;
    };
    const auto gq = *deflec::NgsGrid::open(write(dir / "quad.bin", ngs(36.0, 242.0, kD, 8, 9, quad)));
    double worst_quad = 0.0;
    for (double fy = 1.0; fy <= 6.0; fy += 0.21)
        for (double fx = 1.0; fx <= 7.0; fx += 0.19)
            worst_quad = std::max(worst_quad, std::fabs(*gq.at(36.0 + fy * kD, 242.0 + fx * kD) - quad(36.0 + fy * kD, 242.0 + fx * kD)));
    check(worst_quad < 1e-9, "a quadratic is exact inside (Catmull-Rom)");

    check(g.at(36.0, 242.0) && g.at(36.0 + 7 * kD, 242.0 + 8 * kD), "the corners are inside");
    check(!g.at(36.0 - 1e-6, 242.05) && !g.at(36.0 + 7 * kD + 1e-6, 242.05), "past the south or north edge is not");
    check(!g.at(36.05, 242.0 - 1e-6) && !g.at(36.05, 242.0 + 8 * kD + 1e-6), "nor past the west or east edge");
    const auto a = g.at(36.05, 242.05), b = g.at(36.05, -117.95), c = g.at(36.05, 602.05);
    check(a && b && c && std::fabs(*a - *b) < 1e-9 && std::fabs(*a - *c) < 1e-9, "longitude in any turn");
    check(!g.at(std::nan(""), 242.05) && !g.at(36.05, std::nan("")), "NaN is outside everything");
}

void testGravity(const fs::path& dir) {
    // A uniform lean: xi 10" north, eta 20" east.
    const fs::path mdir = dir / "uniform";
    fs::create_directories(mdir);
    write(mdir / deflec::Model::kEtaFile, ngs(35.0, 240.0, kD, 121, 121, [](double, double) { return 20.0; }));
    write(mdir / deflec::Model::kXiFile, ngs(35.0, 240.0, kD, 121, 121, [](double, double) { return 10.0; }));
    auto model = deflec::Model::open(mdir);
    check(model.has_value(), "a model opens from its directory");
    if (!model) return;
    const deflec::DeflectedGravity gravity(std::make_shared<const deflec::Model>(std::move(*model)));

    constexpr double kRad = std::numbers::pi / 180.0, kArcsec = kRad / 3600.0;
    const double lat = 35.5 * kRad, lon = -119.5 * kRad;
    const auto p = geodesy::llhToEcef(geodesy::Llh<double>{lat, lon, 100.0});
    check(gravity.refinesNormalAt(p), "inside, the model refines normal gravity");
    const auto g = gravity.gravityEcef(p), n = geodesy::normalGravityEcef(p);
    const auto R = geodesy::rotEcefFromNed(lat, lon);
    double gn = 0, ge = 0, gd = 0, nn = 0, nd = 0;
    for (std::size_t k = 0; k < 3; ++k) {
        gn += R(k, 0) * g[k];
        ge += R(k, 1) * g[k];
        gd += R(k, 2) * g[k];
        nn += R(k, 0) * n[k];
        nd += R(k, 2) * n[k];
    }
    // The plumb line leans xi north and eta east: gravity gains -g xi north
    // and -g eta east, and down is normal gravity's.
    check(std::fabs((gn - nn) + nd * 10.0 * kArcsec) < 1e-9, "north: -g xi");
    check(std::fabs(ge + nd * 20.0 * kArcsec) < 1e-9, "east: -g eta");
    check(std::fabs(gd - nd) < 1e-12, "down: normal gravity's");

    const auto far = geodesy::llhToEcef(geodesy::Llh<double>{52.5 * kRad, 13.4 * kRad, 40.0});
    check(!gravity.refinesNormalAt(far), "outside, it does not");
    const auto gf = gravity.gravityEcef(far), nf = geodesy::normalGravityEcef(far);
    check(gf[0] == nf[0] && gf[1] == nf[1] && gf[2] == nf[2], "and gives normal gravity unchanged");
    const deflec::DeflectedGravity none(nullptr);
    check(!none.refinesNormalAt(p) && none.gravityEcef(p)[2] == n[2], "no model at all is normal gravity");
}

}  // namespace

int main() {
    TempDir dir;
    testRefusals(dir.path);
    testInterpolation(dir.path);
    testGravity(dir.path);
    if (failures) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("grid: all passed");
    return 0;
}
