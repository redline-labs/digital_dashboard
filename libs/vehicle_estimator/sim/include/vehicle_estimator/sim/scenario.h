#pragma once

// Driving scenarios with exact truth, and the sensor streams a car running
// them would produce: an MTi's samples (headers, wraps, latency and all) and
// a dual-antenna receiver's epochs.
//
// Trajectories are analytic in position, velocity and acceleration so the
// simulated IMU increments are exact to quadrature error. Every scenario
// starts parked, because the estimator levels itself on a stationary
// accelerometer, and then does something a drift car does.

#include "vehicle_estimator/measurements.h"

#include "imu_preint/increment.h"
#include "imu_preint/sim.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include "geodesy/gravity.h"

#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

namespace vehicle_estimator::sim
{

// The vehicle's own motion: its IMU point in the NED frame at an origin, and
// the body attitude (FRD).
class VehicleMotion
{
  public:
    struct Sample
    {
        Eigen::Vector3d p = Eigen::Vector3d::Zero();
        Eigen::Vector3d v = Eigen::Vector3d::Zero();
        Eigen::Vector3d a = Eigen::Vector3d::Zero();
        Eigen::Matrix3d R_n_b = Eigen::Matrix3d::Identity();
    };
    virtual ~VehicleMotion() = default;
    virtual Sample at(double t) const = 0;
    virtual double duration() const = 0;
};

// Parked for `park` seconds, then launches round a circle of `radius` to
// `speed`, eases into a drift, swings the slip angle sinusoidally between
// `beta_low` and `beta_high`, and finally (if `spin`) rotates the nose past
// ninety degrees of slip.
struct SkidpadParams
{
    double park = 5.0;
    double radius = 40.0;
    double speed = 18.0;
    double launch = 6.0;    // s to reach speed
    double beta_low = 0.05, beta_high = 0.45;  // rad
    double beta_period = 8.0;
    double drive = 40.0;    // s after the park
    bool spin = false;
};
std::unique_ptr<VehicleMotion> skidpad(const SkidpadParams& p = {});

// A figure of eight (lemniscate of Gerono), slip proportional to curvature
// so it changes sign at the crossing, as a drift transition does.
struct FigureEightParams
{
    double park = 5.0;
    double size = 60.0;      // m, half-width
    double period = 20.0;    // s per lap
    double ramp = 5.0;       // s to reach full pace
    double beta_max = 0.5;   // rad
    double drive = 45.0;
};
std::unique_ptr<VehicleMotion> figureEight(const FigureEightParams& p = {});

// Parked for the whole duration.
std::unique_ptr<VehicleMotion> parked(double duration);

// A drive written as phases. Each eases -- C2, so the IMU truth stays exact --
// from where the last one ended to its own speed, turns the course by `turn`,
// and settles the body onto `pitch` and `roll` (a parked car on a grade, not a
// climbing one). Slip is `slip_bias` held through the phase plus a bump of
// `slip` that is zero at both ends: a corner taken sideways.
struct Phase
{
    double duration = 1.0;
    double speed = 0.0;      // m/s at the end
    double turn = 0.0;       // rad of course change
    double slip = 0.0;       // rad, peak of the bump
    double slip_bias = 0.0;  // rad, held
    double pitch = 0.0, roll = 0.0;  // rad, body attitude at the end
    double climb = 0.0;  // m of height gained over the phase; the body pitches to the grade
};
std::unique_ptr<VehicleMotion> scripted(std::vector<Phase> phases, double heading = 0.3);

// Laps of a stadium: straights where the car runs true, half-turn corners
// taken in a drift. What a mounting yaw is learned on.
struct TrackParams
{
    double park = 5.0;
    double launch = 6.0;
    double speed = 22.0;
    double straight = 6.0;       // s
    double corner = 10.0;        // s per half turn
    double corner_slip = 0.35;   // rad
    double straight_slip = 0.0;  // rad, a car that crabs a little on the straights
    int laps = 3;
};
std::unique_ptr<VehicleMotion> track(const TrackParams& p = {});

// Drive, stop on a grade, wait, drive on at a new heading: what a mounting
// roll and pitch are learned on. Grades and cambers alternate in sign, as a
// real set of stops would roughly.
struct StopAndGoParams
{
    double park = 5.0;
    int stops = 6;
    double speed = 12.0;
    double drive = 8.0;  // s accelerating and turning
    double brake = 4.0;
    double hold = 8.0;   // s stopped
    double grade = 0.03, camber = 0.02;  // rad
    double turn = 1.3;   // rad of course change between stops
};
std::unique_ptr<VehicleMotion> stopAndGo(const StopAndGoParams& p = {});

struct Truth
{
    Eigen::Matrix3d R_n_b = Eigen::Matrix3d::Identity();
    Eigen::Vector3d p_e = Eigen::Vector3d::Zero();     // reference point
    Eigen::Vector3d v_ned = Eigen::Vector3d::Zero();   // reference point
    Eigen::Vector3d v_body = Eigen::Vector3d::Zero();  // reference point
    Eigen::Vector3d rate_body = Eigen::Vector3d::Zero();
    double sideslip = 0.0;  // at the reference point
    double roll = 0.0, pitch = 0.0, yaw = 0.0;
};

struct SensorModel
{
    // Mounting, in the IMU frame (as the estimator's config states it).
    Eigen::Matrix3d R_b_i = (Eigen::Matrix3d() << 1, 0, 0, 0, -1, 0, 0, 0, -1).finished();
    // The IMU knocked in its mount: from t, over `duration`, the true R_b_i
    // turns by `yaw` about the body's down axis.
    struct MountStep
    {
        double t = 0.0, duration = 0.5, yaw = 0.0;
    };
    std::optional<MountStep> mount_step;
    Eigen::Matrix3d mountingAt(double t) const;
    Eigen::Vector3d reference_point = Eigen::Vector3d::Zero();
    Eigen::Vector3d lever_arm = Eigen::Vector3d(0.3, 0.0, 1.2);         // true, to antenna 1
    Eigen::Vector3d antenna2_lever_arm = Eigen::Vector3d(-1.2, 0.0, 1.2);  // true, to antenna 2

