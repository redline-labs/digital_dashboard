// SPDX-License-Identifier: GPL-3.0-or-later
//
// The preintegration covariance against Monte Carlo: integrate the same
// motion many times with the sensor's noise added, and the spread of the
// results must be the covariance the preintegrator claims. Too small and the
// smoother trusts the IMU over the GNSS; too large and it throws the IMU away.

#include "imu_preint/preintegrator.h"
#include "imu_preint/sim.h"

#include "trajectories.h"

#include <spdlog/spdlog.h>

#include <random>
#include <string>

int main()
{
    int failures = 0;
    const test_traj::Skidpad drift;
    const geodesy::NormalGravity g;
    const auto incs = imu_preint::simulateIncrements(drift, 0.0, 100.0, 50, imu_preint::DvFrame::end, g);

    // A noisier sensor than the MTi, so the effect of noise dominates the
    // second-order terms the covariance propagation drops.
    imu_preint::NoiseParams noise;
    noise.gyro_noise_density = 0.01;
    noise.accel_noise_density = 0.1;

    imu_preint::Preintegrator nominal(noise, imu_preint::DvFrame::end);
    for (const auto& inc : incs) nominal.integrate(inc);
    const auto& n = nominal.result();

    std::mt19937 rng(11);
    std::normal_distribution<double> gauss(0.0, 1.0);
    constexpr int kRuns = 4000;
    imu_preint::Matrix9d sample = imu_preint::Matrix9d::Zero();
    for (int run = 0; run < kRuns; ++run)
    {
        imu_preint::Preintegrator p(noise, imu_preint::DvFrame::end);
        for (auto inc : incs)
        {
            const double sg = noise.gyro_noise_density * std::sqrt(inc.dt);
            const double sa = noise.accel_noise_density * std::sqrt(inc.dt);
            const Eigen::Vector3d ng(gauss(rng), gauss(rng), gauss(rng)), na(gauss(rng), gauss(rng), gauss(rng));
            inc.dq = inc.dq * imu_preint::fromRotationVector(sg * ng);
            inc.dv += sa * na;
            p.integrate(inc);
        }
        Eigen::Matrix<double, 9, 1> e;
        e.segment<3>(0) = imu_preint::rotationVector(Eigen::Quaterniond(n.dR.transpose() * p.result().dR));
        e.segment<3>(3) = p.result().dv - n.dv;
        e.segment<3>(6) = p.result().dp - n.dp;
        sample += e * e.transpose() / kRuns;
    }

    // Diagonals within 10% (4000 runs puts the sampling error near 2%), and
    // the correlation structure within 0.1.
    for (int i = 0; i < 9; ++i)
    {
        const double ratio = sample(i, i) / n.cov(i, i);
        if (ratio < 0.9 || ratio > 1.1)
        {
            SPDLOG_ERROR("FAIL: variance {} Monte Carlo / predicted = {:.3f}", i, ratio);
            ++failures;
        }
    }
    for (int i = 0; i < 9; ++i)
        for (int j = 0; j < i; ++j)
        {
            const double rs = sample(i, j) / std::sqrt(sample(i, i) * sample(j, j));
            const double rp = n.cov(i, j) / std::sqrt(n.cov(i, i) * n.cov(j, j));
            if (std::fabs(rs - rp) > 0.1)
            {
                SPDLOG_ERROR("FAIL: correlation ({}, {}) Monte Carlo {:.3f}, predicted {:.3f}", i, j, rs, rp);
                ++failures;
            }
        }
    if (failures)
    {
        SPDLOG_ERROR("{} failure(s)", failures);
        return 1;
    }
    SPDLOG_INFO("covariance: Monte Carlo agrees with the propagated covariance");
    return 0;
}
