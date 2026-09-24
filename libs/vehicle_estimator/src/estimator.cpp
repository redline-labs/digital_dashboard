#include "vehicle_estimator/estimator.h"

#include "geodesy/geodetic.h"
#include "geodesy/gravity.h"
#include "geodesy/wgs84.h"

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

V3c toC(const Eigen::Vector3d& v)
{
    return V3c{v.x(), v.y(), v.z()};
}

Eigen::Vector3d toE(const V3c& v)
{
    return {v[0], v[1], v[2]};
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

void Estimator::reset()
{
    if (status_.initialized && calibration_) carried_ = calibration_;
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
    ++status_.resets;
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
    if (!mapped || !(span > 0.0))
    {
        ++status_.imu_discarded;
        return;
    }
    const double t_end = *mapped + config_.imu_time_offset;

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
    if (newest_ && e.gps_time <= newest_->t)
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
    while (!gnss_.empty())
    {
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
        const bool ok = newest_ ? addKeyframe(epoch) : initialize(epoch);
        if (ok) ++made;
        if (epoch.velocity) last_velocity_ = LastVelocity{epoch.gps_time, epoch.velocity->v_ned};
    }
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
    return toE(geodesy::normalGravityEcef(toC(p_e)));
}

// ---- initialisation ----------------------------------------------------------

bool Estimator::initialize(const GnssEpoch& e)
{
    // A heading is what the IMU cannot give itself; course over ground is not
    // one while the car is sliding. So the first keyframe waits for the
    // dual-antenna solution.
    // On any failure the IMU history is trimmed but not emptied: the next
    // epoch needs the half second before it to level.
    const auto give_up = [&] {
        dropIncrementsBefore(e.gps_time - 1.0);
        return false;
    };
    if (!e.position || !e.attitude) return give_up();

    const double g = gravityAt(toE(geodesy::llhToEcef(
                                   geodesy::Llh<double>{e.position->lat, e.position->lon, e.position->h})))
                         .norm();

    // What the accelerometer should read, in NED, and what it did read: the
    // specific force a - g. Parked, a = 0 and this is levelling. Moving, a
    // comes from the change in GNSS velocity over the last epoch, and the
    // accelerometer is averaged over the same interval -- levelling a
    // drifting car on -g alone puts its lateral acceleration into roll.
    const auto meanForce = [&](double t0, double t1, Eigen::Vector3d& f, Eigen::Vector3d& rate) {
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
    };
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
    const Eigen::Quaterniond R_e_i(R_e_n * R_n_i);

    const Eigen::Vector3d antenna_e =
        toE(geodesy::llhToEcef(geodesy::Llh<double>{e.position->lat, e.position->lon, e.position->h}));
    const Eigen::Vector3d omega_i = rateAt(e.gps_time);
    const CalibrationSet cal = carried_ ? *carried_ : seeded_ ? *seeded_ : configuredCalibration(config_);

    const Eigen::Vector3d p_e = antenna_e - R_e_i * cal.lever_arm;
    Eigen::Vector3d v_e = Eigen::Vector3d::Zero();
    if (e.velocity) v_e = R_e_n * e.velocity->v_ned - R_e_i * omega_i.cross(cal.lever_arm);

    dropIncrementsBefore(e.gps_time);

    const std::uint64_t index = next_index_++;
    const auto k = keysFor(index);
    factor_graph::Values values;
    values.insert(k.R, toC(R_e_i));
    values.insert(k.p, toC(p_e));
    values.insert(k.v, toC(v_e));
    values.insert(k.bg, toC(Eigen::Vector3d::Zero()));
    values.insert(k.ba, toC(Eigen::Vector3d::Zero()));
    const Segment seg{next_segment_++, e.gps_time, calibrationKeys(next_segment_ - 1)};
    values.insert(seg.keys.mounting, toC(cal.mounting));
    values.insert(seg.keys.lever_arm, toC(cal.lever_arm));
    values.insert(seg.keys.boresight, csym::Vector2<double>{cal.boresight.x(), cal.boresight.y()});

    // Priors deliberately loose wherever this epoch's own measurement
    // factors (added below) carry the information -- a tight prior built from
    // the same fix would count it twice. The exception is rotation about the
    // baseline: the antennas cannot see it, so the prior is all there is.
    const double loose = 0.2;  // rad
    const Eigen::Matrix3d cov_R =
        loose * loose * Eigen::Matrix3d::Identity() + (about_baseline * about_baseline - loose * loose) * b_i * b_i.transpose();
    factor_graph::FactorList f;
    f.push_back(factors::priorRot(k.R, R_e_i, cov_R));
    f.push_back(factors::priorV3(k.p, p_e, 100.0 * Eigen::Matrix3d::Identity(), "position prior"));
    const double sv = e.velocity ? 1.0 : config_.init_velocity_sigma;
    f.push_back(factors::priorV3(k.v, v_e, sv * sv * Eigen::Matrix3d::Identity(), "velocity prior"));
    f.push_back(factors::priorV3(k.bg, Eigen::Vector3d::Zero(),
                                 std::pow(config_.gyro_bias_prior, 2) * Eigen::Matrix3d::Identity(), "gyro bias prior"));
    f.push_back(factors::priorV3(k.ba, Eigen::Vector3d::Zero(),
                                 std::pow(config_.accel_bias_prior, 2) * Eigen::Matrix3d::Identity(), "accel bias prior"));
    f.push_back(factors::calibrationPrior(seg.keys, cal));

    imu_preint::NavState predicted{R_e_i, p_e, v_e};
    Newest provisional;
    provisional.nav = predicted;
    provisional.omega_i = omega_i;
    newest_ = provisional;  // for measurementFactors' lever-arm terms and gating
    segment_ = seg;
    const auto meas = measurementFactors(e, k, predicted, 0.0, cal);
    newest_.reset();
    f.insert(f.end(), meas.begin(), meas.end());

    std::map<Key, double> stamps;
    for (Key key : {k.R, k.p, k.v, k.bg, k.ba, seg.keys.mounting, seg.keys.lever_arm, seg.keys.boresight})
        stamps[key] = e.gps_time;

    const auto t0 = std::chrono::steady_clock::now();
    const auto report = fls_.update(f, values, stamps);
    status_.last_solve_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!report.ok)
    {
        ++status_.updates_refused;
        segment_.reset();
        return false;
    }
    status_.last_optimize = report.optimize;
    ++status_.calibration_segments;
    if (sink_)
        sink_(KeyframeRecord{e.gps_time, k, f, values, stamps, omega_i, forceAt(e.gps_time), e.fix, true, seg.index,
                             seg.start});
    status_.initialized = true;
    ++status_.keyframes;
    refreshNewest(e, index, k);
    return true;
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
            out.push_back(factors::dualAntenna(k.R, ck.boresight, R_n_e, baseline_, e.attitude->yaw,
                                               e.attitude->pitch, cov, config_.robust_delta));
        else
            ++status_.gated_attitude;
    }
    return out;
}

