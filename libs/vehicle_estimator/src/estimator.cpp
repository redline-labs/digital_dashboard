#include "vehicle_estimator/estimator.h"

#include "geodesy/geodetic.h"
#include "geodesy/gravity.h"
#include "geodesy/wgs84.h"

#include "vehicle_estimator/atmosphere.h"

#include "csym/geo/rot3.h"

#include <Eigen/Eigenvalues>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace vehicle_estimator
{

namespace
{

using factor_graph::Key;
using factor_graph::symbol;
using V3c = csym::Vector3<double>;

const Eigen::Vector3d kOmegaIe(0.0, 0.0, geodesy::wgs84::kOmegaIe);
constexpr double kMinKeyframeSpacing = 1e-3;  // s
constexpr double kAnchorSigma = 0.01;         // m: the anchor is the frame's origin

V3c toC(const Eigen::Vector3d& v)
{
    return V3c{v.x(), v.y(), v.z()};
}

Eigen::Vector3d toE(const V3c& v)
{
    return {v[0], v[1], v[2]};
}

csym::Vector<double, 9> toC9(const Vector9d& v)
{
    csym::Vector<double, 9> out;
    for (std::size_t i = 0; i < 9; ++i) out[i] = v[static_cast<Eigen::Index>(i)];
    return out;
}

Vector9d toE9(const csym::Vector<double, 9>& v)
{
    Vector9d out;
    for (std::size_t i = 0; i < 9; ++i) out[static_cast<Eigen::Index>(i)] = v[i];
    return out;
}

csym::Rot3<double> toC(const Eigen::Quaterniond& q)
{
    const Eigen::Quaterniond n = q.normalized();
    return {n.x(), n.y(), n.z(), n.w()};
}

Eigen::Quaterniond toE(const csym::Rot3<double>& r)
{
    return Eigen::Quaterniond(r.w, r.x, r.y, r.z).normalized();
}

Eigen::Matrix3d rotEcefFromNed(double lat, double lon)
{
    const auto r = geodesy::rotEcefFromNed(lat, lon);
    Eigen::Matrix3d m;
    for (Eigen::Index i = 0; i < 3; ++i)
        for (Eigen::Index j = 0; j < 3; ++j) m(i, j) = r(static_cast<std::size_t>(i), static_cast<std::size_t>(j));
    return m;
}

bool finiteCov(const Eigen::Matrix3d& c)
{
    if (!c.allFinite()) return false;
    const Eigen::SelfAdjointEigenSolver<Eigen::Matrix3d> eig(0.5 * (c + c.transpose()));
    return eig.eigenvalues().minCoeff() > 0.0;
}

// ZYX Euler angles of R_n_b: yaw about down, pitch about the new east, roll
// about the body x.
Eigen::Vector3d rollPitchYaw(const Eigen::Matrix3d& R)
{
    const double pitch = std::asin(std::clamp(-R(2, 0), -1.0, 1.0));
    return {std::atan2(R(2, 1), R(2, 2)), pitch, std::atan2(R(1, 0), R(0, 0))};
}

std::size_t fixIndex(FixQuality f)
{
    switch (f)
    {
        case FixQuality::none:
            return 0;
        case FixQuality::autonomous:
            return 1;
        case FixQuality::differential:
            return 2;
        case FixQuality::float_rtk:
            return 3;
        case FixQuality::rtx:
            return 4;
        case FixQuality::fixed_rtk:
            return 5;
    }
    return 0;
}

}  // namespace

// ---- keys ------------------------------------------------------------------

imu_preint::KeyframeKeys Estimator::keysFor(std::uint64_t index)
{
    return {symbol('R', index), symbol('p', index), symbol('v', index), symbol('g', index), symbol('a', index)};
}

// ---- construction and reset ------------------------------------------------

Estimator::Estimator(EstimatorConfig config)
    : config_(std::move(config)),
      baseline_(factors::Baseline::fromAntennas(config_.lever_arm, config_.antenna2_lever_arm)),
      fls_([&] {
          factor_graph::FixedLagParams p;
          p.lag = config_.lag;
          p.lm = config_.lm;
          return p;
      }()),
      sequencer_(config_.max_bridge_samples)
{
    // A segment's variables are stamped when it opens; one longer than the
    // lag would be marginalised while keyframes were still being added to it.
    if (!(config_.calibration_segment > 0.0) || config_.calibration_segment > 0.5 * config_.lag)
        throw std::invalid_argument("calibration_segment must be positive and at most half the lag");
}

void Estimator::reset(bool count)
{
    if ((status_.initialized || anchored_) && calibration_) carried_ = calibration_;
    factor_graph::FixedLagParams p;
    p.lag = config_.lag;
    p.lm = config_.lm;
    fls_ = factor_graph::FixedLagSmoother(p);
    newest_.reset();
    segment_.reset();
    straight_since_.reset();
    last_straight_factor_.reset();
    level_since_.reset();
    last_level_factor_.reset();
    level_counted_ = false;
    gated_run_ = {};
    status_.initialized = false;
    status_.anchored = false;
    anchored_ = false;
    heading_known_ = false;
    // The anchored start's wait counts from here: a restart gets its second
    // to find a fix like power-on does.
    first_imu_.reset();
    if (count) ++status_.resets;
}

// ---- input -------------------------------------------------------------------

void Estimator::addImu(const ImuSample& s)
{
    ++status_.imu_samples;
    const auto seq = sequencer_.push(s.packet_counter, s.sample_time_fine);
    using Kind = imu_preint::SampleSequencer::Kind;
    switch (seq.kind)
    {
        case Kind::duplicate:
        case Kind::out_of_order:
            ++status_.imu_discarded;
            return;
        case Kind::restart:
            // Too much rotation went unseen to bridge: start over from here.
            ++status_.imu_restarts;
            imu_.clear();
            recent_.clear();
            reset();
            last_device_time_ = seq.device_time_s;
            time_.observeImu(seq.device_time_s, s.host_time);
            return;
        case Kind::first:
            last_device_time_ = seq.device_time_s;
            time_.observeImu(seq.device_time_s, s.host_time);
            return;  // no interval to place it in yet
        case Kind::next:
        case Kind::gap:
            break;
    }

    time_.observeImu(seq.device_time_s, s.host_time);
    const double span = seq.device_time_s - *last_device_time_;
    last_device_time_ = seq.device_time_s;
    const auto mapped = time_.imuToGps(seq.device_time_s);
    status_.imu_clock_offset = time_.currentOffset();
    std::optional<double> t_base;
    if (mapped)
    {
        if (provisional_time_) switchTimeBase();
        t_base = *mapped + config_.imu_time_offset;
    }
    else if (config_.anchored_start && !time_.gnssSeen())
    {
        // No receiver yet, so no GPS time: host time, for an anchored start.
        t_base = time_.imuToHost(seq.device_time_s);
        provisional_time_ = t_base.has_value();
    }
    if (!t_base || !(span > 0.0))
    {
        ++status_.imu_discarded;
        return;
    }
    const double t_end = *t_base;

    imu_preint::Increment inc;
    inc.dq = s.dq;
    inc.dv = s.dv;
    const std::uint64_t pieces = seq.missing + 1;
    inc.dt = span / static_cast<double>(pieces);
    if (imu_preint::incrementProblem(inc))
    {
        ++status_.imu_discarded;
        return;
    }

    // The samples a gap lost are stood in for by this one -- the best guess
    // at a constant rate -- and trusted half as far as they move.
    const double rot = imu_preint::rotationVector(inc.dq).norm();
    const double rot_var = std::pow(std::max(0.5 * rot, 1e-3), 2);
    const double vel_var = std::pow(std::max(0.5 * inc.dv.norm(), 0.02), 2);
    for (std::uint64_t k = 0; k < seq.missing; ++k)
    {
        addImuIncrement(t_end - static_cast<double>(seq.missing - k) * inc.dt, inc, rot_var, vel_var, true);
        ++status_.imu_bridged;
    }
    addImuIncrement(t_end, inc);
    if (s.mag_au && s.mag_au->allFinite()) mags_.push_back({t_end, *s.mag_au});
    if (s.pressure_pa && std::isfinite(*s.pressure_pa) && *s.pressure_pa > 0.0)
        pressures_.push_back({t_end, Eigen::Vector3d(*s.pressure_pa, 0.0, 0.0)});
    // Kept a little past the newest keyframe; nothing older is ever used.
    const double keep = (newest_ ? newest_->t : t_end) - 1.0;
    while (!mags_.empty() && mags_.front().t < keep) mags_.pop_front();
    while (!pressures_.empty() && pressures_.front().t < keep) pressures_.pop_front();
}

void Estimator::addImuIncrement(double t_end, const imu_preint::Increment& inc, double extra_rot_var,
                                double extra_vel_var, bool bridged)
{
    if (!std::isfinite(t_end) || imu_preint::incrementProblem(inc))
    {
        ++status_.imu_discarded;
        return;
    }
    Buffered b{t_end - inc.dt, t_end, inc, extra_rot_var, extra_vel_var, bridged};
    if (!imu_.empty())
    {
        const double gap = b.t0 - imu_.back().t1;
        // Behind the last sample: the stream went backwards. Discard.
        if (b.t1 <= imu_.back().t1)
        {
            ++status_.imu_discarded;
            return;
        }
        // Small mismatches between the mapped and integrated times are
        // absorbed so the intervals tile exactly.
        if (std::fabs(gap) < 0.5 * inc.dt) b.t0 = imu_.back().t1;
    }
    imu_.push_back(b);
    status_.imu_buffered = imu_.size();
    if (!first_imu_) first_imu_ = b.t1;
    recent_.push_back({b.t1, inc.dt, inc.dv, imu_preint::rotationVector(inc.dq).norm() / inc.dt});
    while (!recent_.empty() && recent_.front().t1 < b.t1 - 3.0) recent_.pop_front();

    // Before the first keyframe, keep only what initialisation looks at.
    if (!newest_)
        while (!imu_.empty() && imu_.front().t1 < b.t1 - 2.0) imu_.pop_front();
}

void Estimator::addGnss(const GnssEpoch& in)
{
    ++status_.gnss_epochs;
    if (!std::isfinite(in.gps_time))
    {
        ++status_.gnss_rejected;
        return;
    }
    time_.observeGnss(in.gps_time, in.host_time);

    GnssEpoch e = in;
    if (e.position)
    {
        const bool ok = geodesy::isPlausible(geodesy::Llh<double>{e.position->lat, e.position->lon, e.position->h}) &&
                        finiteCov(e.position->cov_ned) && e.fix != FixQuality::none;
        if (!ok)
        {
            e.position.reset();
            ++status_.gnss_rejected;
        }
    }
    if (e.velocity && (!e.velocity->v_ned.allFinite() || e.velocity->v_ned.norm() > 150.0))
    {
        e.velocity.reset();
        ++status_.gnss_rejected;
    }
    if (e.attitude)
    {
        const bool ok = std::isfinite(e.attitude->yaw) && std::isfinite(e.attitude->pitch) &&
                        std::fabs(e.attitude->pitch) < 1.5 &&
                        (!e.attitude->cov || (e.attitude->cov->allFinite() && e.attitude->cov->determinant() > 0.0 &&
                                              (*e.attitude->cov)(0, 0) > 0.0));
        if (!ok)
        {
            e.attitude.reset();
            ++status_.gnss_rejected;
        }
    }
    if (newest_ && e.gps_time <= newest_->t + kMinKeyframeSpacing)
    {
        ++status_.gnss_late;
        return;
    }
    const auto at = std::upper_bound(gnss_.begin(), gnss_.end(), e.gps_time,
                                     [](double t, const GnssEpoch& g) { return t < g.gps_time; });
    if (at != gnss_.begin() && std::prev(at)->gps_time == e.gps_time)
    {
        ++status_.gnss_late;  // a second copy of an epoch already queued
        return;
    }
    gnss_.insert(at, std::move(e));
}

// ---- processing ----------------------------------------------------------------

std::size_t Estimator::process()
{
    std::size_t made = 0;
    for (;;)
    {
        // A keyframe on the IMU's clock when no GNSS epoch is due to make one:
        // an outage then still has keyframes for the IMU chain to run through,
        // for whatever other measurements there are, and for the covariance
        // to grow on. It waits until GNSS is plainly absent -- as long as an
        // epoch may be late before it is given up on -- so a late or
        // out-of-order epoch is never pre-empted. The output does not wait:
        // latest() carries the newest keyframe forward on the IMU regardless.
        if (newest_)
        {
            // On the interval's grid in GPS time, so that when GNSS returns its
            // epochs land ON inertial keyframe times instead of a hair after.
            const double t_next =
                std::round((newest_->t + config_.keyframe_interval) / config_.keyframe_interval) *
                config_.keyframe_interval;
            const bool epoch_due = !gnss_.empty() && gnss_.front().gps_time <= t_next + 0.5 * config_.keyframe_interval;
            const bool covered = !imu_.empty() && imu_.back().t1 >= t_next + config_.max_gnss_wait;
            if (!epoch_due && covered)
            {
                if (addKeyframe(t_next, nullptr)) ++made;
                continue;
            }
        }
        if (gnss_.empty()) break;
        const GnssEpoch& e = gnss_.front();
        const bool covered = !imu_.empty() && imu_.back().t1 >= e.gps_time + config_.gnss_reorder_window;
        if (!covered)
        {
            // The IMU has stalled or is behind: give up on an epoch once a
            // newer one is max_gnss_wait ahead of it.
            if (gnss_.back().gps_time - e.gps_time > config_.max_gnss_wait)
            {
                ++status_.gnss_timed_out;
                gnss_.pop_front();
                continue;
            }
            break;
        }
        const GnssEpoch epoch = e;
        gnss_.pop_front();
        const bool ok = anchored_ ? reanchor(epoch) : newest_ ? addKeyframe(epoch.gps_time, &epoch) : initialize(epoch);
        if (ok) ++made;
        if (epoch.velocity) last_velocity_ = LastVelocity{epoch.gps_time, epoch.velocity->v_ned};
    }
    // GPS time has arrived under an anchored start: carry it on, in GPS time.
    if (!newest_ && resume_ && first_imu_ && !imu_.empty() && imu_.back().t1 - *first_imu_ >= 0.5 && resumeAnchored())
        ++made;
    // No receiver heard from at all, and the IMU running: start anyway,
    // anchored. (A receiver that is there but has no fix yet gets its turn:
    // its dual-antenna start is the better one.)
    if (!newest_ && !resume_ && config_.anchored_start && !time_.gnssSeen() && first_imu_ && !imu_.empty() &&
        imu_.back().t1 - *first_imu_ >= config_.anchor_wait && anchoredStart(imu_.back().t1))
        ++made;
    return made;
}

bool Estimator::preintegrateTo(double t, imu_preint::Preintegrator& pim)
{
    bool any = false;
    while (!imu_.empty() && imu_.front().t0 < t)
    {
        Buffered& b = imu_.front();
        if (b.t1 <= t + 1e-9)
        {
            pim.integrate(b.inc, b.extra_rot_var, b.extra_vel_var, b.bridged);
            imu_.pop_front();
            any = true;
            continue;
        }
        const double f = (t - b.t0) / (b.t1 - b.t0);
        const auto [head, tail] = imu_preint::split(b.inc, f, config_.dv_frame);
        if (head.dt > 1e-9)
        {
            pim.integrate(head, b.extra_rot_var * f, b.extra_vel_var * f, b.bridged);
            any = true;
        }
        b.inc = tail;
        b.t0 = t;
        b.extra_rot_var *= 1.0 - f;
        b.extra_vel_var *= 1.0 - f;
        break;
    }
    return any;
}

void Estimator::dropIncrementsBefore(double t)
{
    imu_preint::Preintegrator scratch(config_.imu_noise, config_.dv_frame);
    preintegrateTo(t, scratch);
}

Eigen::Vector3d Estimator::rateAt(double t) const
{
    for (const auto& b : imu_)
        if (b.t1 >= t) return imu_preint::rotationVector(b.inc.dq) / b.inc.dt;
    return imu_.empty() ? Eigen::Vector3d::Zero() : Eigen::Vector3d(imu_preint::rotationVector(imu_.back().inc.dq) / imu_.back().inc.dt);
}

Eigen::Vector3d Estimator::forceAt(double t) const
{
    for (const auto& b : imu_)
        if (b.t1 >= t) return b.inc.dv / b.inc.dt;
    return imu_.empty() ? Eigen::Vector3d::Zero() : Eigen::Vector3d(imu_.back().inc.dv / imu_.back().inc.dt);
}

Eigen::Vector3d Estimator::gravityAt(const Eigen::Vector3d& p_e) const
{
    if (config_.gravity) return toE(config_.gravity->gravityEcef(toC(p_e)));
    return toE(geodesy::normalGravityEcef(toC(p_e)));
}

// ---- initialisation ----------------------------------------------------------

double Estimator::meanForce(double t0, double t1, Eigen::Vector3d& f, Eigen::Vector3d& rate) const
{
    Eigen::Vector3d dv = Eigen::Vector3d::Zero(), dtheta = Eigen::Vector3d::Zero();
    double span = 0.0;
    for (const auto& b : imu_)
    {
        if (b.t1 > t1 + 1e-6 || b.t0 < t0 - 1e-6) continue;
        dv += b.inc.dv;
        dtheta += imu_preint::rotationVector(b.inc.dq);
        span += b.inc.dt;
    }
    if (span <= 0.0) return 0.0;
    f = dv / span;
    rate = dtheta / span;
    return span;
}

bool Estimator::initialize(const GnssEpoch& e)
{
    // A heading is what the IMU cannot give itself; course over ground is not
    // one while the car is sliding. So the first keyframe waits for the
    // dual-antenna solution -- or, after an anchored start, takes the one the
    // anchored graph already has.
    // On any failure the IMU history is trimmed but not emptied: the next
    // epoch needs the half second before it to level.
    const auto give_up = [&] {
        dropIncrementsBefore(e.gps_time - 1.0);
        return false;
    };
    const CalibrationSet cal = carried_ ? *carried_ : seeded_ ? *seeded_ : configuredCalibration(config_);
    const Eigen::Vector3d omega_i = rateAt(e.gps_time);

    if (handover_ && e.position)
    {
        // After an anchored start: attitude and biases from it, rotated into
        // this frame by reanchor(); position and velocity from the fix.
        Start s = *handover_;
        handover_.reset();
        const Eigen::Matrix3d R_e_n = rotEcefFromNed(e.position->lat, e.position->lon);
        const Eigen::Vector3d antenna_e =
            toE(geodesy::llhToEcef(geodesy::Llh<double>{e.position->lat, e.position->lon, e.position->h}));
        s.t = e.gps_time;
        s.p_e = antenna_e - s.R_e_i * cal.lever_arm;
        s.sigma_p = 10.0;
        if (e.velocity)
        {
            s.v_e = R_e_n * e.velocity->v_ned - s.R_e_i * omega_i.cross(cal.lever_arm);
            s.sigma_v = 1.0;
        }
        s.epoch = &e;
        s.omega_i = omega_i;
        s.fix = e.fix;
        return startGraph(s, cal);
    }
    if (!e.position || !e.attitude) return give_up();

    const double g = gravityAt(toE(geodesy::llhToEcef(
                                   geodesy::Llh<double>{e.position->lat, e.position->lon, e.position->h})))
                         .norm();

    // What the accelerometer should read, in NED, and what it did read: the
    // specific force a - g. Parked, a = 0 and this is levelling. Moving, a
    // comes from the change in GNSS velocity over the last epoch, and the
    // accelerometer is averaged over the same interval -- levelling a
    // drifting car on -g alone puts its lateral acceleration into roll.
    Eigen::Vector3d f_i, rate_i;
    const double span = meanForce(e.gps_time - 0.5, e.gps_time, f_i, rate_i);
    if (span < 0.4) return give_up();
    const bool still = std::fabs(f_i.norm() - g) < config_.static_accel_tolerance &&
                       rate_i.norm() < config_.static_rate_tolerance;

    Eigen::Vector3d f_n(0.0, 0.0, -g);  // specific force at rest points up
    double about_baseline = still ? 0.01 : config_.init_roll_sigma_moving;
    if (!still && e.velocity && last_velocity_ && e.gps_time - last_velocity_->t > 0.05 &&
        e.gps_time - last_velocity_->t < 0.3)
    {
        Eigen::Vector3d f_short, rate_short;
        if (meanForce(last_velocity_->t, e.gps_time, f_short, rate_short) > 0.04)
        {
            const Eigen::Vector3d a_n = (e.velocity->v_ned - last_velocity_->v_ned) / (e.gps_time - last_velocity_->t);
            f_n = a_n - Eigen::Vector3d(0.0, 0.0, g);
            f_i = f_short;
            // The body turns while that interval is averaged: a few degrees
            // of roll uncertainty, not the ten of an unlevelled start.
            about_baseline = 0.05;
        }
    }

    // TRIAD: the baseline (precise, from the antennas) and the specific force
    // (from the accelerometer), each known in both frames.
    const Eigen::Vector3d b_i = baseline_.direction(Eigen::Vector2d::Zero());
    const double cy = std::cos(e.attitude->yaw), sy = std::sin(e.attitude->yaw);
    const double cp = std::cos(e.attitude->pitch), sp = std::sin(e.attitude->pitch);
    const Eigen::Vector3d b_n(cp * cy, cp * sy, -sp);
    const Eigen::Vector3d t2_i = b_i.cross(f_i.normalized()), t2_n = b_n.cross(f_n.normalized());
    if (t2_i.norm() < 0.1 || t2_n.norm() < 0.1) return give_up();  // baseline along the force: TRIAD undefined
    Eigen::Matrix3d A_i, A_n;
    A_i << b_i, t2_i.normalized(), b_i.cross(t2_i.normalized());
    A_n << b_n, t2_n.normalized(), b_n.cross(t2_n.normalized());
    const Eigen::Matrix3d R_n_i = A_n * A_i.transpose();
    const Eigen::Matrix3d R_e_n = rotEcefFromNed(e.position->lat, e.position->lon);

    Start s;
    s.t = e.gps_time;
    s.R_e_i = Eigen::Quaterniond(R_e_n * R_n_i);
    const Eigen::Vector3d antenna_e =
        toE(geodesy::llhToEcef(geodesy::Llh<double>{e.position->lat, e.position->lon, e.position->h}));
    s.p_e = antenna_e - s.R_e_i * cal.lever_arm;
    if (e.velocity) s.v_e = R_e_n * e.velocity->v_ned - s.R_e_i * omega_i.cross(cal.lever_arm);
    s.sigma_v = e.velocity ? 1.0 : config_.init_velocity_sigma;
    // Priors deliberately loose wherever this epoch's own measurement
    // factors carry the information -- a tight prior built from the same fix
    // would count it twice. The exception is rotation about the baseline: the
    // antennas cannot see it, so the prior is all there is.
    const double loose = 0.2;  // rad
    s.cov_R = loose * loose * Eigen::Matrix3d::Identity() +
              (about_baseline * about_baseline - loose * loose) * b_i * b_i.transpose();
    s.cov_bg = std::pow(config_.gyro_bias_prior, 2) * Eigen::Matrix3d::Identity();
    s.cov_ba = std::pow(config_.accel_bias_prior, 2) * Eigen::Matrix3d::Identity();
    s.epoch = &e;
    s.omega_i = omega_i;
    s.fix = e.fix;
    return startGraph(s, cal);
}

bool Estimator::startGraph(const Start& st, const CalibrationSet& cal)
{
    dropIncrementsBefore(st.t);

    const std::uint64_t index = next_index_++;
    const auto k = keysFor(index);
    factor_graph::Values values;
    values.insert(k.R, toC(st.R_e_i));
    values.insert(k.p, toC(st.p_e));
    values.insert(k.v, toC(st.v_e));
    values.insert(k.bg, toC(st.bg));
    values.insert(k.ba, toC(st.ba));
    const Segment seg{next_segment_++, st.t, calibrationKeys(next_segment_ - 1)};
    values.insert(seg.keys.mounting, toC(cal.mounting));
    values.insert(seg.keys.lever_arm, toC(cal.lever_arm));
    values.insert(seg.keys.boresight, csym::Vector2<double>{cal.boresight.x(), cal.boresight.y()});
    values.insert(seg.keys.magnetometer, toC9(cal.magnetometer()));
    values.insert(seg.keys.barometer, csym::Vector2<double>{cal.baro_offset, cal.baro_airflow});

    factor_graph::FactorList f;
    f.push_back(factors::priorRot(k.R, st.R_e_i, st.cov_R));
    f.push_back(factors::priorV3(k.p, st.p_e, st.sigma_p * st.sigma_p * Eigen::Matrix3d::Identity(), "position prior"));
    f.push_back(factors::priorV3(k.v, st.v_e, st.sigma_v * st.sigma_v * Eigen::Matrix3d::Identity(), "velocity prior"));
    f.push_back(factors::priorV3(k.bg, st.bg, st.cov_bg, "gyro bias prior"));
    f.push_back(factors::priorV3(k.ba, st.ba, st.cov_ba, "accel bias prior"));
    f.push_back(factors::calibrationPrior(seg.keys, cal));

    imu_preint::NavState predicted{st.R_e_i, st.p_e, st.v_e};
    segment_ = seg;
    if (st.epoch)
    {
        Newest provisional;
        provisional.nav = predicted;
        provisional.omega_i = st.omega_i;
        newest_ = provisional;  // for measurementFactors' lever-arm terms and gating
        const auto meas = measurementFactors(*st.epoch, k, predicted, 0.0, cal);
        newest_.reset();
        f.insert(f.end(), meas.begin(), meas.end());
    }

    std::map<Key, double> stamps;
    for (Key key : {k.R, k.p, k.v, k.bg, k.ba, seg.keys.mounting, seg.keys.lever_arm, seg.keys.boresight,
                    seg.keys.magnetometer, seg.keys.barometer})
        stamps[key] = st.t;

    const auto t0 = std::chrono::steady_clock::now();
    const auto cov_keys = newestCovarianceKeys(k);
    const auto report = fls_.update(f, values, stamps, cov_keys);
    status_.last_solve_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!report.ok)
    {
        ++status_.updates_refused;
        segment_.reset();
        return false;
    }
    status_.last_optimize = report.optimize;
    status_.covariance_from_solve = fls_.solverCache().covariancesFromSolve();
    ++status_.calibration_segments;
    if (sink_)
        sink_(KeyframeRecord{st.t, k, f, values, stamps, st.omega_i, forceAt(st.t), st.fix, true, seg.index, seg.start,
                             st.epoch != nullptr});
    status_.initialized = !anchored_;
    status_.anchored = anchored_;
    ++status_.keyframes;
    refreshNewest(st.t, st.fix, index, k, report.covariance);
    return true;
}

