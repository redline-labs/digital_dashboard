#pragma once

// Where a preintegrated interval says the second keyframe is, in ECEF.
//
// Written once over the scalar so the IMU factor's residual (traced by csym)
// and the runtime propagation that produces initial guesses and IMU-rate
// output are the same formula -- two copies of a strapdown equation disagree
// at the first sign convention either one gets wrong.
//
// In the rotating ECEF frame, with omega the earth rate and g gravity
// (gravitation plus centrifugal):
//
//   R_j = Exp(-omega dt) R_i dR
//   v_j = v_i + R_i dv + (g - 2 omega x v_i) dt
//             - omega x (R_i dv dt + R_i dp) - omega x g dt^2
//   p_j = p_i + v_i dt + 1/2 g dt^2 + R_i dp
//             - omega x v_i dt^2 - omega x (R_i dp) dt - 1/3 omega x g dt^3
//
// The rotation is exact for a constant omega. The rest is exact to first
// order in omega dt (7e-6 over a 0.1 s keyframe): R_e_b(t) = Exp(-omega t)
// R_i dR(t) rotates the preintegrated specific force out of the frame it was
// summed in, and Coriolis acts on the velocity along the way; integrating
// both by parts gives the dp terms. Dropping them is not harmless -- parked,
// the last two terms are 1/6 omega x g dt^3, 8e-5 m over one second.

#include "csym/geo/rot3.h"
#include "csym/matrix.h"

#include <tuple>

namespace imu_preint
{

template <class T>
struct Predicted
{
    csym::Rot3<T> R;
    csym::Vector3<T> p;
    csym::Vector3<T> v;
};

// dR, dv, dp and their bias Jacobians as preintegrated at (bg0, ba0);
// (bg, ba) is the bias estimate to correct them to.
template <class T>
constexpr Predicted<T> predict(const csym::Rot3<T>& Ri, const csym::Vector3<T>& pi, const csym::Vector3<T>& vi,
                               const csym::Vector3<T>& bg, const csym::Vector3<T>& ba, const csym::Rot3<T>& dR,
                               const csym::Vector3<T>& dv, const csym::Vector3<T>& dp,
                               const csym::Matrix33<T>& dR_dbg, const csym::Matrix33<T>& dv_dbg,
                               const csym::Matrix33<T>& dv_dba, const csym::Matrix33<T>& dp_dbg,
                               const csym::Matrix33<T>& dp_dba, const csym::Vector3<T>& bg0,
                               const csym::Vector3<T>& ba0, const T& dt, const csym::Vector3<T>& g,
                               const csym::Vector3<T>& omega, const T& eps)
{
    const csym::Vector3<T> dbg = bg - bg0;
    const csym::Vector3<T> dba = ba - ba0;
    const csym::Rot3<T> dRc = dR * csym::Rot3<T>::from_tangent(dR_dbg * dbg, eps);
    const csym::Vector3<T> dvc = dv + dv_dbg * dbg + dv_dba * dba;
    const csym::Vector3<T> dpc = dp + dp_dbg * dbg + dp_dba * dba;

    const csym::Vector3<T> Rdv = Ri * dvc;
    const csym::Vector3<T> Rdp = Ri * dpc;
    const csym::Vector3<T> wv = omega.cross(vi);
    const csym::Vector3<T> wg = omega.cross(g);
    Predicted<T> out;
    out.R = csym::Rot3<T>::from_tangent(omega * (-dt), eps) * Ri * dRc;
    out.v = vi + Rdv + (g - wv * T(2)) * dt - omega.cross(Rdv * dt + Rdp) - wg * (dt * dt);
    out.p = pi + vi * dt + g * (T(0.5) * dt * dt) + Rdp - wv * (dt * dt) - omega.cross(Rdp) * dt -
            wg * (dt * dt * dt / T(3));
    return out;
}

}  // namespace imu_preint
