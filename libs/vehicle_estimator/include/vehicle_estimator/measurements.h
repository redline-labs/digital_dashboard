#pragma once

// What the estimator consumes and produces, with no bus types in sight: the
// node decodes capnp into these, the offline tool decodes a bag into them,
// and the tests build them by hand.

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cstdint>
#include <optional>

namespace vehicle_estimator
{

// What the heading currently rests on.
enum class HeadingSource
{
    none,          // nothing: yaw is unknown
    dual_antenna,  // the antenna baseline, within the last second
    magnetometer,  // the magnetometer is what is holding it
    inertial,      // carried by the gyro since one of the above
};

// One MTi sample: the strapdown increments plus the header that says when.
struct ImuSample
{
    std::uint16_t packet_counter = 0;
    std::uint32_t sample_time_fine = 0;  // 10 kHz device ticks
    Eigen::Quaterniond dq = Eigen::Quaterniond::Identity();
    Eigen::Vector3d dv = Eigen::Vector3d::Zero();
    double host_time = 0.0;  // arrival, s; only its minimum over time matters
    // From the same packet, when the device sent them. The magnetic field in
    // the MTi's arbitrary units (about 1 at the field it was calibrated in),
    // IMU frame; the static pressure in pascals.
    std::optional<Eigen::Vector3d> mag_au;
    std::optional<double> pressure_pa;
};

// How much a GNSS fix can be trusted, coarsely. GSOF 38's fix types map onto
// these; each class scales the reported sigmas (EstimatorConfig).
enum class FixQuality
{
    none,
    autonomous,
    differential,
    float_rtk,
    rtx,
    fixed_rtk,
};

struct GnssPosition
{
    double lat = 0.0, lon = 0.0, h = 0.0;                  // rad, rad, m above the ellipsoid
    Eigen::Matrix3d cov_ned = Eigen::Matrix3d::Identity();  // m^2
};

struct GnssVelocity
{
    Eigen::Vector3d v_ned = Eigen::Vector3d::Zero();  // m/s, of antenna 1
};

// The dual-antenna baseline's direction, as the receiver reports it.
struct DualAntenna
{
    double yaw = 0.0;    // rad, from north, clockwise
    double pitch = 0.0;  // rad, positive when the baseline points up
    std::optional<Eigen::Matrix2d> cov;  // [yaw, pitch], rad^2, when the receiver sent it
};

// Everything one receiver transmission said about one epoch.
struct GnssEpoch
{
    double gps_time = 0.0;  // s, continuous: week * 604800 + time of week
    double host_time = 0.0;
    FixQuality fix = FixQuality::none;
    std::optional<GnssPosition> position;
    std::optional<GnssVelocity> velocity;
    std::optional<DualAntenna> attitude;
};

// The estimator's answer. Frames: e = ECEF, n = local NED at the reference
// point, b = vehicle body, SAE J670 (x forward, y right, z down).
struct VehicleState
{
    double gps_time = 0.0;
    // False before any GNSS has been heard: there is no GPS time yet, and
    // gps_time is the host clock the IMU was mapped onto instead.
    bool gps_time_valid = true;
    bool valid = false;  // initialised and within the configured uncertainty

    // At the configured reference point.
    double lat = 0.0, lon = 0.0, h = 0.0;
    Eigen::Vector3d p_e = Eigen::Vector3d::Zero();
    Eigen::Vector3d v_ned = Eigen::Vector3d::Zero();
    Eigen::Vector3d v_body = Eigen::Vector3d::Zero();
    Eigen::Vector3d accel_body = Eigen::Vector3d::Zero();  // kinematic, gravity removed
    Eigen::Vector3d rate_body = Eigen::Vector3d::Zero();   // relative to the earth
    Eigen::Quaterniond q_n_b = Eigen::Quaterniond::Identity();
    double roll = 0.0, pitch = 0.0, yaw = 0.0;  // rad, ZYX

    // Sideslip: the angle of the velocity at the reference point from the
    // body x axis, positive to the right. Meaningless when stopped.
    double sideslip = 0.0;
    bool sideslip_valid = false;

    // One sigma.
    Eigen::Vector3d sigma_position_ned = Eigen::Vector3d::Zero();
    Eigen::Vector3d sigma_velocity_ned = Eigen::Vector3d::Zero();
    Eigen::Vector3d sigma_attitude = Eigen::Vector3d::Zero();  // roll, pitch, yaw
    double sigma_sideslip = 0.0;

    FixQuality fix = FixQuality::none;

    // Attitude alone can be good when nothing else is: at a start with no
    // GNSS, roll and pitch come from gravity and heading (if anything) from
    // the magnetometer -- in which case heading is MAGNETIC, not true.
    bool attitude_valid = false;
    bool heading_magnetic = false;
    HeadingSource heading_source = HeadingSource::none;
};

}  // namespace vehicle_estimator