bool Estimator::anchoredStart(double t)
{
    // Level from the accelerometer, as any start does; heading from the
    // magnetometer if its calibration is one that was learned, and otherwise
    // none at all.
    Eigen::Vector3d f_i, rate_i;
    if (meanForce(t - 0.5, t, f_i, rate_i) < 0.4) return false;
    const double lat = config_.anchor_latitude, lon = config_.anchor_longitude;
    const Eigen::Vector3d anchor_e = toE(geodesy::llhToEcef(geodesy::Llh<double>{lat, lon, 0.0}));
    const double g = gravityAt(anchor_e).norm();
    const bool still =
        std::fabs(f_i.norm() - g) < config_.static_accel_tolerance && rate_i.norm() < config_.static_rate_tolerance;

    // The level frame's axes in the IMU frame: down along gravity (against the
    // specific force), north the IMU's x laid flat -- a yaw of zero, to be
    // turned by the heading.
    const Eigen::Vector3d down_i = -f_i.normalized();
    Eigen::Vector3d north_i = Eigen::Vector3d::UnitX() - Eigen::Vector3d::UnitX().dot(down_i) * down_i;
    if (north_i.norm() < 0.1) north_i = Eigen::Vector3d::UnitY() - Eigen::Vector3d::UnitY().dot(down_i) * down_i;
    north_i.normalize();
    Eigen::Matrix3d R_i_n;
    R_i_n << north_i, down_i.cross(north_i), down_i;
    Eigen::Matrix3d R_n_i = R_i_n.transpose();

    double yaw_sigma = 3.0;  // unknown
    heading_known_ = false;
    const CalibrationSet cal = carried_ ? *carried_ : seeded_ ? *seeded_ : configuredCalibration(config_);
    if (config_.use_magnetometer && mag_trusted_)
    {
        const Reading* nearest = nullptr;
        for (const auto& m : mags_)
            if (!nearest || std::fabs(m.t - t) < std::fabs(nearest->t - t)) nearest = &m;
        if (nearest && std::fabs(nearest->t - t) < 0.5)
        {
            const Eigen::Vector3d c_n = R_n_i * (cal.softIronMatrix().inverse() * (nearest->value - cal.mag_hard_iron));
            const double psi = -std::atan2(c_n.y(), c_n.x());
            R_n_i = Eigen::AngleAxisd(psi, Eigen::Vector3d::UnitZ()).toRotationMatrix() * R_n_i;
            yaw_sigma = config_.mag_heading_sigma;
            heading_known_ = true;
        }
    }

    Start s;
    s.t = t;
    s.R_e_i = Eigen::Quaterniond(rotEcefFromNed(lat, lon) * R_n_i);
    const double level = still ? 0.01 : config_.init_roll_sigma_moving;
    s.cov_R = level * level * (Eigen::Matrix3d::Identity() - down_i * down_i.transpose()) +
              yaw_sigma * yaw_sigma * down_i * down_i.transpose();
    s.p_e = anchor_e;
    // The anchor is the origin of the frame it defines, so it is known
    // exactly; what moves is the car, through the IMU. A loose prior here
    // would be no more honest and is numerically fatal: the IMU chain holds
    // each keyframe's position to ~1e11 and a 10 m prior is 0.01, a ratio
    // no double-precision factorisation can resolve, and the window then
    // has no covariance at all.
    s.sigma_p = kAnchorSigma;
    s.sigma_v = still ? 0.1 : config_.init_velocity_sigma;
    s.cov_bg = std::pow(config_.gyro_bias_prior, 2) * Eigen::Matrix3d::Identity();
    s.cov_ba = std::pow(config_.accel_bias_prior, 2) * Eigen::Matrix3d::Identity();
    s.omega_i = rateAt(t);
    anchored_ = true;
    if (!startGraph(s, cal))
    {
        anchored_ = false;
        heading_known_ = false;
        return false;
    }
    return true;
}

