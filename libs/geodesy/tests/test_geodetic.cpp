// SPDX-License-Identifier: GPL-3.0-or-later
//
// Geodetic <-> ECEF and the NED frame.
//
// The round trip alone proves little: llhToEcef is textbook and ecefToLlh
// could invert a wrong version of it perfectly. So ecefToLlh is also checked
// against an independent solver written here (Bowring's iteration, a
// different algorithm with no shared intermediate), and the forward
// transform is pinned by points whose answer is geometry, not arithmetic --
// the equator, the poles, a metre straight up.

#include "geodesy/geodetic.h"

#include "csym/function.h"

#include <spdlog/spdlog.h>

#include <cmath>
#include <limits>
#include <numbers>
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

void near(double got, double want, double tolerance, const std::string& what)
{
    check(std::fabs(got - want) <= tolerance,
          fmt::format("{}: got {:.15g} want {:.15g} (diff {:.3g}, tol {:.3g})", what, got, want, got - want,
                      tolerance));
}

using geodesy::Llh;
using V3 = csym::Vector3<double>;
namespace wgs84 = geodesy::wgs84;

constexpr double kDeg = std::numbers::pi / 180.0;

constexpr bool within(double a, double b, double tolerance)
{
    return (a > b ? a - b : b - a) <= tolerance;
}

// ---- compile time: the points that are geometry ---------------------------

constexpr V3 kEquatorPrime = geodesy::llhToEcef(Llh<double>{0.0, 0.0, 0.0});
static_assert(within(kEquatorPrime[0], wgs84::kA, 1e-6) && within(kEquatorPrime[1], 0.0, 1e-9) &&
              within(kEquatorPrime[2], 0.0, 1e-9));

constexpr V3 kNorthPole = geodesy::llhToEcef(Llh<double>{std::numbers::pi / 2, 0.0, 0.0});
static_assert(within(kNorthPole[2], wgs84::kB, 1e-6) && within(kNorthPole[0], 0.0, 1e-6));

constexpr Llh<double> kBack = geodesy::ecefToLlh(V3{wgs84::kA + 250.0, 0.0, 0.0});
static_assert(within(kBack.lat, 0.0, 1e-12) && within(kBack.lon, 0.0, 1e-12) && within(kBack.h, 250.0, 1e-6));

static_assert(within(wgs84::kB, 6356752.3142, 1e-4), "TR8350.2 table 3.3 publishes b to 0.1 mm");
static_assert(within(wgs84::kE2, 6.69437999014e-3, 1e-14), "TR8350.2 table 3.3");

// ---- an independent inverse ---------------------------------------------

// Bowring (1976) with iteration to convergence. Shares nothing with
// Vermeille's closed form beyond the ellipsoid constants.
Llh<double> bowring(const V3& e)
{
    const double a = wgs84::kA, b = wgs84::kB, e2 = wgs84::kE2, ep2 = wgs84::kEp2;
    const double p = std::hypot(e[0], e[1]);
    double lat = std::atan2(e[2], p * (1.0 - e2));
    for (int i = 0; i < 10; ++i)
    {
        const double beta = std::atan2(b * std::sin(lat), a * std::cos(lat));
        lat = std::atan2(e[2] + ep2 * b * std::pow(std::sin(beta), 3),
                         p - e2 * a * std::pow(std::cos(beta), 3));
    }
    const double n = a / std::sqrt(1.0 - e2 * std::sin(lat) * std::sin(lat));
    // Away from the poles use p; near them use z, where p/cos loses precision.
    const double h = std::fabs(lat) < 1.0 ? p / std::cos(lat) - n : e[2] / std::sin(lat) - n * (1.0 - e2);
    return {lat, std::atan2(e[1], e[0]), h};
}

