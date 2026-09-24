#include "vehicle_estimator/sim/scenario.h"

#include "geodesy/geodetic.h"
#include "geodesy/gravity.h"
#include "imu_preint/preintegrator.h"
#include "vehicle_estimator/atmosphere.h"
#include "vehicle_estimator/magnetic_reference.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <set>

namespace vehicle_estimator::sim
{

namespace
{

constexpr double kPi = std::numbers::pi;

// Quintic smoothstep: C2, so the acceleration it implies is continuous.
double smooth(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * x * (10.0 - 15.0 * x + 6.0 * x * x);
}
double smoothDot(double x)  // d/dx
{
    if (x <= 0.0 || x >= 1.0) return 0.0;
    return 30.0 * x * x * (1.0 - x) * (1.0 - x);
}
double smoothDDot(double x)  // d2/dx2
{
    if (x <= 0.0 || x >= 1.0) return 0.0;
    return 60.0 * x * (1.0 - x) * (1.0 - 2.0 * x);
}
double smoothIntegral(double x)  // integral from 0 to x
{
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 0.5 + (x - 1.0);
    return 2.5 * x * x * x * x - 3.0 * x * x * x * x * x + x * x * x * x * x * x;
}

Eigen::Matrix3d yawPitchRoll(double yaw, double pitch, double roll)
{
    return imu_preint::yawPitchRoll(yaw, pitch, roll);
}

Eigen::Vector3d vee(const Eigen::Matrix3d& s)
{
    return {0.5 * (s(2, 1) - s(1, 2)), 0.5 * (s(0, 2) - s(2, 0)), 0.5 * (s(1, 0) - s(0, 1))};
}

Eigen::Matrix3d rotEcefFromNed(double lat, double lon)
{
    const auto r = geodesy::rotEcefFromNed(lat, lon);
    Eigen::Matrix3d m;
    for (Eigen::Index i = 0; i < 3; ++i)
        for (Eigen::Index j = 0; j < 3; ++j) m(i, j) = r(static_cast<std::size_t>(i), static_cast<std::size_t>(j));
    return m;
}

csym::Vector3<double> toC(const Eigen::Vector3d& v)
{
    return csym::Vector3<double>{v.x(), v.y(), v.z()};
}

// ---- skidpad ------------------------------------------------------------------

class Skidpad final : public VehicleMotion
{
  public:
    explicit Skidpad(const SkidpadParams& p) : p_(p) {}

    double duration() const override { return p_.park + p_.drive; }

    Sample at(double t) const override
    {
        const double x = (t - p_.park) / p_.launch;
        const double V = p_.speed * smooth(x);
        const double Vdot = p_.speed * smoothDot(x) / p_.launch;
        const double dist = p_.speed * p_.launch * smoothIntegral(x);
        const double th = 0.3 + dist / p_.radius;
        const Eigen::Vector3d radial(std::cos(th), std::sin(th), 0.0), tangent(-std::sin(th), std::cos(th), 0.0);

        Sample s;
        s.p = p_.radius * radial;
        s.v = V * tangent;
        s.a = Vdot * tangent - V * V / p_.radius * radial;

        // Slip: eased in after the launch, then swung between low and high.
        const double t_drift = p_.park + p_.launch;
        const double ease = smooth((t - t_drift) / 2.0);
        const double swing = 0.5 - 0.5 * std::cos(2.0 * kPi * (t - t_drift) / p_.beta_period);
        double slip = ease * (p_.beta_low + (p_.beta_high - p_.beta_low) * swing);
        if (p_.spin) slip += 1.5 * smooth((t - (duration() - 6.0)) / 4.0);

        const double course = th + kPi / 2.0;
        const double lateral_g = V * V / p_.radius / 9.81;
        const double roll = 0.03 * lateral_g + 0.004 * std::sin(2.1 * t);
        const double pitch = -0.01 * Vdot / 3.0 + 0.003 * std::sin(1.3 * t);
        s.R_n_b = yawPitchRoll(course + slip, pitch, roll);
        return s;
    }

  private:
    SkidpadParams p_;
};

// ---- figure eight ------------------------------------------------------------------

class FigureEight final : public VehicleMotion
{
  public:
    explicit FigureEight(const FigureEightParams& p) : p_(p) {}

    double duration() const override { return p_.park + p_.drive; }