void Estimator::switchTimeBase()
{
    // Everything stamped so far is host time, and nothing of it can be
    // preintegrated into GPS time. What carries over is what is not a time:
    // the attitude in the anchor's frame, the biases, the calibration.
    if (anchored_ && newest_)
    {
        Start s;
        s.R_e_i = newest_->nav.R_e_b;
        s.cov_R = newest_->cov_Rpv ? Eigen::Matrix3d(newest_->cov_Rpv->block<3, 3>(0, 0))
                                   : Eigen::Matrix3d(std::pow(config_.mag_heading_sigma, 2) * Eigen::Matrix3d::Identity());
        s.bg = newest_->bg;
        s.ba = newest_->ba;
        s.cov_bg = std::pow(config_.gyro_bias_prior, 2) * Eigen::Matrix3d::Identity();
        s.cov_ba = std::pow(config_.accel_bias_prior, 2) * Eigen::Matrix3d::Identity();
        const std::array<Key, 2> biases{newest_->keys.bg, newest_->keys.ba};
        if (const auto cov = fls_.jointCovariance(biases))
        {
            s.cov_bg = cov->block<3, 3>(0, 0);
            s.cov_ba = cov->block<3, 3>(3, 3);
        }
        const bool heading = heading_known_;
        reset(false);
        heading_known_ = heading;  // reset() forgets it; resumeAnchored() reads it
        resume_ = s;
    }
    imu_.clear();
    recent_.clear();
    mags_.clear();
    pressures_.clear();
    last_velocity_.reset();
    provisional_time_ = false;
    ++time_base_switches_;
}

