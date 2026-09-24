#pragma once

// The earth's magnetic field where the car is, from WMM-HR 2025: what a
// perfectly calibrated magnetometer would read. Cached, because a degree-133
// synthesis is not free and the field changes by nothing that matters over
// a kilometre or an hour.

#include <Eigen/Core>

#include <optional>

namespace vehicle_estimator
{

// GPS seconds (since 1980-01-06) to a decimal year, to the day. Leap seconds
// and time zones are far below what the field's secular change can see.
double decimalYearFromGps(double gps_seconds);

struct MagneticField
{
    Eigen::Vector3d ned_nt = Eigen::Vector3d::Zero();  // north, east, down
    double intensity_nt = 0.0;
    double inclination = 0.0;  // rad, down positive
    double declination = 0.0;  // rad, east positive
};

// lat, lon in rad; h in metres above the ellipsoid.
MagneticField magneticField(double lat, double lon, double h, double gps_seconds);

class MagneticReference
{
  public:
    explicit MagneticReference(double refresh_distance_m = 1000.0, double refresh_age_s = 3600.0)
        : distance_(refresh_distance_m), age_(refresh_age_s)
    {
    }

    const MagneticField& at(double lat, double lon, double h, double gps_seconds);

  private:
    double distance_, age_;
    struct Cached
    {
        double lat, lon, h, t;
        MagneticField field;
    };
    std::optional<Cached> cached_;
};

}  // namespace vehicle_estimator