    Sample at(double t) const override
    {
        const double w = 2.0 * kPi / p_.period;
        const double x = (t - p_.park) / p_.ramp;
        const double phi = w * p_.ramp * smoothIntegral(x);
        const double phid = w * smooth(x);
        const double phidd = w * smoothDot(x) / p_.ramp;
        const double A = p_.size;

        const Eigen::Vector3d dp(A * std::cos(phi), A * std::cos(2.0 * phi), 0.0);  // d/dphi
        const Eigen::Vector3d ddp(-A * std::sin(phi), -2.0 * A * std::sin(2.0 * phi), 0.0);
        Sample s;
        s.p = Eigen::Vector3d(A * std::sin(phi), 0.5 * A * std::sin(2.0 * phi), 0.0);
        s.v = dp * phid;
        s.a = ddp * phid * phid + dp * phidd;

        const double kappa = (dp.x() * ddp.y() - dp.y() * ddp.x()) / std::pow(dp.norm(), 3);
        const double slip = p_.beta_max * std::clamp(kappa * A, -1.0, 1.0) * smooth(x);
        const double course = std::atan2(dp.y(), dp.x());
        const double roll = 0.02 * std::clamp(kappa * A, -1.0, 1.0) * smooth(x) + 0.003 * std::sin(1.7 * t);
        s.R_n_b = yawPitchRoll(course + slip, 0.004 * std::sin(0.9 * t), roll);
        return s;
    }

  private:
    FigureEightParams p_;
};

class Parked final : public VehicleMotion
{
  public:
    explicit Parked(double d) : d_(d) {}
    double duration() const override { return d_; }
    Sample at(double) const override
    {
        Sample s;
        s.R_n_b = yawPitchRoll(0.7, 0.01, -0.02);
        return s;
    }

  private:
    double d_;
};

// ---- scripted phases ------------------------------------------------------------

class Scripted final : public VehicleMotion
{
  public:
    Scripted(std::vector<Phase> phases, double heading) : phases_(std::move(phases))
    {
        double t = 0.0, v = 0.0, psi = heading, pitch = 0.0, roll = 0.0, bias = 0.0, h = 0.0;
        for (const auto& p : phases_)
        {
            starts_.push_back({t, v, psi, pitch, roll, bias, h});
            h += p.climb;
            t += p.duration;
            v = p.speed;
            psi += p.turn;
            pitch = p.pitch;
            roll = p.roll;
            bias = p.slip_bias;
        }
        duration_ = t;
        // Position is the one thing without a closed form: integrate the
        // (closed-form) velocity once onto a grid, Gauss-Legendre per step.
        const auto n = static_cast<std::size_t>(std::ceil(duration_ / kGrid)) + 2;
        grid_.resize(n, Eigen::Vector3d::Zero());
        for (std::size_t k = 1; k < n; ++k)
            grid_[k] = grid_[k - 1] + integrate(static_cast<double>(k - 1) * kGrid, static_cast<double>(k) * kGrid);
    }

    double duration() const override { return duration_; }

    Sample at(double t) const override
    {
        const State st = state(t);
        const auto k = static_cast<std::size_t>(std::clamp(std::floor(t / kGrid), 0.0, static_cast<double>(grid_.size() - 1)));
        Sample s;
        s.p = grid_[k] + integrate(static_cast<double>(k) * kGrid, t);
        s.v = st.V * Eigen::Vector3d(std::cos(st.psi), std::sin(st.psi), 0.0);
        s.a = st.Vdot * Eigen::Vector3d(std::cos(st.psi), std::sin(st.psi), 0.0) +
              st.V * st.psidot * Eigen::Vector3d(-std::sin(st.psi), std::cos(st.psi), 0.0);
        // Height, NED down: closed form, since it is only the smoothstep.
        s.p.z() = -st.H;
        s.v.z() = -st.Hdot;
        s.a.z() = -st.Hddot;
        // Body roll into the corner, as the others do, on top of any grade;
        // on a climb the nose follows the road.
        const double lateral_g = st.V * st.psidot / 9.81;
        const double grade = st.V > 0.5 ? std::atan2(st.Hdot, st.V) : 0.0;
        s.R_n_b = yawPitchRoll(st.psi + st.slip, st.pitch + grade, st.roll + 0.03 * lateral_g);
        return s;
    }

  private:
    static constexpr double kGrid = 0.01;