bool Estimator::resumeAnchored()
{
    Start s = *resume_;
    const double t = imu_.back().t1;
    Eigen::Vector3d f_i, rate_i;
    const bool have = meanForce(t - 0.5, t, f_i, rate_i) > 0.4;
    const double g = gravityAt(toE(geodesy::llhToEcef(
                                   geodesy::Llh<double>{config_.anchor_latitude, config_.anchor_longitude, 0.0})))
                         .norm();
    const bool still = have && std::fabs(f_i.norm() - g) < config_.static_accel_tolerance &&
                       rate_i.norm() < config_.static_rate_tolerance;
    s.t = t;
    s.p_e = toE(geodesy::llhToEcef(geodesy::Llh<double>{config_.anchor_latitude, config_.anchor_longitude, 0.0}));
    s.sigma_p = kAnchorSigma;
    s.v_e = Eigen::Vector3d::Zero();
    s.sigma_v = still ? 0.1 : config_.init_velocity_sigma;
    s.omega_i = rateAt(t);
    s.epoch = nullptr;
    const bool heading = heading_known_;
    const CalibrationSet cal = carried_ ? *carried_ : seeded_ ? *seeded_ : configuredCalibration(config_);
    anchored_ = true;
    heading_known_ = heading;
    resume_.reset();
    if (!startGraph(s, cal))
    {
        anchored_ = false;
        heading_known_ = false;
        return false;
    }
    return true;
}