    // IMU.
    double imu_rate = 100.0;
    double gyro_noise_density = 0.007 * 3.14159265358979323846 / 180.0;
    double accel_noise_density = 60e-6 * 9.80665;
    Eigen::Vector3d gyro_bias = Eigen::Vector3d(0.002, -0.0015, 0.001);  // rad/s
    Eigen::Vector3d accel_bias = Eigen::Vector3d(0.04, -0.03, 0.05);     // m/s^2
    // The gyro bias wandering, rad/s/sqrt(s): what an MTi's in-run stability
    // is. Zero keeps the bias constant (and the auxiliary stream untouched).
    double gyro_bias_walk = 0.0;
    imu_preint::DvFrame dv_frame = imu_preint::DvFrame::end;
    std::uint16_t first_counter = 65500;          // wraps early on purpose
    std::uint32_t first_tick = 0xFFFF0000u;       // and so does the tick counter
    std::vector<std::size_t> dropped_imu;         // sample indices never delivered
    double imu_latency = 0.002, imu_jitter = 0.002;  // s: floor and mean excess

    // Magnetometer, in every IMU packet: m = A (R_i_e B_e) / F + h + noise,
    // in the MTi's arbitrary units. Its own random stream, so turning it on
    // changes no other sensor's noise.
    // Truth gravity; null is WGS84 normal gravity everywhere. A
    // deflec::DeflectedGravity makes it lean as real gravity does.
    std::shared_ptr<const geodesy::GravityModel> gravity;
    bool magnetometer = true;
    Eigen::Matrix3d mag_soft_iron = (Eigen::Matrix3d() << 1.03, 0.01, -0.02,  //
                                     0.01, 0.98, 0.015,                        //
                                     -0.02, 0.015, 1.04)
                                        .finished();
    Eigen::Vector3d mag_hard_iron = Eigen::Vector3d(0.05, -0.08, 0.12);  // a.u.
    double mag_normalisation_nt = 48000.0;  // the field the factory called 1.0
    double mag_noise = 0.003;               // a.u., per sample
    // [t0, t1): a field that is not the earth's (a bridge, a car alongside),
    // added in NED, a.u.
    struct MagDisturbance
    {
        double t0 = 0.0, t1 = 0.0;
        Eigen::Vector3d ned_au = Eigen::Vector3d::Zero();
    };
    std::vector<MagDisturbance> mag_disturbances;