void testRoundTripGrid()
{
    const double heights[] = {-400.0, 0.0, 1.5, 100.0, 8848.0, 30000.0};
    double worst_m = 0.0;
    double worst_vs_bowring = 0.0;
    for (int ilat = -12; ilat <= 12; ++ilat)
    {
        for (int ilon = -12; ilon <= 12; ++ilon)
        {
            for (double h : heights)
            {
                const Llh<double> p{7.5 * ilat * kDeg, 15.0 * ilon * kDeg, h};
                const V3 e = geodesy::llhToEcef(p);
                const Llh<double> q = geodesy::ecefToLlh(e);
                const V3 e2 = geodesy::llhToEcef(q);
                worst_m = std::max(worst_m, std::sqrt((e2 - e).squared_norm()));

                near(q.lat, p.lat, 1e-11, fmt::format("lat at {} {} {}", ilat, ilon, h));
                near(q.h, p.h, 1e-5, fmt::format("h at {} {} {}", ilat, ilon, h));
                // Longitude is undefined at the poles; everywhere else it is
                // compared on the circle, so +180 and -180 agree.
                if (std::abs(ilat) != 12)
                {
                    near(std::remainder(q.lon - p.lon, 2 * std::numbers::pi), 0.0, 1e-12,
                         fmt::format("lon at {} {} {}", ilat, ilon, h));
                }
                const Llh<double> r = bowring(e);
                worst_vs_bowring = std::max(worst_vs_bowring, std::fabs(r.lat - q.lat) * wgs84::kA);
                worst_vs_bowring = std::max(worst_vs_bowring, std::fabs(r.h - q.h));
            }
        }
    }
    SPDLOG_INFO("round trip worst {:.3g} m; Vermeille vs Bowring worst {:.3g} m", worst_m, worst_vs_bowring);
    check(worst_m < 1e-6, "round trip within a micrometre");
    check(worst_vs_bowring < 1e-6, "Vermeille and Bowring agree within a micrometre");
}

void testNedFrame()
{
    // At lat 0, lon 0: north is +z, east is +y, down is -x.
    const auto r = geodesy::rotEcefFromNed(0.0, 0.0);
    near(r(2, 0), 1.0, 1e-15, "north = +z");
    near(r(1, 1), 1.0, 1e-15, "east = +y");
    near(r(0, 2), -1.0, 1e-15, "down = -x");

    // Orthonormal and right-handed everywhere.
    for (double lat = -89.0; lat <= 89.0; lat += 17.0)
    {
        for (double lon = -179.0; lon <= 179.0; lon += 31.0)
        {
            const auto m = geodesy::rotEcefFromNed(lat * kDeg, lon * kDeg);
            const auto mtm = m.transpose() * m;
            for (std::size_t i = 0; i < 3; ++i)
                for (std::size_t j = 0; j < 3; ++j) near(mtm(i, j), i == j ? 1.0 : 0.0, 1e-14, "RᵀR = I");
            const double det = m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) -
                               m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
                               m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0));
            near(det, 1.0, 1e-14, "det = +1");
        }
    }

    // The down axis IS the ellipsoid normal: one metre of height moves the
    // ECEF point one metre along -down. This ties the frame to llhToEcef.
    const Llh<double> p{47.3 * kDeg, 8.5 * kDeg, 400.0};
    const V3 up = geodesy::llhToEcef(Llh<double>{p.lat, p.lon, p.h + 1.0}) - geodesy::llhToEcef(p);
    const auto m = geodesy::rotEcefFromNed(p.lat, p.lon);
    for (std::size_t i = 0; i < 3; ++i) near(up[i], -m(i, 2), 1e-9, "d(ecef)/dh = -down");
}

void testRadii()
{
    near(geodesy::meridianRadius(0.0), wgs84::kA * (1.0 - wgs84::kE2), 1e-6, "M(equator)");
    near(geodesy::primeVerticalRadius(0.0), wgs84::kA, 1e-6, "N(equator)");
    const double polar = wgs84::kA * wgs84::kA / wgs84::kB;
    near(geodesy::meridianRadius(std::numbers::pi / 2), polar, 1e-6, "M(pole) = a^2/b");
    near(geodesy::primeVerticalRadius(std::numbers::pi / 2), polar, 1e-6, "N(pole) = a^2/b");

    // One arc-second of latitude is M * 1" metres of ground: check against
    // the ECEF chord at 45 degrees, where both are ~30.87 m.
    const double lat = 45.0 * kDeg, dlat = kDeg / 3600.0;
    const V3 a = geodesy::llhToEcef(Llh<double>{lat - dlat / 2, 0.0, 0.0});
    const V3 b = geodesy::llhToEcef(Llh<double>{lat + dlat / 2, 0.0, 0.0});
    near(std::sqrt((b - a).squared_norm()), geodesy::meridianRadius(lat) * dlat, 1e-6, "M matches the chord");
}