bool Estimator::reanchor(const GnssEpoch& e)
{
    if (!e.position) return false;
    // Without a heading the anchored attitude has nothing to hand over that
    // the antennas cannot do better: wait for them, and start as usual.
    if (!heading_known_)
    {
        if (!e.attitude) return false;
        reset(false);
        return initialize(e);
    }
    // The anchored state at exactly the epoch. An epoch older than the
    // anchored graph's newest keyframe is passed over: the IMU before it is
    // already spent, and the next epoch will be newer.
    if (e.gps_time < newest_->t + kMinKeyframeSpacing) return false;
    if (!addKeyframe(e.gps_time, nullptr)) return false;
    const Newest a = *newest_;
    // ...rotated from the anchor's level frame, whose x is magnetic north,
    // into the fix's, whose x is true north: magnetic north lies the
    // declination east of true.
    const MagneticField field = magneticField(e.position->lat, e.position->lon, e.position->h, e.gps_time);
    const Eigen::Matrix3d T = rotEcefFromNed(e.position->lat, e.position->lon) *
                              Eigen::AngleAxisd(field.declination, Eigen::Vector3d::UnitZ()).toRotationMatrix() *
                              rotEcefFromNed(config_.anchor_latitude, config_.anchor_longitude).transpose();
    Start h;
    h.R_e_i = Eigen::Quaterniond(T * a.nav.R_e_b.toRotationMatrix());
    // The attitude's tangent is a right perturbation, in the IMU frame: a
    // rotation applied on the left leaves its covariance as it was.
    h.cov_R = a.cov_Rpv ? Eigen::Matrix3d(a.cov_Rpv->block<3, 3>(0, 0))
                        : Eigen::Matrix3d(std::pow(config_.mag_heading_sigma, 2) * Eigen::Matrix3d::Identity());
    h.v_e = T * a.nav.v_e;
    h.sigma_v = 1.0;
    h.bg = a.bg;
    h.ba = a.ba;
    const std::array<Key, 2> biases{a.keys.bg, a.keys.ba};
    if (const auto cov = fls_.jointCovariance(biases))
    {
        h.cov_bg = cov->block<3, 3>(0, 0);
        h.cov_ba = cov->block<3, 3>(3, 3);
    }
    else
    {
        h.cov_bg = std::pow(config_.gyro_bias_prior, 2) * Eigen::Matrix3d::Identity();
        h.cov_ba = std::pow(config_.accel_bias_prior, 2) * Eigen::Matrix3d::Identity();
    }
    // The anchor's gravity and earth rate were not these: loosen the biases
    // by what that could have put into them.
    h.cov_ba += std::pow(0.03, 2) * Eigen::Matrix3d::Identity();
    h.cov_bg += std::pow(1e-4, 2) * Eigen::Matrix3d::Identity();
    reset(false);
    handover_ = h;
    ++status_.reanchors;
    return initialize(e);
}

// ---- measurement factors -----------------------------------------------------------

factor_graph::FactorList Estimator::measurementFactors(const GnssEpoch& e, const imu_preint::KeyframeKeys& k,
                                                       const imu_preint::NavState& predicted, double dt,
                                                       const CalibrationSet& cal)
{
    factor_graph::FactorList out;
    const CalibrationKeys& ck = segment_->keys;
    const double scale = config_.fix_sigma_scale[fixIndex(e.fix)];
    const Eigen::Vector3d& la = cal.lever_arm;
    const Eigen::Vector3d bg = newest_ ? newest_->bg : Eigen::Vector3d::Zero();
    const Eigen::Matrix3d R_e_i = predicted.R_e_b.toRotationMatrix();

    // Predicted uncertainty for gating: the last keyframe's, grown over dt.
    Eigen::Matrix3d P_p = 25.0 * Eigen::Matrix3d::Identity(), P_v = 4.0 * Eigen::Matrix3d::Identity();
    if (newest_ && newest_->cov_Rpv)
    {
        P_p = newest_->cov_Rpv->block<3, 3>(3, 3) + dt * dt * newest_->cov_Rpv->block<3, 3>(6, 6);
        P_v = newest_->cov_Rpv->block<3, 3>(6, 6);
    }
    // What the car can do between keyframes that the IMU prediction already
    // covers only if the biases are right: a generous allowance.
    P_p += std::pow(0.5 * dt * dt * 2.0 + 0.05, 2) * Eigen::Matrix3d::Identity();
    P_v += std::pow(dt * 2.0 + 0.05, 2) * Eigen::Matrix3d::Identity();

    const auto gate = [&](std::size_t which, const Eigen::VectorXd& nu, const Eigen::MatrixXd& S) {
        const Eigen::LLT<Eigen::MatrixXd> llt(S);
        const bool inside = llt.info() == Eigen::Success &&
                            nu.dot(llt.solve(nu)) <= config_.gate_sigmas * config_.gate_sigmas;
        if (inside || gated_run_[which] >= config_.gate_lockout)
        {
            gated_run_[which] = 0;
            return true;
        }
        ++gated_run_[which];
        return false;
    };

    if (e.position && scale > 0.0)
    {
        const Eigen::Matrix3d R_e_n = rotEcefFromNed(e.position->lat, e.position->lon);
        const Eigen::Vector3d meas =
            toE(geodesy::llhToEcef(geodesy::Llh<double>{e.position->lat, e.position->lon, e.position->h}));
        const Eigen::Matrix3d cov_ned =
            scale * scale * e.position->cov_ned + std::pow(config_.position_sigma_floor, 2) * Eigen::Matrix3d::Identity();
        const Eigen::Matrix3d cov_e = R_e_n * cov_ned * R_e_n.transpose();
        const Eigen::Vector3d nu = meas - (predicted.p_e + R_e_i * la);
        if (gate(0, nu, cov_e + P_p))
            out.push_back(factors::gnssPosition(k.R, k.p, ck.lever_arm, meas, cov_e, config_.robust_delta));
        else
            ++status_.gated_position;
    }

    if (e.velocity && scale > 0.0)
    {
        const double lat = e.position ? e.position->lat : 0.0, lon = e.position ? e.position->lon : 0.0;
        const auto llh = geodesy::ecefToLlh(toC(predicted.p_e));
        const Eigen::Matrix3d R_e_n = e.position ? rotEcefFromNed(lat, lon) : rotEcefFromNed(llh.lat, llh.lon);
        const Eigen::Vector3d meas = R_e_n * e.velocity->v_ned;
        const Eigen::Vector3d sig(config_.velocity_sigma_horizontal, config_.velocity_sigma_horizontal,
                                  config_.velocity_sigma_vertical);
        const Eigen::Matrix3d cov_e = R_e_n * Eigen::Matrix3d((scale * sig).cwiseAbs2().asDiagonal()) * R_e_n.transpose();
        // Rate relative to the earth, bias removed.
        const Eigen::Vector3d omega = rateAt(e.gps_time) - bg - R_e_i.transpose() * kOmegaIe;
        const Eigen::Vector3d nu = meas - (predicted.v_e + R_e_i * omega.cross(la));
        if (gate(1, nu, cov_e + P_v))
            out.push_back(factors::gnssVelocity(k.R, k.v, ck.lever_arm, meas, cov_e, omega, config_.robust_delta));
        else
            ++status_.gated_velocity;
    }

    if (e.attitude)
    {
        const auto llh = geodesy::ecefToLlh(toC(predicted.p_e));
        const Eigen::Matrix3d R_n_e = rotEcefFromNed(llh.lat, llh.lon).transpose();
        Eigen::Matrix2d cov = Eigen::Vector2d(config_.attitude_sigma_yaw * config_.attitude_sigma_yaw,
                                              config_.attitude_sigma_pitch * config_.attitude_sigma_pitch)
                                  .asDiagonal();
        if (e.attitude->cov) cov = *e.attitude->cov;
        const Eigen::Vector2d& bs = cal.boresight;
        const Eigen::Vector3d b_n = R_n_e * R_e_i * baseline_.direction(bs);
        const double yaw = std::atan2(b_n.y(), b_n.x());
        const double pitch = std::atan2(-b_n.z(), std::hypot(b_n.x(), b_n.y()));
        const double dyaw = std::remainder(yaw - e.attitude->yaw, 2.0 * std::numbers::pi);
        Eigen::Vector2d nu(dyaw, pitch - e.attitude->pitch);
        // The prediction's own attitude uncertainty, in round terms.
        const Eigen::Matrix2d S = cov + std::pow(0.02 + 0.01 * dt, 2) * Eigen::Matrix2d::Identity();
        if (gate(2, nu, S))
        {
            out.push_back(factors::dualAntenna(k.R, ck.boresight, R_n_e, baseline_, e.attitude->yaw,
                                               e.attitude->pitch, cov, config_.robust_delta));
            last_dual_antenna_ = e.gps_time;
        }
        else
        {
            ++status_.gated_attitude;
        }
    }
    return out;
}

// ---- keyframes ------------------------------------------------------------------------

