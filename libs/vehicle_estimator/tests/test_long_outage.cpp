// SPDX-License-Identifier: GPL-3.0-or-later
//
// Five minutes without GNSS. The window's covariance is the one thing that
// says how far to trust the output, and in a long outage it is also the
// hardest to compute: each keyframe's position is held to its neighbours by
// the IMU chain at ~1e11 of information, and to the world by what little the
// marginal prior still knows -- a ratio that falls with the square of the
// growing position sigma, past what a factorisation of the normal equations
// can resolve in double precision. When it could not, the covariance
// vanished: every sigma read zero, and a perfectly good attitude stopped
// being claimed valid.

#include "harness.h"

#include "geodesy/geodetic.h"

using harness::check;
namespace sim = vehicle_estimator::sim;

namespace
{

struct Outage
{
    std::size_t keyframes = 0;        // inside the outage
    std::size_t without_covariance = 0;
    std::size_t attitude_invalid = 0;
    double worst_z = 0.0;             // worst horizontal position error in its own sigmas
    double sigma_at_end = 0.0;        // horizontal position sigma as the outage ends
    double error_at_end = 0.0;
    double worst_after = 0.0;         // position error once GNSS is back and settled
};

Outage drive(double outage_s, unsigned seed)
{
    sim::SensorModel sensors;
    sensors.seed = seed;
    sensors.outages = {{60.0, 60.0 + outage_s}};
    sim::TrackParams tp;
    tp.laps = static_cast<int>((90.0 + outage_s) / 32.0) + 1;
    sim::Scenario sc(sim::track(tp), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const auto run = harness::run(sc, est, 10.0);

    Outage o;
    for (std::size_t k = 0; k < run.states.size(); ++k)
    {
        const auto& s = run.states[k];
        const auto& truth = run.truths[k];
        const double t = sc.toScenarioTime(s.gps_time);
        // Horizontal position error in the local frame, against its sigma.
        const Eigen::Vector3d err_e = s.p_e - truth.p_e;
        const auto R_e_n = geodesy::rotEcefFromNed(s.lat, s.lon);
        Eigen::Vector3d err_n;
        for (Eigen::Index i = 0; i < 3; ++i)
            err_n[i] = R_e_n(0, static_cast<std::size_t>(i)) * err_e.x() + R_e_n(1, static_cast<std::size_t>(i)) * err_e.y() +
                       R_e_n(2, static_cast<std::size_t>(i)) * err_e.z();
        const double err_h = err_n.head<2>().norm();
        const double sigma_h = s.sigma_position_ned.head<2>().norm();
        if (t >= 61.0 && t < 60.0 + outage_s)
        {
            ++o.keyframes;
            if (!(s.sigma_attitude.minCoeff() > 0.0)) ++o.without_covariance;
            if (!s.attitude_valid) ++o.attitude_invalid;
            if (sigma_h > 0.0) o.worst_z = std::max(o.worst_z, err_h / sigma_h);
            o.sigma_at_end = sigma_h;
            o.error_at_end = err_h;
        }
        if (t > 60.0 + outage_s + 20.0) o.worst_after = std::max(o.worst_after, (s.p_e - truth.p_e).norm());
    }
    SPDLOG_INFO("{:.0f} s outage: {} keyframes, {} without a covariance, {} with attitude not valid; horizontal "
                "error {:.1f} m against sigma {:.1f} m at the end, worst {:.2f} sigma; {:.3f} m once GNSS is back",
                outage_s, o.keyframes, o.without_covariance, o.attitude_invalid, o.error_at_end, o.sigma_at_end,
                o.worst_z, o.worst_after);
    return o;
}

}  // namespace

int main()
{
    const Outage o = drive(300.0, 101);
    check(o.keyframes > 2500, "the outage ran");
    check(o.without_covariance == 0, "every keyframe of a five-minute outage has a covariance");
    check(o.attitude_invalid == 0, "and its attitude stays valid");
    check(o.sigma_at_end > 5.0, "the position sigma grows well past where it used to vanish");
    check(o.worst_z < 4.0, "and still covers the error");
    check(o.worst_after < 0.2, "and the estimate comes back when GNSS does");

    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("long outage: all passed");
    return 0;
}