// ---- keyframes ------------------------------------------------------------------------

bool Estimator::addKeyframe(const GnssEpoch& e)
{
    const Newest last = *newest_;
    imu_preint::Preintegrator pim(config_.imu_noise, config_.dv_frame, last.bg, last.ba);
    if (!preintegrateTo(e.gps_time, pim))
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
    for (Key key : {k.R, k.p, k.v, k.bg, k.ba}) stamps[key] = e.gps_time;

    factor_graph::FactorList f;
    f.push_back(imu_preint::makeImuFactor(last.keys, k, pim.result(), g, kOmegaIe));
    f.push_back(imu_preint::makeBiasWalkFactor(last.keys.bg, k.bg, config_.gyro_bias_walk, dt, "gyro bias walk"));
    f.push_back(imu_preint::makeBiasWalkFactor(last.keys.ba, k.ba, config_.accel_bias_walk, dt, "accel bias walk"));
    advanceSegment(e.gps_time, f, values, stamps);
    mountingFactors(e.gps_time, k, predicted, f);
    const std::size_t n_imu = f.size();
    const auto meas = measurementFactors(e, k, predicted, dt, *calibration_);
    f.insert(f.end(), meas.begin(), meas.end());

    const auto t0 = std::chrono::steady_clock::now();
    auto report = fls_.update(f, values, stamps);
    if (!report.ok)
    {
        // A measurement the checks let through but the factor refused: keep
        // the IMU chain unbroken without it.
        ++status_.updates_refused;
        f.resize(n_imu);
        report = fls_.update(f, values, stamps);
        if (!report.ok)
        {
            reset();
            return false;
        }
    }
    status_.last_solve_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    status_.last_optimize = report.optimize;
    if (sink_)
        sink_(KeyframeRecord{e.gps_time, k, f, values, stamps, rateAt(e.gps_time), forceAt(e.gps_time), e.fix, false,
                             segment_->index, segment_->start});
    ++status_.keyframes;
    refreshNewest(e, index, k);
    return true;
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
    f.push_back(factors::calibrationWalk(segment_->keys, next.keys, t - segment_->start, config_.mounting_walk,
                                         config_.lever_arm_walk, config_.boresight_walk));
    for (Key key : {next.keys.mounting, next.keys.lever_arm, next.keys.boresight}) stamps[key] = t;
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

void Estimator::refreshNewest(const GnssEpoch& e, std::uint64_t index, const imu_preint::KeyframeKeys& k)
{
    const auto& est = fls_.estimate();
    Newest n;
    n.t = e.gps_time;
    n.index = index;
    n.keys = k;
    n.nav.R_e_b = toE(est.at<csym::Rot3<double>>(k.R));
    n.nav.p_e = toE(est.at<V3c>(k.p));
    n.nav.v_e = toE(est.at<V3c>(k.v));
    n.bg = toE(est.at<V3c>(k.bg));
    n.ba = toE(est.at<V3c>(k.ba));
    n.omega_i = rateAt(e.gps_time);
    n.f_i = forceAt(e.gps_time);
    n.fix = e.fix;

    const CalibrationKeys& ck = segment_->keys;
    CalibrationSet cal = calibration_ ? *calibration_ : configuredCalibration(config_);
    cal.mounting = toE(est.at<csym::Rot3<double>>(ck.mounting));
    cal.lever_arm = toE(est.at<V3c>(ck.lever_arm));
    const auto bs = est.at<csym::Vector2<double>>(ck.boresight);
    cal.boresight = Eigen::Vector2d(bs[0], bs[1]);
    const std::array<Key, 6> keys{k.R, k.p, k.v, ck.mounting, ck.lever_arm, ck.boresight};
    if (const auto cov = fls_.jointCovariance(keys))
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
    return s;
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
    return s;
}

}  // namespace vehicle_estimator