bool Estimator::addKeyframe(double t, const GnssEpoch* e)
{
    const Newest last = *newest_;
    // An interval of a hair has a singular preintegrated covariance; an epoch
    // that close behind the newest keyframe is late, not a new keyframe.
    if (t - last.t < kMinKeyframeSpacing)
    {
        ++status_.gnss_late;
        return false;
    }
    imu_preint::Preintegrator pim(config_.imu_noise, config_.dv_frame, last.bg, last.ba);
    if (!preintegrateTo(t, pim))
    {
        ++status_.gnss_late;
        return false;
    }
    const double dt = pim.result().dt;
    const Eigen::Vector3d g = gravityAt(last.nav.p_e);
    const imu_preint::NavState predicted = imu_preint::predict(last.nav, last.bg, last.ba, pim.result(), g, kOmegaIe);

    const std::uint64_t index = next_index_++;
    const auto k = keysFor(index);
    factor_graph::Values values;
    values.insert(k.R, toC(predicted.R_e_b));
    values.insert(k.p, toC(predicted.p_e));
    values.insert(k.v, toC(predicted.v_e));
    values.insert(k.bg, toC(last.bg));
    values.insert(k.ba, toC(last.ba));

    std::map<Key, double> stamps;
    for (Key key : {k.R, k.p, k.v, k.bg, k.ba}) stamps[key] = t;

    factor_graph::FactorList f;
    f.push_back(imu_preint::makeImuFactor(last.keys, k, pim.result(), g, kOmegaIe));
    f.push_back(imu_preint::makeBiasWalkFactor(last.keys.bg, k.bg, config_.gyro_bias_walk, dt, "gyro bias walk"));
    f.push_back(imu_preint::makeBiasWalkFactor(last.keys.ba, k.ba, config_.accel_bias_walk, dt, "accel bias walk"));
    advanceSegment(t, f, values, stamps);
    mountingFactors(t, k, predicted, f);
    zeroVelocity(t, k, predicted, f);
    baroFactor(last.t, t, k, predicted, f);
    magFactor(last.t, t, k, predicted, f);
    const std::size_t n_imu = f.size();
    if (e)
    {
        const auto meas = measurementFactors(*e, k, predicted, dt, *calibration_);
        f.insert(f.end(), meas.begin(), meas.end());
    }
    else
    {
        ++status_.inertial_keyframes;
    }

    const auto t0 = std::chrono::steady_clock::now();
    const auto cov_keys = newestCovarianceKeys(k);
    auto report = fls_.update(f, values, stamps, cov_keys);
    if (!report.ok)
    {
        // A measurement the checks let through but the factor refused: keep
        // the IMU chain unbroken without it.
        ++status_.updates_refused;
        f.resize(n_imu);
        report = fls_.update(f, values, stamps, cov_keys);
        if (!report.ok)
        {
            reset();
            return false;
        }
    }
    status_.last_solve_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    status_.last_optimize = report.optimize;
    status_.covariance_from_solve = fls_.solverCache().covariancesFromSolve();
    const FixQuality fix = e ? e->fix : last.fix;
    if (sink_)
        sink_(KeyframeRecord{t, k, f, values, stamps, rateAt(t), forceAt(t), fix, false, segment_->index,
                             segment_->start, e != nullptr});
    ++status_.keyframes;
    refreshNewest(t, fix, index, k, report.covariance);
    return true;
}

void Estimator::baroFactor(double t_prev, double t, const imu_preint::KeyframeKeys& k,
                           const imu_preint::NavState& predicted, factor_graph::FactorList& f)
{
    if (!config_.use_barometer) return;
    // The mean over the interval: at 50 Hz, five readings a keyframe, which
    // also averages away the device's whole-pascal quantisation.
    double sum = 0.0;
    std::size_t n = 0;
    for (const auto& r : pressures_)
        if (r.t > t_prev && r.t <= t)
        {
            sum += r.value.x();
            ++n;
        }
    if (n == 0) return;
    const double pressure = sum / static_cast<double>(n);
    const auto llh = geodesy::ecefToLlh(toC(predicted.p_e));
    const Eigen::Vector3d up = -rotEcefFromNed(llh.lat, llh.lon).col(2);
    const double rho = isa::density(isa::altitude(pressure));
    f.push_back(factors::baroHeight(k.p, k.v, segment_->keys.barometer, pressure, predicted.p_e, up, llh.h, rho,
                                    config_.baro_sigma, config_.robust_delta));
    ++status_.baro_factors;
    if (predicted.v_e.norm() > 5.0) status_.baro_moving_s += t - t_prev;
    status_.baro_height = isa::altitude(pressure - calibration_->baro_airflow * 0.5 * rho * predicted.v_e.squaredNorm()) +
                          calibration_->baro_offset;
}

void Estimator::magFactor(double t_prev, double t, const imu_preint::KeyframeKeys& k,
                          const imu_preint::NavState& predicted, factor_graph::FactorList& f)
{
    if (!config_.use_magnetometer || mags_.empty()) return;
    // The reading nearest the keyframe -- not a mean: the car turns a few
    // degrees within one keyframe interval.
    const Reading* nearest = nullptr;
    for (const auto& r : mags_)
        if (!nearest || std::fabs(r.t - t) < std::fabs(nearest->t - t)) nearest = &r;
    if (std::fabs(nearest->t - t) > 0.5 * config_.keyframe_interval) return;

    if (anchored_)
    {
        // No position, so no reference field: heading only, against magnetic
        // north, and only from a calibration that was learned. The gate can
        // judge only the strength, against what the calibration expects.
        if (!heading_known_) return;
        const CalibrationSet& cal = *calibration_;
        const Eigen::Vector3d corrected = cal.softIronMatrix().inverse() * (nearest->value - cal.mag_hard_iron);
        if (std::fabs(corrected.norm() - 1.0) > 0.15)
        {
            ++status_.mag_rejected;
            return;
        }
        const Eigen::Matrix3d R_n_e = rotEcefFromNed(config_.anchor_latitude, config_.anchor_longitude).transpose();
        f.push_back(factors::magneticHeading(k.R, corrected, R_n_e, 0.0, config_.mag_heading_sigma,
                                             config_.robust_delta));
        ++status_.mag_used;
        last_mag_used_ = t;
        return;
    }

    const auto llh = geodesy::ecefToLlh(toC(predicted.p_e));
    const MagneticField& field = magnetic_.at(llh.lat, llh.lon, llh.h, t);
    const Eigen::Matrix3d R_e_n = rotEcefFromNed(llh.lat, llh.lon);

    // The disturbance gate: the reading against its prediction, in sigmas of
    // everything the prediction does not know -- the attitude, the whole
    // calibration (with its correlations), the sensor. What a car does not
    // yet know about its magnetometer widens the gate only where it does not
    // know it: a poorly learned vertical hard iron opens it vertically and
    // nowhere else. So a calibration that is still its prior still learns
    // (the gate is wide), and a steel bridge beside a learned one does not
    // get in (it is narrow where the bridge pushes).
    const CalibrationSet& cal = *calibration_;
    const Eigen::Matrix3d R_e_i = predicted.R_e_b.toRotationMatrix();
    const Eigen::Vector3d b = R_e_i.transpose() * (R_e_n * field.ned_nt);  // nT, IMU frame
    const double invF = 1.0 / field.intensity_nt;
    const Eigen::Matrix3d A = cal.softIronMatrix() * invF;
    const Eigen::Vector3d nu = nearest->value - (A * b + cal.mag_hard_iron);
    Eigen::Matrix<double, 3, 9> J_k = Eigen::Matrix<double, 3, 9>::Zero();
    J_k.leftCols<3>() = Eigen::Matrix3d::Identity();
    J_k.rightCols<6>() << b.x(), 0.0, 0.0, b.y(), b.z(), 0.0,  //
        0.0, b.y(), 0.0, b.x(), 0.0, b.z(),                     //
        0.0, 0.0, b.z(), 0.0, b.x(), b.y();
    J_k.rightCols<6>() *= invF;
    Eigen::Matrix3d skew_b;
    skew_b << 0.0, -b.z(), b.y(), b.z(), 0.0, -b.x(), -b.y(), b.x(), 0.0;
    const Eigen::Matrix3d J_R = A * skew_b;  // R_e_i Exp(d): b -> b + b x d
    Eigen::Matrix3d S = std::pow(config_.mag_sigma, 2) * Eigen::Matrix3d::Identity() +
                        J_k * cal.magnetometerCov() * J_k.transpose();
    if (newest_ && newest_->cov_Rpv) S += J_R * newest_->cov_Rpv->block<3, 3>(0, 0) * J_R.transpose();
    const Eigen::LLT<Eigen::Matrix3d> llt(S);
    if (llt.info() != Eigen::Success || nu.dot(llt.solve(nu)) > std::pow(config_.mag_gate_sigmas, 2))
    {
        ++status_.mag_rejected;
        return;
    }
    f.push_back(factors::magnetometer(k.R, segment_->keys.magnetometer, nearest->value, R_e_n * field.ned_nt,
                                      field.intensity_nt, config_.mag_sigma, config_.robust_delta));
    ++status_.mag_used;
    last_mag_used_ = t;
    if (last_dual_antenna_ && t - *last_dual_antenna_ < 1.0) status_.mag_learning_s += t - t_prev;
    // Half a minute of learning against the antennas and the calibration is
    // one to take a heading from, should this power cycle need it.
    if (status_.mag_learning_s > config_.mag_trust_after) mag_trusted_ = true;
}

