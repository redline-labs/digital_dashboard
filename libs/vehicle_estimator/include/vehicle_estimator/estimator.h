#pragma once

// The vehicle state estimator: an MTi's strapdown increments and a
// dual-antenna receiver's fixes, fused in a fixed-lag smoother on ECEF
// states.
//
// Each GNSS epoch becomes a keyframe -- attitude, position, velocity and both
// IMU biases at that instant -- tied to the previous keyframe by the IMU
// preintegrated between them. The installation -- how the IMU is mounted in
// the body, the IMU-to-antenna lever arm and the antenna baseline's boresight
// -- is learned alongside, as a slow random walk from its prior (see
// calibration.h). Output between keyframes is the newest
// keyframe carried forward through the IMU samples since, so the rate is the
// IMU's while the smoothing is the GNSS's.
//
// Nothing here knows about zenoh or capnp; the node and the offline tool
// decode into measurements.h and call in.

#include "vehicle_estimator/calibration.h"
#include "vehicle_estimator/clock.h"
#include "vehicle_estimator/config.h"
#include "vehicle_estimator/factors.h"
#include "vehicle_estimator/measurements.h"

#include "factor_graph/smoother.h"
#include "imu_preint/imu_factor.h"
#include "imu_preint/sequencer.h"

#include <array>
#include <deque>
#include <functional>
#include <map>
#include <optional>

namespace vehicle_estimator
{

struct EstimatorStatus
{
    bool initialized = false;
    std::uint64_t keyframes = 0;
    std::uint64_t resets = 0;

    std::uint64_t imu_samples = 0;
    std::uint64_t imu_bridged = 0;    // samples filled in across a gap
    std::uint64_t imu_discarded = 0;  // duplicate, out of order, malformed, or before time settled
    std::uint64_t imu_restarts = 0;   // gaps too long to bridge

    std::uint64_t gnss_epochs = 0;
    std::uint64_t gnss_late = 0;       // older than the newest keyframe
    std::uint64_t gnss_timed_out = 0;  // the IMU never covered it
    std::uint64_t gnss_rejected = 0;   // malformed parts removed
    std::uint64_t gated_position = 0, gated_velocity = 0, gated_attitude = 0;
    std::uint64_t updates_refused = 0;

    factor_graph::OptimizeReport last_optimize;
    double last_solve_ms = 0.0;
    std::size_t window_variables = 0, window_factors = 0;

    Eigen::Vector3d lever_arm = Eigen::Vector3d::Zero(), lever_arm_sigma = Eigen::Vector3d::Zero();
    Eigen::Vector2d boresight = Eigen::Vector2d::Zero(), boresight_sigma = Eigen::Vector2d::Zero();
    // R_b_i as roll, pitch, yaw (the config's convention) and its sigma about
    // the body axes.
    Eigen::Vector3d mounting_rpy = Eigen::Vector3d::Zero(), mounting_sigma = Eigen::Vector3d::Zero();
    std::uint64_t calibration_segments = 0;
    // What the mounting has been learned from: seconds of straight, true
    // running, and stops.
    double mount_straight_s = 0.0;
    std::uint64_t mount_level_stops = 0;
    Eigen::Vector3d gyro_bias = Eigen::Vector3d::Zero(), accel_bias = Eigen::Vector3d::Zero();
    std::optional<double> imu_clock_offset;  // gps - imu device time
};

// What one keyframe added to the graph: every factor and new variable,
// never a marginal prior. Handing these to a BatchSmoother reproduces the
// whole run without the fixed-lag window -- the offline smoother.
struct KeyframeRecord
{
    double t = 0.0;
    imu_preint::KeyframeKeys keys;
    factor_graph::FactorList factors;
    factor_graph::Values values;
    std::map<factor_graph::Key, double> stamps;
    // What stateFrom() needs besides the variables: the raw IMU rate and
    // specific force at the keyframe, and the fix it was made from.
    Eigen::Vector3d omega_i = Eigen::Vector3d::Zero();
    Eigen::Vector3d f_i = Eigen::Vector3d::Zero();
    FixQuality fix = FixQuality::none;
    // True for the first keyframe after a (re)start. Its calibration prior
    // restates what the smoother had learned; a batch over the whole drive
    // already has that information and must not count it twice.
    bool start = false;
    // The calibration segment this keyframe's factors use, and when it began.
    std::uint64_t segment = 0;
    double segment_start = 0.0;
};

class Estimator
{
  public:
    explicit Estimator(EstimatorConfig config);

    // A raw MTi sample: sequenced, bridged across small gaps, and mapped onto
    // GPS time through the arrival-time alignment.
    void addImu(const ImuSample& sample);

    // An increment already on GPS time, ending at t_end. For a source that
    // is synchronised, and for tests.
    void addImuIncrement(double t_end, const imu_preint::Increment& inc, double extra_rot_var = 0.0,
                         double extra_vel_var = 0.0, bool bridged = false);

    void addGnss(const GnssEpoch& epoch);

    // Turns every GNSS epoch the IMU now covers into a keyframe. Returns how
    // many were made.
    std::size_t process();

    // Newest keyframe carried forward to the newest IMU sample.
    std::optional<VehicleState> latest() const;
    // The newest keyframe itself, smoothed.
    std::optional<VehicleState> keyframeState() const;

    const EstimatorStatus& status() const { return status_; }
    const factor_graph::FixedLagSmoother& smoother() const { return fls_; }
    const EstimatorConfig& config() const { return config_; }

    void setKeyframeSink(std::function<void(const KeyframeRecord&)> sink) { sink_ = std::move(sink); }

