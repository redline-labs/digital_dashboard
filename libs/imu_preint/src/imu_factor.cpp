// The IMU factor's residual, compiled by csym. Its own translation unit: the
// residual is a few thousand operations and building its program is the
// slowest thing in this library's compile.

#include "imu_preint/imu_factor.h"
#include "imu_preint/imu_model.h"

#include "factor_graph/csym_factor.h"
#include "factor_graph/values.h"

#include <Eigen/Cholesky>

#include <cmath>
#include <stdexcept>

namespace imu_preint
{

namespace
{

using V3 = csym::Vector3<double>;
using M33 = csym::Matrix33<double>;
using Rot3 = csym::Rot3<double>;

constexpr auto kImuResidual = [](auto Ri, auto pi, auto vi, auto bg, auto ba, auto Rj, auto pj, auto vj, auto dR,
                                 auto dv, auto dp, auto dR_dbg, auto dv_dbg, auto dv_dba, auto dp_dbg, auto dp_dba,
                                 auto bg0, auto ba0, auto dt, auto g, auto omega, auto eps) {
    using T = decltype(dt);
    const auto pred = predict(Ri, pi, vi, bg, ba, dR, dv, dp, dR_dbg, dv_dbg, dv_dba, dp_dbg, dp_dba, bg0, ba0, dt, g,
                              omega, eps);
    const csym::Rot3<T> Ri_inv = Ri.inverse();
    const csym::Vector3<T> rR = csym::local_coordinates(pred.R, Rj, eps);
    const csym::Vector3<T> rv = Ri_inv * (vj - pred.v);
    const csym::Vector3<T> rp = Ri_inv * (pj - pred.p);
    csym::Vector<T, 9> r;
    for (std::size_t k = 0; k < 3; ++k)
    {
        r[k] = rR[k];
        r[3 + k] = rv[k];
        r[6 + k] = rp[k];
    }
    return r;  // whitened by the factor, numerically: see makeImuFactor
};

using ImuFactor = factor_graph::CsymFactor<
    kImuResidual, factor_graph::Vars<Rot3, V3, V3, V3, V3, Rot3, V3, V3>,
    factor_graph::Params<Rot3, V3, V3, M33, M33, M33, M33, M33, V3, V3, double, V3, V3, double>>;

constexpr auto kBiasWalk = [](auto bi, auto bj, auto inv_sigma) { return (bj - bi) * inv_sigma; };
using BiasWalkFactor = factor_graph::CsymFactor<kBiasWalk, factor_graph::Vars<V3, V3>, factor_graph::Params<double>>;

V3 toCsym(const Eigen::Vector3d& v)
{
    return V3{v.x(), v.y(), v.z()};
}

M33 toCsym(const Eigen::Matrix3d& m)
{
    M33 out;
    for (std::size_t r = 0; r < 3; ++r)
        for (std::size_t c = 0; c < 3; ++c) out(r, c) = m(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c));
    return out;
}

Rot3 toCsym(const Eigen::Quaterniond& q)
{
    const Eigen::Quaterniond n = q.normalized();
    return Rot3(n.x(), n.y(), n.z(), n.w());
}

}  // namespace

std::shared_ptr<const factor_graph::Factor> makeImuFactor(const KeyframeKeys& i, const KeyframeKeys& j,
                                                          const Preintegrated& pim, const Eigen::Vector3d& gravity_e,
                                                          const Eigen::Vector3d& omega_ie)
{
    // Whitening S with S^T S = cov^-1: S = L^-1 for cov = L L^T.
    const Eigen::LLT<Matrix9d> llt(pim.cov);
    if (llt.info() != Eigen::Success) throw std::invalid_argument("IMU factor: covariance is not positive definite");
    const Matrix9d sqrt_info = llt.matrixL().solve(Matrix9d::Identity());

    // Whitened after the residual program rather than inside it: a 9 x 9
    // product in the traced residual was most of this file's compile time.
    return std::make_shared<ImuFactor>(
        factor_graph::Whitened{}, "imu", std::array<factor_graph::Key, 8>{i.R, i.p, i.v, i.bg, i.ba, j.R, j.p, j.v},
        sqrt_info,
        toCsym(Eigen::Quaterniond(pim.dR)), toCsym(pim.dv), toCsym(pim.dp), toCsym(pim.dR_dbg), toCsym(pim.dv_dbg),
        toCsym(pim.dv_dba), toCsym(pim.dp_dbg), toCsym(pim.dp_dba), toCsym(pim.bg_lin), toCsym(pim.ba_lin), pim.dt,
        toCsym(gravity_e), toCsym(omega_ie), factor_graph::kEpsilon);
}

std::shared_ptr<const factor_graph::Factor> makeBiasWalkFactor(factor_graph::Key bi, factor_graph::Key bj,
                                                               double sigma, double dt, const char* name)
{
    if (!(sigma > 0.0) || !(dt > 0.0)) throw std::invalid_argument("bias walk: sigma and dt must be positive");
    return std::make_shared<BiasWalkFactor>(name, std::array<factor_graph::Key, 2>{bi, bj},
                                            1.0 / (sigma * std::sqrt(dt)));
}

NavState predict(const NavState& i, const Eigen::Vector3d& bg, const Eigen::Vector3d& ba, const Preintegrated& pim,
                 const Eigen::Vector3d& gravity_e, const Eigen::Vector3d& omega_ie)
{
    const auto p = imu_preint::predict<double>(
        toCsym(i.R_e_b), toCsym(i.p_e), toCsym(i.v_e), toCsym(bg), toCsym(ba), toCsym(Eigen::Quaterniond(pim.dR)),
        toCsym(pim.dv), toCsym(pim.dp), toCsym(pim.dR_dbg), toCsym(pim.dv_dbg), toCsym(pim.dv_dba),
        toCsym(pim.dp_dbg), toCsym(pim.dp_dba), toCsym(pim.bg_lin), toCsym(pim.ba_lin), pim.dt, toCsym(gravity_e),
        toCsym(omega_ie), factor_graph::kEpsilon);
    NavState out;
    out.R_e_b = Eigen::Quaterniond(p.R.w, p.R.x, p.R.y, p.R.z).normalized();
    out.p_e = Eigen::Vector3d(p.p[0], p.p[1], p.p[2]);
    out.v_e = Eigen::Vector3d(p.v[0], p.v[1], p.v[2]);
    return out;
}

}  // namespace imu_preint