void Estimator::zeroVelocity(double t, const imu_preint::KeyframeKeys& k, const imu_preint::NavState& predicted,
                             factor_graph::FactorList& f)
{
    // An IMU alone cannot tell a parked car from one cruising straight at a
    // steady speed -- both read gravity and no rotation. So stillness needs
    // the estimate's own speed to agree, which through an outage of tens of
    // seconds it does.
    if (!imuStill(t, config_.zero_velocity_hold) || predicted.v_e.norm() > config_.zero_velocity_max_speed) return;
    f.push_back(factors::zeroVelocity(k.v, config_.zero_velocity_sigma, config_.robust_delta));
    ++status_.zero_velocity_updates;
}

bool Estimator::imuStill(double t, double hold) const
{
    // Over the last `hold` seconds of IMU up to t: the mean specific force
    // within tolerance of gravity, and every sample's rate small.
    Eigen::Vector3d dv = Eigen::Vector3d::Zero();
    double span = 0.0;
    for (auto it = recent_.rbegin(); it != recent_.rend(); ++it)
    {
        if (it->t1 > t + 1e-6) continue;
        if (it->t1 < t - hold) break;
        if (it->rate > config_.static_rate_tolerance) return false;
        dv += it->dv;
        span += it->dt;
    }
    if (span < 0.9 * hold) return false;
    const double g = newest_ ? gravityAt(newest_->nav.p_e).norm() : 9.80665;
    return std::fabs((dv / span).norm() - g) < config_.static_accel_tolerance;
}

void Estimator::advanceSegment(double t, factor_graph::FactorList& f, factor_graph::Values& values,
                               std::map<Key, double>& stamps)
{
    if (t - segment_->start < config_.calibration_segment) return;
    const Segment next{next_segment_++, t, calibrationKeys(next_segment_ - 1)};
    // Started where the old segment is now: the walk says it has barely moved.
    const CalibrationSet& at = *calibration_;
    values.insert(next.keys.mounting, toC(at.mounting));
    values.insert(next.keys.lever_arm, toC(at.lever_arm));
    values.insert(next.keys.boresight, csym::Vector2<double>{at.boresight.x(), at.boresight.y()});
    values.insert(next.keys.magnetometer, toC9(at.magnetometer()));
    values.insert(next.keys.barometer, csym::Vector2<double>{at.baro_offset, at.baro_airflow});
    f.push_back(factors::calibrationWalk(segment_->keys, next.keys, t - segment_->start,
                                         calibrationWalkDensities(config_)));
    for (Key key : {next.keys.mounting, next.keys.lever_arm, next.keys.boresight, next.keys.magnetometer,
                    next.keys.barometer})
        stamps[key] = t;
    segment_ = next;
    ++status_.calibration_segments;
}

void Estimator::mountingFactors(double t, const imu_preint::KeyframeKeys& k, const imu_preint::NavState& predicted,
                                factor_graph::FactorList& f)
{
    // Judged from the prediction, through the mounting as it now stands: a
    // few degrees of mounting error change none of these by enough to matter.
    const Eigen::Vector3d omega_raw = rateAt(t), f_raw = forceAt(t);
    const VehicleState s = stateFrom(t, predicted.R_e_b, predicted.p_e, predicted.v_e, newest_->bg, newest_->ba,
                                     omega_raw, f_raw, calibration_->mounting, std::nullopt, std::nullopt);
    const Key mounting = segment_->keys.mounting;
    const double dt = t - newest_->t;

    const bool straight = s.v_body.x() > config_.straight_min_speed &&
                          std::fabs(s.rate_body.z()) < config_.straight_max_yaw_rate &&
                          std::fabs(s.accel_body.y()) < config_.straight_max_lateral_accel;
    if (!straight)
        straight_since_.reset();
    else if (!straight_since_)
        straight_since_ = t;
    if (straight && t - *straight_since_ >= config_.straight_hold)
    {
        status_.mount_straight_s += dt;
        if (!last_straight_factor_ || t - *last_straight_factor_ >= config_.straight_interval)
        {
            const Eigen::Matrix3d R_e_i = predicted.R_e_b.toRotationMatrix();
            const Eigen::Vector3d omega = omega_raw - newest_->bg - R_e_i.transpose() * kOmegaIe;
            f.push_back(factors::straightDriving(k.R, k.v, mounting, omega, config_.reference_point,
                                                 config_.straight_sigma, config_.straight_sigma, config_.robust_delta));
            last_straight_factor_ = t;
        }
    }

    const bool still = s.v_body.norm() < config_.level_max_speed && s.rate_body.norm() < config_.level_max_rate;
    if (!still)
    {
        level_since_.reset();
        level_counted_ = false;
    }
    else if (!level_since_)
    {
        level_since_ = t;
    }
    if (still && t - *level_since_ >= config_.level_hold &&
        (!last_level_factor_ || !level_counted_ || t - *last_level_factor_ >= config_.level_interval))
    {
        const auto llh = geodesy::ecefToLlh(toC(predicted.p_e));
        f.push_back(factors::stationaryLevel(k.R, mounting, rotEcefFromNed(llh.lat, llh.lon).transpose(),
                                             config_.level_sigma, config_.robust_delta));
        last_level_factor_ = t;
        if (!level_counted_) ++status_.mount_level_stops;
        level_counted_ = true;
    }
}

std::array<Key, 8> Estimator::newestCovarianceKeys(const imu_preint::KeyframeKeys& k) const
{
    const CalibrationKeys& ck = segment_->keys;
    return {k.R, k.p, k.v, ck.mounting, ck.lever_arm, ck.boresight, ck.magnetometer, ck.barometer};
}

void Estimator::refreshNewest(double t, FixQuality fix, std::uint64_t index, const imu_preint::KeyframeKeys& k,
                              const std::optional<Eigen::MatrixXd>& cov)
{
    const auto& est = fls_.estimate();
    Newest n;
    n.t = t;
    n.index = index;
    n.keys = k;
    n.nav.R_e_b = toE(est.at<csym::Rot3<double>>(k.R));
    n.nav.p_e = toE(est.at<V3c>(k.p));
    n.nav.v_e = toE(est.at<V3c>(k.v));
    n.bg = toE(est.at<V3c>(k.bg));
    n.ba = toE(est.at<V3c>(k.ba));
    n.omega_i = rateAt(t);
    n.f_i = forceAt(t);
    n.fix = fix;

    const CalibrationKeys& ck = segment_->keys;
    CalibrationSet cal = calibration_ ? *calibration_ : configuredCalibration(config_);
    cal.mounting = toE(est.at<csym::Rot3<double>>(ck.mounting));
    cal.lever_arm = toE(est.at<V3c>(ck.lever_arm));
    const auto bs = est.at<csym::Vector2<double>>(ck.boresight);
    cal.boresight = Eigen::Vector2d(bs[0], bs[1]);
    const Vector9d mag = toE9(est.at<csym::Vector<double, 9>>(ck.magnetometer));
    cal.mag_hard_iron = mag.head<3>();
    cal.mag_soft_iron = mag.tail<6>();
    const auto baro = est.at<csym::Vector2<double>>(ck.barometer);
    cal.baro_offset = baro[0];
    cal.baro_airflow = baro[1];
    if (cov)
    {
        n.cov_Rpv = cov->topLeftCorner(9, 9);
        cal.cov = cov->block<kCalibrationDim, kCalibrationDim>(9, 9);
        n.mounting_cov = cal.mountingCov();
    }
    calibration_ = cal;
    n.R_b_i = cal.mounting;
    status_.mounting_rpy = mountingRpy(cal.mounting);
    status_.mounting_sigma = mountingSigmaBody(cal.mounting, cal.mountingCov());
    status_.lever_arm = cal.lever_arm;
    status_.lever_arm_sigma = cal.leverArmCov().diagonal().cwiseMax(0.0).cwiseSqrt();
    status_.boresight = cal.boresight;
    status_.boresight_sigma = cal.boresightCov().diagonal().cwiseMax(0.0).cwiseSqrt();
    status_.gravity_deflection = config_.gravity && config_.gravity->refinesNormalAt(toC(n.nav.p_e));
    status_.mag_hard_iron = cal.mag_hard_iron;
    status_.mag_hard_iron_sigma = cal.magnetometerCov().diagonal().head<3>().cwiseMax(0.0).cwiseSqrt();
    status_.mag_soft_iron = cal.mag_soft_iron;
    status_.baro_offset = cal.baro_offset;
    status_.baro_offset_sigma = std::sqrt(std::max(0.0, cal.barometerCov()(0, 0)));
    status_.baro_airflow = cal.baro_airflow;
    status_.baro_airflow_sigma = std::sqrt(std::max(0.0, cal.barometerCov()(1, 1)));
    status_.mag_trusted = mag_trusted_;
    status_.gyro_bias = n.bg;
    status_.accel_bias = n.ba;
    status_.window_variables = est.size();
    status_.window_factors = fls_.factors().size();
    newest_ = n;
}