    // The installation prior for the next start, in place of the config's:
    // what an earlier session learned. A reset within this session still
    // carries over what THIS session learned, which takes precedence.
    void seedCalibration(const CalibrationSet& prior) { seeded_ = prior; }
    // The installation as the newest segment has it, with its covariance.
    const std::optional<CalibrationSet>& calibration() const { return calibration_; }

    // Turns a state estimate (one keyframe's variables) into the outputs, for
    // the offline smoother, which has its own estimates of every keyframe.
    VehicleState stateFrom(double t, const Eigen::Quaterniond& R_e_i, const Eigen::Vector3d& p_e,
                           const Eigen::Vector3d& v_e, const Eigen::Vector3d& bg, const Eigen::Vector3d& ba,
                           const Eigen::Vector3d& omega_i, const Eigen::Vector3d& f_i,
                           const Eigen::Quaterniond& R_b_i, const std::optional<Eigen::MatrixXd>& cov_Rpv,
                           const std::optional<Eigen::Matrix3d>& mounting_cov) const;

    static imu_preint::KeyframeKeys keysFor(std::uint64_t index);

  private:
    struct Buffered
    {
        double t0 = 0.0, t1 = 0.0;
        imu_preint::Increment inc;
        double extra_rot_var = 0.0, extra_vel_var = 0.0;
        bool bridged = false;
    };

    struct Newest
    {
        double t = 0.0;
        std::uint64_t index = 0;
        imu_preint::KeyframeKeys keys;
        imu_preint::NavState nav;
        Eigen::Vector3d bg = Eigen::Vector3d::Zero(), ba = Eigen::Vector3d::Zero();
        std::optional<Eigen::MatrixXd> cov_Rpv;  // 9x9, [R, p, v]
        Eigen::Quaterniond R_b_i = Eigen::Quaterniond::Identity();
        std::optional<Eigen::Matrix3d> mounting_cov;
        Eigen::Vector3d omega_i = Eigen::Vector3d::Zero(), f_i = Eigen::Vector3d::Zero();
        FixQuality fix = FixQuality::none;
    };

    void reset();
    bool initialize(const GnssEpoch& e);
    bool addKeyframe(const GnssEpoch& e);
    // Moves every increment ending by t into `pim` (splitting the one that
    // straddles t); returns false when nothing lay in (t_prev, t].
    bool preintegrateTo(double t, imu_preint::Preintegrator& pim);
    void dropIncrementsBefore(double t);
    // Measurement factors for keyframe `keys` at epoch `e`, gated against the
    // predicted state.
    factor_graph::FactorList measurementFactors(const GnssEpoch& e, const imu_preint::KeyframeKeys& keys,
                                                const imu_preint::NavState& predicted, double dt_since_last,
                                                const CalibrationSet& calibration);
    void refreshNewest(const GnssEpoch& e, std::uint64_t index, const imu_preint::KeyframeKeys& keys);
    Eigen::Vector3d rateAt(double t) const;       // raw gyro rate, IMU frame
    Eigen::Vector3d forceAt(double t) const;      // raw specific force, IMU frame
    Eigen::Vector3d gravityAt(const Eigen::Vector3d& p_e) const;

    EstimatorConfig config_;
    factors::Baseline baseline_;
    factor_graph::FixedLagSmoother fls_;
    imu_preint::SampleSequencer sequencer_;
    TimeMapper time_;
    std::optional<double> last_device_time_;

    std::deque<Buffered> imu_;
    std::deque<GnssEpoch> gnss_;
    std::optional<Newest> newest_;
    std::uint64_t next_index_ = 0;

    // The last GNSS velocity seen, for levelling on a moving start: the
    // accelerometer reads the change in velocity minus gravity, not minus
    // gravity alone.
    struct LastVelocity
    {
        double t = 0.0;
        Eigen::Vector3d v_ned = Eigen::Vector3d::Zero();
    };
    std::optional<LastVelocity> last_velocity_;

    // The calibration segment the newest keyframes use.
    struct Segment
    {
        std::uint64_t index = 0;
        double start = 0.0;
        CalibrationKeys keys;
    };
    std::optional<Segment> segment_;
    std::uint64_t next_segment_ = 0;
    // Opens the segment keyframe time t belongs in, if the current one has
    // run its length: new variables started at the old estimate, and the walk
    // factor between them.
    void advanceSegment(double t, factor_graph::FactorList& f, factor_graph::Values& values,
                        std::map<factor_graph::Key, double>& stamps);

    // What the car taught the last run of the smoother about itself. A reset
    // (a gap the IMU cannot bridge) loses the state, not the calibration: it
    // carries into the next start as the prior, ahead of anything seeded.
    std::optional<CalibrationSet> carried_;
    std::optional<CalibrationSet> seeded_;
    std::optional<CalibrationSet> calibration_;

    // The mounting's two pseudo-measurements, gated on how the car is moving
    // (see config.h), for keyframe `k` at time t with the predicted state.
    void mountingFactors(double t, const imu_preint::KeyframeKeys& k, const imu_preint::NavState& predicted,
                         factor_graph::FactorList& f);
    std::optional<double> straight_since_, last_straight_factor_;
    std::optional<double> level_since_, last_level_factor_;
    bool level_counted_ = false;

    // Consecutive gated epochs, per measurement: position, velocity, attitude.
    std::array<std::size_t, 3> gated_run_{};

    EstimatorStatus status_;
    std::function<void(const KeyframeRecord&)> sink_;
};

}  // namespace vehicle_estimator
