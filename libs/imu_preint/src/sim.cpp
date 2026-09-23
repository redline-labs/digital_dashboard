#include "imu_preint/sim.h"

#include "geodesy/geodetic.h"
#include "geodesy/wgs84.h"

#include <array>
#include <cmath>

namespace imu_preint
{

namespace
{

Eigen::Vector3d toEigen(const csym::Vector3<double>& v)
{
    return {v[0], v[1], v[2]};
}

csym::Vector3<double> toCsym(const Eigen::Vector3d& v)
{
    return csym::Vector3<double>{v.x(), v.y(), v.z()};
}

// The earth's rotation since t = 0: carries ECEF axes into inertial ones.
Eigen::Matrix3d rotInertialFromEcef(double t)
{
    return Eigen::AngleAxisd(geodesy::wgs84::kOmegaIe * t, Eigen::Vector3d::UnitZ()).toRotationMatrix();
}

}  // namespace

Eigen::Matrix3d yawPitchRoll(double yaw, double pitch, double roll)
{
    return (Eigen::AngleAxisd(yaw, Eigen::Vector3d::UnitZ()) * Eigen::AngleAxisd(pitch, Eigen::Vector3d::UnitY()) *
            Eigen::AngleAxisd(roll, Eigen::Vector3d::UnitX()))
        .toRotationMatrix();
}

LocalTrajectory::LocalTrajectory(double lat_rad, double lon_rad, double h_m)
{
    const auto r = geodesy::rotEcefFromNed(lat_rad, lon_rad);
    for (Eigen::Index i = 0; i < 3; ++i)
        for (Eigen::Index j = 0; j < 3; ++j) r_e_n_(i, j) = r(static_cast<std::size_t>(i), static_cast<std::size_t>(j));
    origin_e_ = toEigen(geodesy::llhToEcef(geodesy::Llh<double>{lat_rad, lon_rad, h_m}));
}

TruthSample LocalTrajectory::at(double t) const
{
    const Local l = local(t);
    TruthSample s;
    s.R_e_b = r_e_n_ * l.R_n_b;
    s.p_e = origin_e_ + r_e_n_ * l.p;
    s.v_e = r_e_n_ * l.v;
    s.a_e = r_e_n_ * l.a;
    return s;
}

std::vector<Increment> simulateIncrements(const Trajectory& traj, double t0, double rate_hz, std::size_t count,
                                          DvFrame frame, const geodesy::GravityModel& gravity)
{
    // Five-point Gauss-Legendre on [-1, 1], applied to eight sub-intervals of
    // each sample: exact for polynomials far beyond any trajectory here.
    static constexpr std::array<double, 5> kNodes{0.0, -0.5384693101056831, 0.5384693101056831,
                                                  -0.9061798459386640, 0.9061798459386640};
    static constexpr std::array<double, 5> kWeights{0.5688888888888889, 0.4786286704993665, 0.4786286704993665,
                                                    0.2369268850561891, 0.2369268850561891};
    constexpr int kPieces = 8;
    const Eigen::Vector3d omega(0.0, 0.0, geodesy::wgs84::kOmegaIe);

    // Specific force in inertial axes: R_ie(t) (a_e + 2 omega x v_e - g_e).
    const auto specificForceInertial = [&](double t) {
        const TruthSample s = traj.at(t);
        const Eigen::Vector3d g = toEigen(gravity.gravityEcef(toCsym(s.p_e)));
        return Eigen::Vector3d(rotInertialFromEcef(t) * (s.a_e + 2.0 * omega.cross(s.v_e) - g));
    };
    const auto rotInertialFromBody = [&](double t) { return Eigen::Matrix3d(rotInertialFromEcef(t) * traj.at(t).R_e_b); };

    std::vector<Increment> out;
    out.reserve(count);
    const double dt = 1.0 / rate_hz;
    for (std::size_t k = 0; k < count; ++k)
    {
        const double ta = t0 + static_cast<double>(k) * dt;
        const double tb = ta + dt;
        Eigen::Vector3d integral = Eigen::Vector3d::Zero();
        const double h = dt / kPieces;
        for (int piece = 0; piece < kPieces; ++piece)
        {
            const double mid = ta + (piece + 0.5) * h;
            for (std::size_t q = 0; q < kNodes.size(); ++q)
                integral += kWeights[q] * 0.5 * h * specificForceInertial(mid + 0.5 * h * kNodes[q]);
        }
        const Eigen::Matrix3d ra = rotInertialFromBody(ta), rb = rotInertialFromBody(tb);
        Increment inc;
        inc.dt = dt;
        inc.dq = Eigen::Quaterniond(ra.transpose() * rb).normalized();
        inc.dv = (frame == DvFrame::start ? ra : rb).transpose() * integral;
        out.push_back(inc);
    }
    return out;
}

}  // namespace imu_preint