// ---- output ------------------------------------------------------------------------------

VehicleState Estimator::stateFrom(double t, const Eigen::Quaterniond& q_e_i, const Eigen::Vector3d& p_e,
                                  const Eigen::Vector3d& v_e, const Eigen::Vector3d& bg, const Eigen::Vector3d& ba,
                                  const Eigen::Vector3d& omega_raw, const Eigen::Vector3d& f_raw,
                                  const Eigen::Quaterniond& q_b_i, const std::optional<Eigen::MatrixXd>& cov,
                                  const std::optional<Eigen::Matrix3d>& mounting_cov) const
{
    VehicleState s;
    s.gps_time = t;
    const Eigen::Matrix3d R_e_i = q_e_i.toRotationMatrix();
    const Eigen::Matrix3d R_b_i = q_b_i.normalized().toRotationMatrix();
    const Eigen::Vector3d& r = config_.reference_point;

    // Angular rate relative to the earth, in the IMU frame.
    const Eigen::Vector3d omega = omega_raw - bg - R_e_i.transpose() * kOmegaIe;
    const Eigen::Vector3d p_ref = p_e + R_e_i * r;
    const Eigen::Vector3d v_ref = v_e + R_e_i * omega.cross(r);
    const auto llh = geodesy::ecefToLlh(toC(p_ref));
    s.lat = llh.lat;
    s.lon = llh.lon;
    s.h = llh.h;
    s.p_e = p_ref;
    const Eigen::Matrix3d R_n_e = rotEcefFromNed(llh.lat, llh.lon).transpose();
    const Eigen::Matrix3d R_n_b = R_n_e * R_e_i * R_b_i.transpose();
    s.q_n_b = Eigen::Quaterniond(R_n_b).normalized();
    const Eigen::Vector3d rpy = rollPitchYaw(R_n_b);
    s.roll = rpy.x();
    s.pitch = rpy.y();
    s.yaw = rpy.z();
    s.v_ned = R_n_e * v_ref;
    s.v_body = R_b_i * R_e_i.transpose() * v_ref;
    s.rate_body = R_b_i * omega;

    // Kinematic acceleration at the IMU relative to the earth -- specific
    // force plus gravity less Coriolis -- carried to the reference point by
    // the centripetal term. The angular-acceleration term is left out: it
    // needs a derivative of the gyro, which is noise.
    const Eigen::Vector3d g_e = gravityAt(p_e);
    const Eigen::Vector3d a_e = R_e_i * (f_raw - ba) + g_e - 2.0 * kOmegaIe.cross(v_e);
    s.accel_body = R_b_i * (R_e_i.transpose() * a_e + omega.cross(omega.cross(r)));

    const double speed = std::hypot(s.v_body.x(), s.v_body.y());
    s.sideslip_valid = speed >= config_.sideslip_min_speed;
    s.sideslip = s.sideslip_valid ? std::atan2(s.v_body.y(), s.v_body.x()) : 0.0;

    if (cov)
    {
        const Eigen::Matrix3d R_n_i = R_n_e * R_e_i;
        const Eigen::Matrix3d att_nav = R_n_i * cov->block<3, 3>(0, 0) * R_n_i.transpose();
        // The body's attitude is the IMU's through the mounting, so the
        // mounting's uncertainty is the body's too -- and its yaw is sideslip.
        // An IMU-frame tangent on R_b_i reaches NED through R_n_i.
        Eigen::Matrix3d att = att_nav;
        if (mounting_cov) att += R_n_i * *mounting_cov * R_n_i.transpose();
        // Small-tilt mapping: rotation about north, east, down reads as
        // roll, pitch, yaw error.
        s.sigma_attitude = att.diagonal().cwiseMax(0.0).cwiseSqrt();
        s.sigma_position_ned = (R_n_e * cov->block<3, 3>(3, 3) * R_n_e.transpose()).diagonal().cwiseMax(0.0).cwiseSqrt();
        const Eigen::Matrix3d cov_vn = R_n_e * cov->block<3, 3>(6, 6) * R_n_e.transpose();
        s.sigma_velocity_ned = cov_vn.diagonal().cwiseMax(0.0).cwiseSqrt();
        if (s.sideslip_valid)
        {
            // Lateral velocity error over speed, and heading error, in quadrature.
            const Eigen::Vector3d lateral_n = R_n_b.col(1);
            const double sv = std::sqrt(std::max(0.0, lateral_n.dot(cov_vn * lateral_n)));
            s.sigma_sideslip = std::hypot(sv / speed, s.sigma_attitude.z());
        }
        // Valid is about the navigation solution. A mounting still being
        // learned widens the reported sigmas; it does not blank the output.
        s.valid = att_nav.diagonal().cwiseMax(0.0).cwiseSqrt().maxCoeff() < config_.valid_attitude_sigma &&
                  s.sigma_position_ned.maxCoeff() < config_.valid_position_sigma;
    }
    return s;
}

std::optional<VehicleState> Estimator::keyframeState() const
{
    if (!newest_) return std::nullopt;
    VehicleState s = stateFrom(newest_->t, newest_->nav.R_e_b, newest_->nav.p_e, newest_->nav.v_e, newest_->bg,
                               newest_->ba, newest_->omega_i, newest_->f_i, newest_->R_b_i, newest_->cov_Rpv,
                               newest_->mounting_cov);
    s.fix = newest_->fix;
    decorate(s);
    return s;
}

void Estimator::decorate(VehicleState& s) const
{
    // No covariance, no claim: its sigmas then read zero, which would pass.
    const bool level = newest_ && newest_->cov_Rpv &&
                       std::max(s.sigma_attitude.x(), s.sigma_attitude.y()) < config_.valid_attitude_sigma;
    s.gps_time_valid = !provisional_time_;
    if (anchored_)
    {
        // Attitude only. Position and velocity are the anchor's placeholders.
        s.valid = false;
        s.sideslip_valid = false;
        s.attitude_valid = level;
        s.heading_magnetic = heading_known_;
        s.heading_source = heading_known_ ? HeadingSource::magnetometer : HeadingSource::none;
        return;
    }
    s.attitude_valid = level;
    s.heading_magnetic = false;
    if (last_dual_antenna_ && s.gps_time - *last_dual_antenna_ < 1.0)
        s.heading_source = HeadingSource::dual_antenna;
    else if (last_mag_used_ && s.gps_time - *last_mag_used_ < 1.0)
        s.heading_source = HeadingSource::magnetometer;
    else
        s.heading_source = HeadingSource::inertial;
}

std::optional<VehicleState> Estimator::latest() const
{
    if (!newest_) return std::nullopt;
    if (imu_.empty()) return keyframeState();
    imu_preint::Preintegrator pim(config_.imu_noise, config_.dv_frame, newest_->bg, newest_->ba);
    for (const auto& b : imu_) pim.integrate(b.inc);
    const imu_preint::NavState nav =
        imu_preint::predict(newest_->nav, newest_->bg, newest_->ba, pim.result(), gravityAt(newest_->nav.p_e), kOmegaIe);
    const auto& last = imu_.back();
    VehicleState s = stateFrom(last.t1, nav.R_e_b, nav.p_e, nav.v_e, newest_->bg, newest_->ba,
                               imu_preint::rotationVector(last.inc.dq) / last.inc.dt, last.inc.dv / last.inc.dt,
                               newest_->R_b_i, newest_->cov_Rpv, newest_->mounting_cov);
    s.fix = newest_->fix;
    decorate(s);
    return s;
}

}  // namespace vehicle_estimator
