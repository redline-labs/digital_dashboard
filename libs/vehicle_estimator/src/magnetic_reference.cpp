#include "vehicle_estimator/magnetic_reference.h"

#include "wmm/wmmhr2025.h"

#include <cmath>
#include <numbers>

namespace vehicle_estimator
{

namespace
{

constexpr double kDeg = std::numbers::pi / 180.0;

}  // namespace

double decimalYearFromGps(double gps_seconds)
{
    // 1980-01-06 is day 5 of a leap year.
    constexpr double kEpoch = 1980.0 + 5.0 / 366.0;
    constexpr double kYear = 365.2425 * 86400.0;
    return kEpoch + gps_seconds / kYear;
}

MagneticField magneticField(double lat, double lon, double h, double gps_seconds)
{
    const auto e = wmm::wmmhr2025::magnetic_field(wmm::GeodeticCoord{lat / kDeg, lon / kDeg, h / 1000.0},
                                                  decimalYearFromGps(gps_seconds));
    MagneticField f;
    f.ned_nt = Eigen::Vector3d(e.X, e.Y, e.Z);
    f.intensity_nt = e.F;
    f.inclination = e.I * kDeg;
    f.declination = e.D * kDeg;
    return f;
}

const MagneticField& MagneticReference::at(double lat, double lon, double h, double gps_seconds)
{
    if (cached_)
    {
        // Distance on a sphere is plenty for "has it moved a kilometre".
        const double dn = (lat - cached_->lat) * 6.371e6;
        const double de = (lon - cached_->lon) * 6.371e6 * std::cos(lat);
        const double moved = std::sqrt(dn * dn + de * de + (h - cached_->h) * (h - cached_->h));
        if (moved < distance_ && std::fabs(gps_seconds - cached_->t) < age_) return cached_->field;
    }
    cached_ = Cached{lat, lon, h, gps_seconds, magneticField(lat, lon, h, gps_seconds)};
    return cached_->field;
}

}  // namespace vehicle_estimator
