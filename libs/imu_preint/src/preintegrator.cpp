#include "imu_preint/preintegrator.h"

#include "factor_graph/values.h"

#include "csym/function.h"
#include "csym/geo/rot3.h"

#include <cmath>
#include <tuple>

namespace imu_preint
{

namespace
{

Eigen::Matrix3d skew(const Eigen::Vector3d& v)
{
    Eigen::Matrix3d m;
    m << 0.0, -v.z(), v.y(), v.z(), 0.0, -v.x(), -v.y(), v.x(), 0.0;
    return m;
}

using V3 = csym::Vector3<double>;
using Rot3 = csym::Rot3<double>;

// One sample of preintegration: the interval's state before it -- rotation,
// velocity, position, and the previous sample's specific force for the slope
// term -- to the state after. Everything the preintegrator needs besides the
// value is a Jacobian of this one function, which csym derives: the
// covariance's transition and noise maps, and how the whole state moves with
// the biases. They were once written out by hand, and the slope term's bias
// Jacobian (through the PREVIOUS sample's force) was the one left out.
//
//   theta = w_dt - bg dt + n_theta       u = dv - ba dt + n_u
//   R' = R Exp(theta)
//   du = R' u (dv in the end frame)  or  R u (in the start frame)
//   f' = u / dt  or  Exp(theta)^T u / dt   (this sample's force, end frame)
//   v' = v + du
//   p' = p + v dt + du dt / 2 - slope dt / 12,  slope = du - R f dt
//
// Half of du at mid-sample is exact for a force constant in this interval's
// frame; a force that turns (a skidpad's centripetal direction sweeping
// round) leaves a third-order error, 5e-6 m per 0.1 s at 1.3 g. A force
// changing linearly from the previous sample removes it: the double integral
// picks up -dt/12 of the change. `slope_on` is 0 for the very first sample,
// which has no previous force.
template <bool EndFrame>
constexpr auto kStep = [](auto R, auto v, auto p, auto f, auto bg, auto ba, auto n_theta, auto n_u, auto w_dt,
                          auto dv, auto dt, auto slope_on, auto eps) {
    using T = decltype(dt);
    const auto theta = w_dt - bg * dt + n_theta;
    const auto u = dv - ba * dt + n_u;
    const auto dr = csym::Rot3<T>::from_tangent(theta, eps);
    const csym::Rot3<T> R1 = R * dr;
    csym::Vector3<T> du, f1;
    if constexpr (EndFrame)
    {
        du = R1 * u;
        f1 = u / dt;
    }
    else
    {
        du = R * u;
        f1 = (dr.inverse() * u) / dt;
    }
    const csym::Vector3<T> slope = (du - R * (f * dt)) * slope_on;
    const csym::Vector3<T> v1 = v + du;
    const csym::Vector3<T> p1 = p + v * dt + du * (dt * T(0.5)) - slope * (dt / T(12));
    return std::make_tuple(R1, v1, p1, f1);
};

template <bool EndFrame>
using StepFn = csym::Function<kStep<EndFrame>, Rot3, V3, V3, V3, V3, V3, V3, V3, V3, V3, double, double, double>;

V3 toC(const Eigen::Vector3d& v)
{
    return V3{v.x(), v.y(), v.z()};
}

Eigen::Vector3d toE(const V3& v)
{
    return Eigen::Vector3d(v[0], v[1], v[2]);
}

}  // namespace

Eigen::Vector3d rotationVector(const Eigen::Quaterniond& q_in)
{
    // The shorter of the two rotations a quaternion and its negative describe.
    const Eigen::Quaterniond q = q_in.w() < 0.0 ? Eigen::Quaterniond(-q_in.coeffs()) : q_in;
    const Eigen::Vector3d v = q.vec();
    const double n = v.norm();
    if (n < 1e-12) return 2.0 * v / q.w();
    return 2.0 * std::atan2(n, q.w()) / n * v;
}

Eigen::Quaterniond fromRotationVector(const Eigen::Vector3d& v)
{
    const double a = v.norm();
    if (a < 1e-12) return Eigen::Quaterniond(1.0, 0.5 * v.x(), 0.5 * v.y(), 0.5 * v.z()).normalized();
    return Eigen::Quaterniond(Eigen::AngleAxisd(a, v / a));
}

std::pair<Increment, Increment> split(const Increment& inc, double fraction, DvFrame frame)
{
    // Constant body rate w and constant specific force f over the interval T.
    // To second order, dv in the start frame is T f + T^2/2 w x f, and in the
    // end frame T f - T^2/2 w x f. Solve for f, then rebuild each part.
    const double T = inc.dt;
    const Eigen::Vector3d w = rotationVector(inc.dq) / T;
    const double sign = frame == DvFrame::start ? 1.0 : -1.0;
    const auto integral = [&](double s) -> Eigen::Matrix3d {
        return s * Eigen::Matrix3d::Identity() + sign * 0.5 * s * s * skew(w);
    };
    const Eigen::Vector3d f = integral(T).inverse() * inc.dv;

    const double t1 = fraction * T, t2 = T - t1;
    Increment a, b;
    a.dt = t1;
    b.dt = t2;
    a.dq = fromRotationVector(w * t1);
    b.dq = fromRotationVector(w * t2);
    a.dv = integral(t1) * f;
    b.dv = integral(t2) * f;
    return {a, b};
}

const char* incrementProblem(const Increment& inc)
{
    if (!std::isfinite(inc.dt) || !(inc.dt > 0.0)) return "non-positive or non-finite dt";
    if (inc.dt > 1.0) return "dt longer than a second";
    if (!inc.dq.coeffs().allFinite()) return "non-finite rotation";
    if (!inc.dv.allFinite()) return "non-finite velocity increment";
    if (std::fabs(inc.dq.norm() - 1.0) > 1e-3) return "rotation quaternion is not unit length";
    return nullptr;
}

Preintegrator::Preintegrator(NoiseParams noise, DvFrame frame, const Eigen::Vector3d& bg_lin,
                             const Eigen::Vector3d& ba_lin)
    : noise_(noise), frame_(frame)
{
    reset(bg_lin, ba_lin);
}

void Preintegrator::reset(const Eigen::Vector3d& bg_lin, const Eigen::Vector3d& ba_lin)
{
    p_ = Preintegrated{};
    p_.bg_lin = bg_lin;
    p_.ba_lin = ba_lin;
    // The interval starts again; the previous sample's force, and how it
    // moves with the biases, carry over -- it is in the frame the next sample
    // starts in.
    R_ = Eigen::Quaterniond::Identity();
    J_.topRows<9>().setZero();
    // Only the force's own uncertainty carries over; its correlation with the
    // last interval's states belongs to that interval.
    const Eigen::Matrix3d f_var = cov_.bottomRightCorner<3, 3>();
    cov_.setZero();
    cov_.bottomRightCorner<3, 3>() = f_var;
}

std::string Preintegrator::integrate(const Increment& inc, double extra_rot_var, double extra_vel_var, bool bridged)
{
    if (const char* e = incrementProblem(inc)) return e;
    if (!(extra_rot_var >= 0.0) || !(extra_vel_var >= 0.0)) return "negative extra variance";

    const double var_theta = noise_.gyro_noise_density * noise_.gyro_noise_density * inc.dt + extra_rot_var;
    const double var_u = noise_.accel_noise_density * noise_.accel_noise_density * inc.dt + extra_vel_var;

    // Value and Jacobian with respect to the state (R, v, p, f), the biases
    // and the two noises, at zero noise: 12 tangent rows by 24 columns.
    const Eigen::Quaterniond q = R_;
    const auto args = std::make_tuple(Rot3(q.x(), q.y(), q.z(), q.w()), toC(p_.dv), toC(p_.dp), toC(f_prev_),
                                      toC(p_.bg_lin), toC(p_.ba_lin), V3{0.0, 0.0, 0.0}, V3{0.0, 0.0, 0.0},
                                      toC(rotationVector(inc.dq.normalized())), toC(inc.dv), inc.dt,
                                      have_prev_ ? 1.0 : 0.0, factor_graph::kEpsilon);
    const auto step = [&]<bool End>() {
        return std::apply([](const auto&... a) { return StepFn<End>::template jacobian<0, 1, 2, 3, 4, 5, 6, 7>(a...); },
                          args);
    };
    Eigen::Matrix<double, 12, 24> jac;
    Eigen::Quaterniond R1;
    Eigen::Vector3d v1, p1, f1;
    const auto take = [&](const auto& r) {
        jac = Eigen::Map<const Eigen::Matrix<double, 12, 24>>(r.jacobian.data.data());
        const auto& [Rn, vn, pn, fn] = r.value;
        R1 = Eigen::Quaterniond(Rn.w, Rn.x, Rn.y, Rn.z);
        v1 = toE(vn);
        p1 = toE(pn);
        f1 = toE(fn);
    };
    switch (frame_)
    {
        case DvFrame::start:
            take(step.template operator()<false>());
            break;
        case DvFrame::end:
            take(step.template operator()<true>());
            break;
        default:
            // Every enumerator is listed above, and -Wswitch-enum keeps it so,
            // so this is reached only by a frame_ holding a value that is not
            // one. It is also what lets the compiler see R1..f1 are set on
            // every path that uses them: without it, it cannot rule out
            // falling through the switch with all four uninitialized.
            return "unknown dv frame";
    }

    // Covariance over the same state the Jacobians use, the previous force
    // included: a sample's accelerometer noise reaches position through its
    // own step (5/12 dt) and again through the next sample's slope term
    // (1/12 dt), and dropping the second under-counts position noise.
    const Eigen::Matrix<double, 12, 12> A = jac.leftCols<12>();
    const Eigen::Matrix<double, 12, 6> B = jac.block<12, 6>(0, 18);
    Eigen::Matrix<double, 6, 1> var;
    var << var_theta, var_theta, var_theta, var_u, var_u, var_u;
    cov_ = A * cov_ * A.transpose() + B * var.asDiagonal() * B.transpose();
    p_.cov = cov_.topLeftCorner<9, 9>();

    // How the whole state -- the previous force included -- moves with the
    // biases, chained through the step.
    J_ = jac.leftCols<12>() * J_ + jac.block<12, 6>(0, 12);
    p_.dR_dbg = J_.block<3, 3>(0, 0);
    p_.dv_dbg = J_.block<3, 3>(3, 0);
    p_.dv_dba = J_.block<3, 3>(3, 3);
    p_.dp_dbg = J_.block<3, 3>(6, 0);
    p_.dp_dba = J_.block<3, 3>(6, 3);

    R_ = R1.normalized();
    p_.dR = R_.toRotationMatrix();
    p_.dv = v1;
    p_.dp = p1;
    f_prev_ = f1;
    have_prev_ = true;
    p_.dt += inc.dt;
    ++p_.samples;
    if (bridged) ++p_.bridged;
    return {};
}

}  // namespace imu_preint
