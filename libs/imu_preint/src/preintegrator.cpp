#include "imu_preint/preintegrator.h"

#include <cmath>

namespace imu_preint
{

Eigen::Matrix3d skew(const Eigen::Vector3d& v)
{
    Eigen::Matrix3d m;
    m << 0.0, -v.z(), v.y(), v.z(), 0.0, -v.x(), -v.y(), v.x(), 0.0;
    return m;
}

Eigen::Matrix3d rightJacobian(const Eigen::Vector3d& theta)
{
    const double a = theta.norm();
    const Eigen::Matrix3d k = skew(theta);
    if (a < 1e-5)
    {
        // Series to second order; the closed form divides by a^3.
        return Eigen::Matrix3d::Identity() - 0.5 * k + (1.0 / 6.0) * k * k;
    }
    return Eigen::Matrix3d::Identity() - (1.0 - std::cos(a)) / (a * a) * k + (a - std::sin(a)) / (a * a * a) * k * k;
}

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
}

std::string Preintegrator::integrate(const Increment& inc, double extra_rot_var, double extra_vel_var, bool bridged)
{
    if (const char* e = incrementProblem(inc)) return e;
    if (!(extra_rot_var >= 0.0) || !(extra_vel_var >= 0.0)) return "negative extra variance";

    const Eigen::Quaterniond dq = inc.dq.normalized();
    const Eigen::Vector3d theta = rotationVector(dq) - p_.bg_lin * inc.dt;
    const Eigen::Vector3d u = inc.dv - p_.ba_lin * inc.dt;
    const double var_theta = noise_.gyro_noise_density * noise_.gyro_noise_density * inc.dt + extra_rot_var;
    const double var_u = noise_.accel_noise_density * noise_.accel_noise_density * inc.dt + extra_vel_var;

    // The two conventions differ only in whether dv is rotated by the
    // orientation before or after this sample's rotation.
    const Eigen::Matrix3d R_start = p_.dR;
    const Eigen::Matrix3d R_start_dbg = p_.dR_dbg;
    // This sample's specific force in its end frame, and how it moves with
    // the biases, for the next sample's slope term.
    Eigen::Vector3d f_end = Eigen::Vector3d::Zero();
    Eigen::Matrix3d df_dbg = Eigen::Matrix3d::Zero(), df_dba = Eigen::Matrix3d::Zero();
    switch (frame_)
    {
        case DvFrame::start:
        {
            translate(p_.dR, u, inc.dt, var_u, R_start, R_start_dbg);
            rotate(theta, inc.dt, var_theta);
            const Eigen::Matrix3d dr_t = fromRotationVector(theta).toRotationMatrix().transpose();
            f_end = dr_t * u / inc.dt;
            df_dbg = -skew(dr_t * u) * rightJacobian(theta);
            df_dba = -dr_t;
            break;
        }
        case DvFrame::end:
            rotate(theta, inc.dt, var_theta);
            translate(p_.dR, u, inc.dt, var_u, R_start, R_start_dbg);
            f_end = u / inc.dt;
            df_dba = -Eigen::Matrix3d::Identity();
            break;
    }
    f_prev_ = f_end;
    df_prev_dbg_ = df_dbg;
    df_prev_dba_ = df_dba;
    have_prev_ = true;
    p_.dt += inc.dt;
    ++p_.samples;
    if (bridged) ++p_.bridged;
    return {};
}

void Preintegrator::translate(const Eigen::Matrix3d& R, const Eigen::Vector3d& u, double dt, double var_u,
                              const Eigen::Matrix3d& R_start, const Eigen::Matrix3d& R_start_dbg)
{
    // Error state [dphi, dv, dp] with R = R_hat Exp(dphi):
    //   dv' = dv - R [u]x dphi + R n
    //   dp' = dp + dv dt - 1/2 R [u]x dphi dt + 1/2 R n dt
    const Eigen::Matrix3d Ru = R * skew(u);
    Matrix9d A = Matrix9d::Identity();
    A.block<3, 3>(3, 0) = -Ru;
    A.block<3, 3>(6, 0) = -0.5 * Ru * dt;
    A.block<3, 3>(6, 3) = Eigen::Matrix3d::Identity() * dt;
    Eigen::Matrix<double, 9, 3> B = Eigen::Matrix<double, 9, 3>::Zero();
    B.block<3, 3>(3, 0) = R;
    B.block<3, 3>(6, 0) = 0.5 * R * dt;
    p_.cov = A * p_.cov * A.transpose() + var_u * B * B.transpose();

    // Bias Jacobians; position first, since it uses the old velocity terms.
    // u depends on ba through -ba dt, and so does the previous sample's
    // force in the slope term below.
    p_.dp_dbg += p_.dv_dbg * dt - 0.5 * Ru * p_.dR_dbg * dt;
    p_.dp_dba += p_.dv_dba * dt - 0.5 * R * dt * dt;
    if (have_prev_)
    {
        // slope = R u - R_start f_prev dt
        const Eigen::Matrix3d d_slope_dbg =
            -Ru * p_.dR_dbg + R_start * skew(f_prev_ * dt) * R_start_dbg - R_start * df_prev_dbg_ * dt;
        const Eigen::Matrix3d d_slope_dba = -R * dt - R_start * df_prev_dba_ * dt;
        p_.dp_dbg -= d_slope_dbg * (dt / 12.0);
        p_.dp_dba -= d_slope_dba * (dt / 12.0);
    }
    p_.dv_dbg -= Ru * p_.dR_dbg;
    p_.dv_dba -= R * dt;

    // Position needs the shape of the velocity increment within the sample,
    // which dv alone does not give. Half of it at mid-sample is exact for a
    // specific force constant in this interval's frame; a force that turns
    // (a car on a skidpad, whose centripetal direction sweeps round) leaves a
    // third-order error, 5e-6 m per 0.1 s at 1.3 g -- half the sensor's own
    // sigma. A force changing linearly between the previous sample and this
    // one removes it: the double integral picks up -dt/12 of the change. The
    // bias Jacobians above carry the term; the covariance does not, since it
    // is an order of dt below every noise term there.
    const Eigen::Vector3d du = R * u;
    Eigen::Vector3d slope = Eigen::Vector3d::Zero();
    if (have_prev_) slope = du - R_start * f_prev_ * dt;
    p_.dp += p_.dv * dt + 0.5 * du * dt - slope * (dt / 12.0);
    p_.dv += du;
}

void Preintegrator::rotate(const Eigen::Vector3d& theta, double dt, double var_theta)
{
    const Eigen::Matrix3d dr = fromRotationVector(theta).toRotationMatrix();
    const Eigen::Matrix3d jr = rightJacobian(theta);
    Matrix9d A = Matrix9d::Identity();
    A.block<3, 3>(0, 0) = dr.transpose();
    Eigen::Matrix<double, 9, 3> B = Eigen::Matrix<double, 9, 3>::Zero();
    B.block<3, 3>(0, 0) = jr;
    p_.cov = A * p_.cov * A.transpose() + var_theta * B * B.transpose();

    // theta depends on bg through -bg dt.
    p_.dR_dbg = dr.transpose() * p_.dR_dbg - jr * dt;
    p_.dR = p_.dR * dr;
}

}  // namespace imu_preint