    struct Start
    {
        double t, v, psi, pitch, roll, bias, h;
    };
    struct State
    {
        double V = 0.0, Vdot = 0.0, psi = 0.0, psidot = 0.0, slip = 0.0, pitch = 0.0, roll = 0.0;
        double H = 0.0, Hdot = 0.0, Hddot = 0.0;
    };

    State state(double t) const
    {
        std::size_t i = 0;
        while (i + 1 < phases_.size() && t >= starts_[i + 1].t) ++i;
        const Phase& p = phases_[i];
        const Start& a = starts_[i];
        const double x = (t - a.t) / p.duration;
        const double sm = smooth(x), sd = smoothDot(x) / p.duration;
        State s;
        s.V = a.v + (p.speed - a.v) * sm;
        s.Vdot = (p.speed - a.v) * sd;
        s.psi = a.psi + p.turn * sm;
        s.psidot = p.turn * sd;
        const double xc = std::clamp(x, 0.0, 1.0);
        const double bump = 16.0 * xc * xc * (1.0 - xc) * (1.0 - xc);
        s.slip = a.bias + (p.slip_bias - a.bias) * sm + p.slip * bump;
        s.pitch = a.pitch + (p.pitch - a.pitch) * sm;
        s.roll = a.roll + (p.roll - a.roll) * sm;
        s.H = a.h + p.climb * sm;
        s.Hdot = p.climb * sd;
        s.Hddot = p.climb * smoothDDot(x) / (p.duration * p.duration);
        return s;
    }

    Eigen::Vector3d velocity(double t) const
    {
        const State s = state(t);
        return s.V * Eigen::Vector3d(std::cos(s.psi), std::sin(s.psi), 0.0);
    }

    Eigen::Vector3d integrate(double t0, double t1) const
    {
        static constexpr std::array<double, 5> x{0.0, -0.5384693101056831, 0.5384693101056831, -0.9061798459386640,
                                                 0.9061798459386640};
        static constexpr std::array<double, 5> w{0.5688888888888889, 0.4786286704993665, 0.4786286704993665,
                                                 0.2369268850561891, 0.2369268850561891};
        const double h = 0.5 * (t1 - t0), m = 0.5 * (t0 + t1);
        Eigen::Vector3d sum = Eigen::Vector3d::Zero();
        for (std::size_t j = 0; j < x.size(); ++j) sum += w[j] * velocity(m + h * x[j]);
        return h * sum;
    }

    std::vector<Phase> phases_;
    std::vector<Start> starts_;
    std::vector<Eigen::Vector3d> grid_;
    double duration_ = 0.0;
};

// The IMU frame's motion, for imu_preint's simulator.
class ImuTrajectory final : public imu_preint::LocalTrajectory
{
  public:
    ImuTrajectory(const VehicleMotion& m, const SensorModel& sensors, double lat, double lon, double h)
        : LocalTrajectory(lat, lon, h), m_(m), sensors_(sensors)
    {
    }
    Local local(double t) const override
    {
        const auto s = m_.at(t);
        Local l;
        l.p = s.p;
        l.v = s.v;
        l.a = s.a;
        l.R_n_b = s.R_n_b * sensors_.mountingAt(t);  // "body" here is the IMU frame
        return l;
    }