void testPlausibility()
{
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double inf = std::numeric_limits<double>::infinity();
    check(geodesy::isPlausible(Llh<double>{0.5, 2.0, 100.0}), "ordinary point accepted");
    check(geodesy::isPlausible(Llh<double>{std::numbers::pi / 2, 0.0, 0.0}), "pole accepted");
    check(!geodesy::isPlausible(Llh<double>{nan, 0.0, 0.0}), "NaN lat rejected");
    check(!geodesy::isPlausible(Llh<double>{0.0, inf, 0.0}), "inf lon rejected");
    check(!geodesy::isPlausible(Llh<double>{0.0, 0.0, nan}), "NaN height rejected");
    check(!geodesy::isPlausible(Llh<double>{91.0 * kDeg, 0.0, 0.0}), "lat past the pole rejected");
    // Degrees handed over as radians: 47 "radians" is out of range, which is
    // the one unit mistake this check can catch.
    check(!geodesy::isPlausible(Llh<double>{47.0, 8.0, 400.0}), "degrees-as-radians rejected");
    check(!geodesy::isPlausible(Llh<double>{0.0, 0.0, 1e6}), "1000 km up rejected");

    check(geodesy::isPlausibleEcef(geodesy::llhToEcef(Llh<double>{0.8, -1.2, 250.0})), "ordinary ECEF accepted");
    check(!geodesy::isPlausibleEcef(V3{0.0, 0.0, 0.0}), "earth's centre rejected");
    check(!geodesy::isPlausibleEcef(V3{nan, 0.0, 0.0}), "NaN ECEF rejected");
    check(!geodesy::isPlausibleEcef(V3{4e6, 0.0, 0.0}), "2000 km below the surface rejected");
}

// The same template, traced by csym: a factor that uses llhToEcef gets its
// Jacobian from this path, so it is checked against finite differences.
constexpr auto kLlhToEcef = [](auto lat, auto lon, auto h) {
    using T = decltype(lat);
    return geodesy::llhToEcef(Llh<T>{lat, lon, h});
};

void testSymbolicJacobian()
{
    using F = csym::Function<kLlhToEcef, double, double, double>;
    const double lat = 0.83, lon = -1.9, h = 312.0;
    const auto [value, J] = F::jacobian(lat, lon, h);
    const V3 direct = geodesy::llhToEcef(Llh<double>{lat, lon, h});
    for (std::size_t i = 0; i < 3; ++i) near(value[i], direct[i], 1e-6, "traced value");

    const double step[3] = {1e-8, 1e-8, 1e-3};
    for (std::size_t c = 0; c < 3; ++c)
    {
        double args_p[3] = {lat, lon, h}, args_m[3] = {lat, lon, h};
        args_p[c] += step[c];
        args_m[c] -= step[c];
        const V3 ep = geodesy::llhToEcef(Llh<double>{args_p[0], args_p[1], args_p[2]});
        const V3 em = geodesy::llhToEcef(Llh<double>{args_m[0], args_m[1], args_m[2]});
        for (std::size_t r = 0; r < 3; ++r)
        {
            const double fd = (ep[r] - em[r]) / (2 * step[c]);
            near(J(r, c), fd, 1e-5 * std::max(1.0, std::fabs(fd)), fmt::format("J({}, {})", r, c));
        }
    }
}

}  // namespace

int main()
{
    testRoundTripGrid();
    testNedFrame();
    testRadii();
    testPlausibility();
    testSymbolicJacobian();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("geodetic: all passed");
    return 0;
}