    // Barometer, in every `baro_every`-th packet, whole pascals:
    // p = ISA(h - offset(t)) + airflow * rho v^2 / 2 + noise.
    bool barometer = true;
    std::size_t baro_every = 2;
    double baro_offset = 35.0;       // m: ellipsoidal height minus pressure altitude (weather + geoid)
    double baro_offset_rate = 0.0;   // m/s: the weather moving
    double baro_airflow = 0.3;       // fraction of dynamic pressure the sensor sees
    double baro_noise_pa = 2.0;

    // GNSS.
    double gnss_rate = 10.0;
    Eigen::Vector3d position_sigma_ned = Eigen::Vector3d(0.02, 0.02, 0.04);
    Eigen::Vector3d velocity_sigma_ned = Eigen::Vector3d(0.02, 0.02, 0.04);
    double yaw_sigma = 0.002, pitch_sigma = 0.004;  // rad
    bool attitude_covariance = true;                // GSOF 27 long form
    FixQuality fix = FixQuality::rtx;
    double gnss_latency = 0.03, gnss_jitter = 0.01;
    std::vector<std::pair<double, double>> outages;               // [t0, t1) with no epochs
    std::vector<std::pair<double, Eigen::Vector3d>> outliers;     // t, NED offset added to position
    // From time t on, the receiver reports `fix` and its noise grows by
    // `scale` (the reported sigmas grow with it, as a receiver's do).
    struct FixChange
    {
        double t = 0.0;
        FixQuality fix = FixQuality::rtx;
        double scale = 1.0;
    };
    std::vector<FixChange> fix_changes;
    // [t0, t1) during which the dual-antenna solution reads off by (yaw, pitch).
    struct AttitudeBias
    {
        double t0 = 0.0, t1 = 0.0;
        Eigen::Vector2d offset = Eigen::Vector2d::Zero();
    };
    std::vector<AttitudeBias> attitude_biases;
    double attitude_from = 0.0;  // no heading before this time
    // [t0, t1): no dual-antenna heading, position and velocity still there --
    // a baseline that lost carrier phase on one antenna.
    std::vector<std::pair<double, double>> attitude_outages;
    // False lets a GNSS epoch arrive after the one that followed it.
    bool gnss_in_order = true;

    // Clocks: host time = host_epoch + t + latency; GPS time = gps_epoch + t.
    double host_epoch = 1790000000.0;
    double gps_epoch = 1470000000.0;
    unsigned seed = 1;
};

// A message in arrival order.
struct Message
{
    double host_time = 0.0;
    std::optional<ImuSample> imu;
    std::optional<GnssEpoch> gnss;
};

class Scenario
{
  public:
    Scenario(std::unique_ptr<VehicleMotion> motion, SensorModel sensors, double lat = 49.33, double lon = 8.57,
             double h = 110.0);

    // Every IMU sample and GNSS epoch, sorted by host arrival time.
    std::vector<Message> messages() const;

    // Truth at scenario time t (seconds from the start, not GPS time).
    Truth truth(double t) const;
    double toScenarioTime(double gps_time) const { return gps_time - sensors_.gps_epoch; }
    double duration() const { return motion_->duration(); }
    const SensorModel& sensors() const { return sensors_; }

    // The IMU frame's trajectory in ECEF, as imu_preint's simulator wants it.
    const imu_preint::Trajectory& imuTrajectory() const { return *imu_traj_; }

  private:
    std::unique_ptr<VehicleMotion> motion_;
    SensorModel sensors_;
    std::unique_ptr<imu_preint::LocalTrajectory> imu_traj_;
};

}  // namespace vehicle_estimator::sim