  private:
    const VehicleMotion& m_;
    const SensorModel& sensors_;
};

}  // namespace

std::unique_ptr<VehicleMotion> skidpad(const SkidpadParams& p)
{
    return std::make_unique<Skidpad>(p);
}

std::unique_ptr<VehicleMotion> figureEight(const FigureEightParams& p)
{
    return std::make_unique<FigureEight>(p);
}

std::unique_ptr<VehicleMotion> parked(double duration)
{
    return std::make_unique<Parked>(duration);
}

std::unique_ptr<VehicleMotion> scripted(std::vector<Phase> phases, double heading)
{
    return std::make_unique<Scripted>(std::move(phases), heading);
}

std::unique_ptr<VehicleMotion> track(const TrackParams& p)
{
    std::vector<Phase> phases;
    phases.push_back({p.park, 0.0});
    phases.push_back({p.launch, p.speed, 0.0, 0.0, p.straight_slip});
    for (int lap = 0; lap < 2 * p.laps; ++lap)
    {
        phases.push_back({p.straight, p.speed, 0.0, 0.0, p.straight_slip});
        phases.push_back({p.corner, p.speed, kPi, p.corner_slip, p.straight_slip});
    }
    phases.push_back({p.straight, p.speed, 0.0, 0.0, p.straight_slip});
    return scripted(std::move(phases));
}

std::unique_ptr<VehicleMotion> stopAndGo(const StopAndGoParams& p)
{
    std::vector<Phase> phases;
    phases.push_back({p.park, 0.0});
    for (int i = 0; i < p.stops; ++i)
    {
        const double k = static_cast<double>(i + 1);
        const double grade = p.grade * std::sin(1.7 * k), camber = p.camber * std::cos(2.3 * k);
        phases.push_back({p.drive, p.speed, p.turn});
        phases.push_back({p.brake, 0.0});
        phases.push_back({1.5, 0.0, 0.0, 0.0, 0.0, grade, camber});  // settles onto the grade
        phases.push_back({p.hold, 0.0, 0.0, 0.0, 0.0, grade, camber});
        phases.push_back({1.5, 0.0});  // and off it
    }
    return scripted(std::move(phases));
}

Eigen::Matrix3d SensorModel::mountingAt(double t) const
{
    if (!mount_step) return R_b_i;
    const double f = smooth((t - mount_step->t) / mount_step->duration);
    return Eigen::AngleAxisd(mount_step->yaw * f, Eigen::Vector3d::UnitZ()).toRotationMatrix() * R_b_i;
}

Scenario::Scenario(std::unique_ptr<VehicleMotion> motion, SensorModel sensors, double lat, double lon, double h)
    : motion_(std::move(motion)), sensors_(std::move(sensors))
{
    imu_traj_ = std::make_unique<ImuTrajectory>(*motion_, sensors_, lat * kPi / 180.0, lon * kPi / 180.0, h);
}

Truth Scenario::truth(double t) const
{
    const auto s = motion_->at(t);
    const double h = 1e-5;
    const Eigen::Matrix3d dR = (motion_->at(t + h).R_n_b - motion_->at(t - h).R_n_b) / (2.0 * h);
    const Eigen::Vector3d w_b = vee(s.R_n_b.transpose() * dR);  // relative to the (earth-fixed) origin frame

    const Eigen::Vector3d r_b = sensors_.mountingAt(t) * sensors_.reference_point;
    const Eigen::Vector3d p_ref_n0 = s.p + s.R_n_b * r_b;
    const Eigen::Vector3d v_ref_n0 = s.v + s.R_n_b * w_b.cross(r_b);

    Truth out;
    out.p_e = imu_traj_->originEcef() + imu_traj_->rotEcefFromNed() * p_ref_n0;
    const auto llh = geodesy::ecefToLlh(toC(out.p_e));
    // Local NED at the car, not at the origin: they differ by the track's
    // extent over the earth's radius, ~1e-5 rad.
    const Eigen::Matrix3d R_n_n0 = rotEcefFromNed(llh.lat, llh.lon).transpose() * imu_traj_->rotEcefFromNed();
    out.R_n_b = R_n_n0 * s.R_n_b;
    out.v_ned = R_n_n0 * v_ref_n0;
    out.v_body = s.R_n_b.transpose() * v_ref_n0;
    out.rate_body = w_b;
    out.sideslip = std::atan2(out.v_body.y(), out.v_body.x());
    out.pitch = std::asin(std::clamp(-out.R_n_b(2, 0), -1.0, 1.0));
    out.roll = std::atan2(out.R_n_b(2, 1), out.R_n_b(2, 2));
    out.yaw = std::atan2(out.R_n_b(1, 0), out.R_n_b(0, 0));
    return out;
}

std::vector<Message> Scenario::messages() const
{
    std::mt19937 rng(sensors_.seed);
    std::normal_distribution<double> gauss(0.0, 1.0);
    const auto jitter = [&](double mean) { return std::exponential_distribution<double>(1.0 / mean)(rng); };
    const geodesy::NormalGravity normal_gravity;
    const geodesy::GravityModel& gravity = sensors_.gravity ? *sensors_.gravity : normal_gravity;
    const auto& S = sensors_;

    // The magnetometer and barometer draw from their own stream, so adding
    // them changed no existing scenario's noise.
    std::mt19937 aux(sensors_.seed * 7919u + 17u);
    std::normal_distribution<double> aux_gauss(0.0, 1.0);
    MagneticReference magnetic;
    Eigen::Vector3d gyro_bias = sensors_.gyro_bias;

    std::vector<Message> out;
    const auto n_imu = static_cast<std::size_t>(std::floor(duration() * S.imu_rate));
    const auto incs = imu_preint::simulateIncrements(*imu_traj_, 0.0, S.imu_rate, n_imu, S.dv_frame, gravity);
    const std::set<std::size_t> dropped(S.dropped_imu.begin(), S.dropped_imu.end());
    const auto ticks_per_sample = static_cast<std::uint32_t>(std::lround(10000.0 / S.imu_rate));
    double last_imu_host = 0.0, last_gnss_host = 0.0;
    for (std::size_t k = 0; k < incs.size(); ++k)
    {
        imu_preint::Increment inc = incs[k];
        const double sg = S.gyro_noise_density * std::sqrt(inc.dt);
        const double sa = S.accel_noise_density * std::sqrt(inc.dt);
        if (S.gyro_bias_walk > 0.0)
            gyro_bias += S.gyro_bias_walk * std::sqrt(inc.dt) *
                         Eigen::Vector3d(aux_gauss(aux), aux_gauss(aux), aux_gauss(aux));
        const Eigen::Vector3d theta = imu_preint::rotationVector(inc.dq) + gyro_bias * inc.dt +
                                      sg * Eigen::Vector3d(gauss(rng), gauss(rng), gauss(rng));
        inc.dq = imu_preint::fromRotationVector(theta);
        inc.dv += S.accel_bias * inc.dt + sa * Eigen::Vector3d(gauss(rng), gauss(rng), gauss(rng));
        const double lat_s = S.imu_latency + jitter(S.imu_jitter);
        if (dropped.contains(k)) continue;

        ImuSample m;
        // The sample ending at (k + 1) / rate carries counter first + k + 1.
        m.packet_counter = static_cast<std::uint16_t>(S.first_counter + static_cast<std::uint16_t>((k + 1) & 0xFFFFu));
        m.sample_time_fine = S.first_tick + static_cast<std::uint32_t>(k + 1) * ticks_per_sample;
        m.dq = inc.dq;
        m.dv = inc.dv;
        const double t_end = static_cast<double>(k + 1) / S.imu_rate;
        // One ordered stream: a sample never overtakes the one before it,
        // however its latency was drawn.
        m.host_time = std::max(S.host_epoch + t_end + lat_s, last_imu_host + 1e-6);
        last_imu_host = m.host_time;
        if (S.magnetometer || S.barometer)
        {
            const auto truth_i = imu_traj_->at(t_end);
            const auto llh = geodesy::ecefToLlh(toC(truth_i.p_e));
            const Eigen::Matrix3d R_e_n = rotEcefFromNed(llh.lat, llh.lon);
            if (S.magnetometer)
            {
                const auto& field = magnetic.at(llh.lat, llh.lon, llh.h, S.gps_epoch + t_end);
                Eigen::Vector3d ned_au = field.ned_nt / S.mag_normalisation_nt;
                for (const auto& d : S.mag_disturbances)
                    if (t_end >= d.t0 && t_end < d.t1) ned_au += d.ned_au;
                const Eigen::Vector3d in_imu = truth_i.R_e_b.transpose() * (R_e_n * ned_au);
                m.mag_au = S.mag_soft_iron * in_imu + S.mag_hard_iron +
                           S.mag_noise * Eigen::Vector3d(aux_gauss(aux), aux_gauss(aux), aux_gauss(aux));
            }
            if (S.barometer && (k + 1) % S.baro_every == 0)
            {
                const double pressure_altitude = llh.h - (S.baro_offset + S.baro_offset_rate * t_end);
                const double dynamic = 0.5 * isa::density(pressure_altitude) * truth_i.v_e.squaredNorm();
                const double pa =
                    isa::pressure(pressure_altitude) + S.baro_airflow * dynamic + S.baro_noise_pa * aux_gauss(aux);
                m.pressure_pa = std::round(pa);  // whole pascals, as the device sends them
            }
        }
        out.push_back(Message{m.host_time, m, std::nullopt});
    }

    const Eigen::Vector3d baseline_i = (S.antenna2_lever_arm - S.lever_arm).normalized();
    const auto n_gnss = static_cast<std::size_t>(std::floor(duration() * S.gnss_rate));
    for (std::size_t k = 1; k <= n_gnss; ++k)
    {
        const double t = static_cast<double>(k) / S.gnss_rate;
        if (t > duration() - 0.2) break;
        const double lat_s = S.gnss_latency + jitter(S.gnss_jitter);
        const bool out_now = std::any_of(S.outages.begin(), S.outages.end(),
                                         [&](const auto& o) { return t >= o.first && t < o.second; });
        // Draw the noise either way, so an outage does not change the noise
        // every later epoch sees.
        const Eigen::Vector3d np(gauss(rng), gauss(rng), gauss(rng)), nv(gauss(rng), gauss(rng), gauss(rng));
        const double ny = gauss(rng), npi = gauss(rng);
        if (out_now) continue;

        FixQuality fix = S.fix;
        double scale = 1.0;
        for (const auto& c : S.fix_changes)
            if (t >= c.t)
            {
                fix = c.fix;
                scale = c.scale;
            }

        const auto ts = imu_traj_->at(t);
        const auto vm = motion_->at(t);
        // The IMU's own rate, which includes the mount turning under it.
        const double h = 1e-5;
        const Eigen::Matrix3d R_n_i_p = motion_->at(t + h).R_n_b * S.mountingAt(t + h);
        const Eigen::Matrix3d R_n_i_m = motion_->at(t - h).R_n_b * S.mountingAt(t - h);
        const Eigen::Matrix3d R_n_i = vm.R_n_b * S.mountingAt(t);
        const Eigen::Vector3d w_i = vee(R_n_i.transpose() * (R_n_i_p - R_n_i_m) / (2.0 * h));

        const Eigen::Vector3d ant_e = ts.p_e + ts.R_e_b * S.lever_arm;
        const auto llh0 = geodesy::ecefToLlh(toC(ant_e));
        const Eigen::Matrix3d R_e_n = rotEcefFromNed(llh0.lat, llh0.lon);
        Eigen::Vector3d offset = scale * S.position_sigma_ned.cwiseProduct(np);
        for (const auto& [to, off] : S.outliers)
            if (std::fabs(to - t) < 1e-6) offset += off;
        const auto llh = geodesy::ecefToLlh(toC(Eigen::Vector3d(ant_e + R_e_n * offset)));

        GnssEpoch e;
        e.gps_time = S.gps_epoch + t;
        e.host_time = S.host_epoch + t + lat_s;
        if (S.gnss_in_order) e.host_time = std::max(e.host_time, last_gnss_host + 1e-6);
        last_gnss_host = e.host_time;
        e.fix = fix;
        GnssPosition pos;
        pos.lat = llh.lat;
        pos.lon = llh.lon;
        pos.h = llh.h;
        pos.cov_ned = (scale * S.position_sigma_ned).cwiseAbs2().asDiagonal();
        e.position = pos;

        const Eigen::Vector3d v_ant_e = ts.v_e + ts.R_e_b * w_i.cross(S.lever_arm);
        GnssVelocity vel;
        vel.v_ned = R_e_n.transpose() * v_ant_e + scale * S.velocity_sigma_ned.cwiseProduct(nv);
        e.velocity = vel;

        const Eigen::Vector3d b_n = R_e_n.transpose() * ts.R_e_b * baseline_i;
        DualAntenna att;
        att.yaw = std::atan2(b_n.y(), b_n.x()) + S.yaw_sigma * ny;
        att.pitch = std::atan2(-b_n.z(), std::hypot(b_n.x(), b_n.y())) + S.pitch_sigma * npi;
        for (const auto& b : S.attitude_biases)
            if (t >= b.t0 && t < b.t1)
            {
                att.yaw += b.offset.x();
                att.pitch += b.offset.y();
            }
        if (S.attitude_covariance)
            att.cov = Eigen::Vector2d(S.yaw_sigma * S.yaw_sigma, S.pitch_sigma * S.pitch_sigma).asDiagonal();
        const bool heading_out = std::any_of(S.attitude_outages.begin(), S.attitude_outages.end(),
                                             [&](const auto& o) { return t >= o.first && t < o.second; });
        if (t >= S.attitude_from && !heading_out) e.attitude = att;
        out.push_back(Message{e.host_time, std::nullopt, e});
    }

    std::stable_sort(out.begin(), out.end(), [](const Message& a, const Message& b) { return a.host_time < b.host_time; });
    return out;
}

}  // namespace vehicle_estimator::sim
