// SPDX-License-Identifier: GPL-3.0-or-later
//
// WGS 84 normal gravity.
//
// Somigliana's formula and TR8350.2's height expansion are both one-liners
// that are easy to transcribe wrongly, and a wrong one still returns ~9.8.
// Three independent things pin them: the published equator and pole values,
// the published constants re-derived from the defining four, and the gradient
// of the normal potential written as GM with its zonal harmonics plus the
// centrifugal term -- different physics that must give the same vector.

#include "geodesy/gravity.h"

#include <spdlog/spdlog.h>

#include <cmath>
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

static_assert(within(geodesy::normalGravitySurface(0.0), wgs84::kGammaE, 1e-12), "Somigliana at the equator");
static_assert(within(geodesy::normalGravitySurface(std::numbers::pi / 2), wgs84::kGammaP, 1e-9),
              "Somigliana at the pole");

void testPublishedConstantsAgree()
{
    // k = (b gamma_p) / (a gamma_e) - 1 and m = omega^2 a^2 b / GM: the
    // copied constants must be the ones the defining parameters imply.
    // gamma_e and gamma_p are published to 11 figures, so k re-derived from
    // them carries ~1e-11 of rounding.
    near(wgs84::kB * wgs84::kGammaP / (wgs84::kA * wgs84::kGammaE) - 1.0, wgs84::kSomiglianaK, 2e-11, "k derived");
    near(wgs84::kOmegaIe * wgs84::kOmegaIe * wgs84::kA * wgs84::kA * wgs84::kB / wgs84::kGM, wgs84::kM, 1e-13,
         "m derived");
}

void testHeight()
{
    // The free-air gradient: -0.3086 mGal/m is the figure everyone quotes.
    // A dropped factor of 2 or a units slip is off by far more than 0.3%.
    const double lat = 45.0 * kDeg;
    const double g0 = geodesy::normalGravityNed(lat, 0.0)[2];
    const double g1 = geodesy::normalGravityNed(lat, 1000.0)[2];
    near((g1 - g0) / 1000.0, -3.086e-6, 0.01e-6, "free-air gradient at 45 deg");
    near(g0, geodesy::normalGravitySurface(lat), 1e-15, "h = 0 is the surface value");

    // No north component on the surface, and a small one of the right sign
    // above it (the normal field's plumb line curves toward the equator).
    near(geodesy::normalGravityNed(lat, 0.0)[0], 0.0, 1e-15, "north component on the surface");
    const double gn = geodesy::normalGravityNed(lat, 1000.0)[0];
    check(gn < 0.0 && gn > -1e-5, fmt::format("north component at 1 km is small and negative: {:.3g}", gn));
    near(geodesy::normalGravityNed(lat, 1000.0)[1], 0.0, 0.0, "no east component");
}

// The normal field built from different physics: the ellipsoid's gravity
// potential as GM with its even zonal harmonics, plus the centrifugal
// potential, differentiated numerically. Nothing here shares a formula with
// Somigliana or with the height expansion.
double potential(const V3& p)
{
    // WGS 84 normal-field zonal coefficients (TR8350.2 sec. 3.3 / NGA).
    constexpr double kJ[] = {1.08262982131e-3, -2.37091222e-6, 6.08347e-9, -1.42681e-11};
    const double r = std::sqrt(p.squared_norm());
    const double t = p[2] / r;  // sin of geocentric latitude
    // P2..P8 by Bonnet's recurrence, (n+1) P(n+1) = (2n+1) t P(n) - n P(n-1);
    // the odd degrees are zero in a field symmetric about the equator.
    double pnm1 = 1.0, pn = t, ar_n = 1.0, sum = 0.0;
    const double ar = wgs84::kA / r;
    for (std::size_t k = 1; k < 8; ++k)
    {
        const double n = static_cast<double>(k);  // degree, as a value in the recurrence
        const double next = ((2 * n + 1) * t * pn - n * pnm1) / (n + 1);
        pnm1 = pn;
        pn = next;
        ar_n *= ar;
        if (k % 2 == 1) sum += kJ[k / 2] * ar_n * ar * pn;  // J(k+1) (a/r)^(k+1) P(k+1)
    }
    const double w2 = wgs84::kOmegaIe * wgs84::kOmegaIe;
    return wgs84::kGM / r * (1.0 - sum) + 0.5 * w2 * (p[0] * p[0] + p[1] * p[1]);
}

V3 potentialGradient(const V3& p)
{
    constexpr double h = 1.0;  // m; truncation ~1e-12, rounding ~3e-9 m/s^2
    V3 g;
    for (std::size_t i = 0; i < 3; ++i)
    {
        V3 a = p, b = p;
        a[i] += h;
        b[i] -= h;
        g[i] = (potential(a) - potential(b)) / (2 * h);
    }
    return g;
}

void testAgainstPotential()
{
    double worst_surface = 0.0, worst_height = 0.0;
    for (double lat = -90.0; lat <= 90.0; lat += 5.0)
    {
        for (double h : {0.0, 500.0, 3000.0})
        {
            const V3 p = geodesy::llhToEcef(Llh<double>{lat * kDeg, 0.7, h});
            const V3 diff = geodesy::normalGravityEcef(p) - potentialGradient(p);
            double& worst = h == 0.0 ? worst_surface : worst_height;
            worst = std::max(worst, std::sqrt(diff.squared_norm()));
        }
    }
    SPDLOG_INFO("normal gravity vs zonal potential: surface {:.3g}, 500-3000 m {:.3g} m/s^2", worst_surface,
                worst_height);
    // Somigliana is exact on the surface. Above it the second-order height
    // expansion and the first-order north term are approximations whose
    // residual at 3 km is well under a micro-g; a transcription error is 1e-5
    // or worse.
    check(worst_surface < 1e-7, "surface normal gravity matches the potential");
    check(worst_height < 1e-6, "normal gravity at height matches the potential");
}

void testEcefDirection()
{
    // At the equator gravity points at the earth's centre, at the pole along -z.
    const V3 eq = geodesy::normalGravityEcef(V3{wgs84::kA, 0.0, 0.0});
    near(eq[0], -wgs84::kGammaE, 1e-9, "equator: -x");
    near(eq[1], 0.0, 1e-12, "equator: no y");
    near(eq[2], 0.0, 1e-9, "equator: no z");
    const V3 np = geodesy::normalGravityEcef(V3{0.0, 0.0, wgs84::kB});
    near(np[2], -wgs84::kGammaP, 1e-9, "pole: -z");

    geodesy::NormalGravity model;
    const geodesy::GravityModel& g = model;
    const V3 p = geodesy::llhToEcef(Llh<double>{0.9, 0.1, 200.0});
    const V3 a = g.gravityEcef(p), b = geodesy::normalGravityEcef(p);
    for (std::size_t i = 0; i < 3; ++i) near(a[i], b[i], 0.0, "model interface = function");
}

}  // namespace

int main()
{
    testPublishedConstantsAgree();
    testHeight();
    testAgainstPotential();
    testEcefDirection();
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("gravity: all passed");
    return 0;
}
