// SPDX-License-Identifier: GPL-3.0-or-later
//
// Whether the estimator's sigmas mean what they say.
//
// Twenty drives of the same figure of eight, each with its own sensor noise.
// At every keyframe the squared error in units of the reported covariance
// (NEES) is recorded; averaged over the runs it should sit near the number
// of dimensions. Far above and the estimator is overconfident -- the output a
// consumer would trust while it is wrong; far below and it is throwing away
// information. The bounds are generous (a smoother with robust losses and a
// clock alignment is not the textbook linear case) but they have to hold.

#include "harness.h"

using harness::check;
namespace sim = vehicle_estimator::sim;

int main()
{
    constexpr int kRuns = 20;
    double nees_pos = 0.0, nees_vel = 0.0, nees_yaw = 0.0;
    std::size_t samples = 0;
    for (int run = 0; run < kRuns; ++run)
    {
        sim::SensorModel sensors;
        sensors.seed = 100 + static_cast<unsigned>(run);
        sim::FigureEightParams p;
        p.drive = 30.0;
        sim::Scenario sc(sim::figureEight(p), sensors);
        vehicle_estimator::Estimator est(harness::configFor(sensors));
        const auto r = harness::run(sc, est, 12.0);
        for (std::size_t k = 0; k < r.states.size(); ++k)
        {
            const auto& s = r.states[k];
            if (sc.toScenarioTime(s.gps_time) < 12.0) continue;
            const auto& t = r.truths[k];
            // Diagonal NEES in NED: the reported sigmas are per axis.
            const auto llh_err = [&] {
                // Position error in local NED at the truth.
                const double lat = s.lat, lon = s.lon;
                const double sl = std::sin(lat), cl = std::cos(lat), so = std::sin(lon), co = std::cos(lon);
                Eigen::Matrix3d R_e_n;
                R_e_n << -sl * co, -so, -cl * co, -sl * so, co, -cl * so, cl, 0.0, -sl;
                return Eigen::Vector3d(R_e_n.transpose() * (s.p_e - t.p_e));
            }();
            nees_pos += llh_err.cwiseQuotient(s.sigma_position_ned).squaredNorm();
            nees_vel += (s.v_ned - t.v_ned).cwiseQuotient(s.sigma_velocity_ned).squaredNorm();
            const double dyaw = harness::wrap(s.yaw - t.yaw) / s.sigma_attitude.z();
            nees_yaw += dyaw * dyaw;
            ++samples;
        }
    }
    const double n = static_cast<double>(samples);
    nees_pos /= n;
    nees_vel /= n;
    nees_yaw /= n;
    SPDLOG_INFO("consistency over {} keyframes: mean NEES position {:.2f} (3), velocity {:.2f} (3), yaw {:.2f} (1)",
                samples, nees_pos, nees_vel, nees_yaw);
    check(nees_pos > 0.3 && nees_pos < 12.0, "position sigmas mean something");
    check(nees_vel > 0.3 && nees_vel < 12.0, "velocity sigmas mean something");
    check(nees_yaw > 0.1 && nees_yaw < 4.0, "yaw sigma means something");
    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("consistency: all passed");
    return 0;
}
