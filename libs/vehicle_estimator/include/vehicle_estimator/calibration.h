#pragma once

// The installation, as the estimator learns it: how the IMU is mounted in the
// body, where the primary antenna is, and how the antenna baseline sits
// against the IMU. Each starts as a prior -- the configured value and its
// sigma -- and follows a slow random walk from there, one set of variables
// per calibration segment, so it keeps adjusting instead of hardening into a
// constant, and a batch over a drive sees how it moved.
//
// Tangent order everywhere: mounting (3, a right perturbation of R_b_i, in the
// IMU frame), lever arm (3, m, IMU frame), boresight (2, rad), magnetometer
// (9: hard iron xyz, soft iron xx yy zz xy xz yz), barometer (2: offset m,
// airflow).

#include "vehicle_estimator/config.h"

#include "factor_graph/key.h"

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace vehicle_estimator
{

inline constexpr Eigen::Index kCalibrationDim = 19;
using CalibrationCov = Eigen::Matrix<double, kCalibrationDim, kCalibrationDim>;
using Vector6d = Eigen::Matrix<double, 6, 1>;
using Vector9d = Eigen::Matrix<double, 9, 1>;

struct CalibrationSet
{
    Eigen::Quaterniond mounting = Eigen::Quaterniond::Identity();  // R_b_i
    Eigen::Vector3d lever_arm = Eigen::Vector3d::Zero();
    Eigen::Vector2d boresight = Eigen::Vector2d::Zero();
    Eigen::Vector3d mag_hard_iron = Eigen::Vector3d::Zero();  // a.u.
    Vector6d mag_soft_iron = Vector6d::Zero();                // xx yy zz xy xz yz
    double baro_offset = 0.0;                                 // m
    double baro_airflow = 0.0;
    CalibrationCov cov = CalibrationCov::Identity();

    Eigen::Matrix3d mountingCov() const { return cov.block<3, 3>(0, 0); }
    Eigen::Matrix3d leverArmCov() const { return cov.block<3, 3>(3, 3); }
    Eigen::Matrix2d boresightCov() const { return cov.block<2, 2>(6, 6); }
    Eigen::Matrix<double, 9, 9> magnetometerCov() const { return cov.block<9, 9>(8, 8); }
    Eigen::Matrix2d barometerCov() const { return cov.block<2, 2>(17, 17); }

    Vector9d magnetometer() const
    {
        Vector9d k;
        k << mag_hard_iron, mag_soft_iron;
        return k;
    }
    // I + S, the soft iron as a matrix.
    Eigen::Matrix3d softIronMatrix() const;
};

// Walk densities in tangent order, per sqrt(s).
Eigen::Matrix<double, kCalibrationDim, 1> calibrationWalkDensities(const EstimatorConfig& config);

// The configured installation as a prior: means from the config, sigmas
// uncorrelated. The mounting sigma is given about the BODY axes (roll, pitch,
// yaw of the installation, which is how anyone would state it) and carried
// into the IMU-frame tangent here.
CalibrationSet configuredCalibration(const EstimatorConfig& config);

// One segment's variables.
struct CalibrationKeys
{
    factor_graph::Key mounting, lever_arm, boresight, magnetometer, barometer;
};
CalibrationKeys calibrationKeys(std::uint64_t segment);
bool isCalibrationKey(factor_graph::Key key);

// ---- groups ----------------------------------------------------------------
//
// What is kept between sessions, one group at a time: re-measuring the lever
// arm must not throw away a well-learned mounting.

// The barometer's offset is not a group: it is the weather and the geoid,
// learned afresh every session and never kept.
enum class CalibrationGroup
{
    mounting,
    lever_arm,
    boresight,
    magnetometer,
    baro_airflow,
};
inline constexpr std::array<CalibrationGroup, 5> kCalibrationGroups{
    CalibrationGroup::mounting, CalibrationGroup::lever_arm, CalibrationGroup::boresight,
    CalibrationGroup::magnetometer, CalibrationGroup::baro_airflow};

std::string_view groupName(CalibrationGroup g);
std::optional<CalibrationGroup> groupFromName(std::string_view name);
// Bumped when a group's parameterisation changes, so a stored value in the
// old form stops matching instead of being read as the new one.
int modelVersion(CalibrationGroup g);
Eigen::Index tangentOffset(CalibrationGroup g);
Eigen::Index tangentDim(CalibrationGroup g);
// Stored mean: the mounting as a quaternion (w, x, y, z), the magnetometer as
// hard iron then soft iron (9), the others as is.
Eigen::Index meanDim(CalibrationGroup g);

struct GroupEstimate
{
    CalibrationGroup group = CalibrationGroup::mounting;
    Eigen::VectorXd mean;  // meanDim(group)
    Eigen::MatrixXd cov;   // tangentDim(group) square
};

GroupEstimate extract(const CalibrationSet& set, CalibrationGroup g);
// Replaces the group's mean and covariance; its correlation with the other
// groups is dropped, since they did not come from the same place.
void apply(CalibrationSet& set, const GroupEstimate& estimate);
// What is wrong with a stored estimate, or nothing: dimensions, finiteness, a
// unit quaternion, a symmetric positive-definite covariance.
std::optional<std::string> problem(const GroupEstimate& estimate);
// b relative to a, in the group's tangent.
Eigen::VectorXd tangentDifference(const GroupEstimate& a, const GroupEstimate& b);
// "roll 180.00 pitch 0.41 yaw -0.87 deg, sigma 0.05 0.05 0.08 deg": for a
// person reading the database.
std::string summary(const GroupEstimate& estimate);

// ---- the policy ----------------------------------------------------------------

// FNV-1a over the group, its model version, and the configured MEANS it is
// learned from. The sigmas are left out on purpose: saying you are more or
// less sure of a measurement keeps what was learned; changing the
// measurement does not. The boresight hashes both antennas, because it is
// learned around the baseline between them.
std::uint64_t priorHash(CalibrationGroup g, const EstimatorConfig& config);
std::string hashHex(std::uint64_t hash);

struct WritePolicy
{
    double min_interval = 900.0;  // s between writes of one group
    double move_sigma = 1.0;      // Mahalanobis distance that counts as moved
    double tighten_ratio = 0.5;   // a sigma this fraction of what was written
    double settle = 120.0;        // s after a (re)start before anything is written
};

enum class WriteReason
{
    first_converged,
    moved,
    tightened,
    shutdown_moved,
    shutdown_tightened,
};
std::string_view reasonName(WriteReason r);

struct LastWritten
{
    GroupEstimate estimate;
    std::optional<double> at;  // this session's clock; none for a row from an earlier one
};

struct WriteContext
{
    double now = 0.0;
    bool valid = false;         // the navigation solution is
    double settled = 0.0;       // s since the estimator last (re)started
    // What the group was learned from: the mounting's straights and stops, the
    // magnetometer's s against a dual-antenna heading, the airflow's s moving.
    // Groups that need it write nothing at zero; the others ignore it.
    double evidence = 0.0;
    bool shutdown = false;      // the last chance: skips the interval, not the trigger
};

std::optional<WriteReason> decideWrite(const WritePolicy& policy, const GroupEstimate& current,
                                       const std::optional<LastWritten>& last, const WriteContext& context);

// A stored estimate as the next session's prior: its covariance inflated
// (the car may have been worked on since), floored, and never looser than
// what the config says.
GroupEstimate loadPrior(const GroupEstimate& stored, const EstimatorConfig& config, double inflation);

// How far the current estimate is from what was loaded, in sigmas of both
// together. Large means something moved between sessions.
double distanceFrom(const GroupEstimate& loaded, const GroupEstimate& current);

// Roll, pitch, yaw of R_b_i in the convention the config states it:
// R_b_i = Rz(yaw) Ry(pitch) Rx(roll).
Eigen::Vector3d mountingRpy(const Eigen::Quaterniond& R_b_i);
// Mounting sigma about the body axes, from the IMU-frame tangent covariance.
Eigen::Vector3d mountingSigmaBody(const Eigen::Quaterniond& R_b_i, const Eigen::Matrix3d& cov_i);

}  // namespace vehicle_estimator
