// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a keyframe costs the solver. A fixed-lag window that has already
// converged gains one keyframe at a time, and the problem is close to linear:
// Gauss-Newton should finish in a couple of iterations. One that runs to its
// iteration cap every time is paying several times over for nothing, and it
// shows only as solve time -- the estimate is the same -- so the iteration
// count is asserted here, where it is deterministic, rather than the time.

#include "harness.h"

#include <map>

#include <time.h>

using harness::check;
namespace sim = vehicle_estimator::sim;

namespace
{

// This thread's CPU time: wall time on a shared machine measures the machine.
double threadSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + 1e-9 * static_cast<double>(ts.tv_nsec);
}

struct Cost
{
    std::size_t keyframes = 0;
    std::map<int, std::size_t> iterations;  // iterations -> keyframes
    std::map<std::string, std::size_t> stops;
    std::size_t rejected = 0;
    std::uint64_t covariance_from_solve = 0;
    double process_s = 0.0;
    harness::Errors errors;
    double median_iterations() const
    {
        std::size_t seen = 0;
        for (const auto& [n, count] : iterations)
        {
            seen += count;
            if (2 * seen >= keyframes) return n;
        }
        return 0;
    }
};

Cost drive(const sim::Scenario& sc, vehicle_estimator::Estimator& est)
{
    Cost c;
    std::uint64_t seen = 0;
    for (const auto& m : sc.messages())
    {
        if (m.imu) est.addImu(*m.imu);
        if (m.gnss) est.addGnss(*m.gnss);
        const double t0 = threadSeconds();
        est.process();
        c.process_s += threadSeconds() - t0;
        const auto& st = est.status();
        if (st.keyframes == seen || !st.initialized) continue;
        seen = st.keyframes;
        ++c.keyframes;
        ++c.iterations[st.last_optimize.iterations];
        ++c.stops[st.last_optimize.stop_reason];
        c.rejected += static_cast<std::size_t>(st.last_optimize.rejected_steps);
        c.covariance_from_solve = st.covariance_from_solve;
    }
    return c;
}

void report(const char* label, const Cost& c)
{
    std::string hist, stops;
    for (const auto& [n, count] : c.iterations) hist += fmt::format(" {}:{}", n, count);
    for (const auto& [s, count] : c.stops) stops += fmt::format(" '{}':{}", s, count);
    SPDLOG_INFO("{}: {} keyframes, {:.2f} ms CPU per keyframe in process(); iterations{}; stops{}; {} rejected steps", label,
                c.keyframes, 1e3 * c.process_s / static_cast<double>(std::max<std::size_t>(c.keyframes, 1)), hist,
                stops, c.rejected);
}

}  // namespace

int main()
{
    sim::SensorModel sensors;
    sensors.seed = 91;
    sim::TrackParams tp;
    tp.laps = 2;
    sim::Scenario sc(sim::track(tp), sensors);
    vehicle_estimator::Estimator est(harness::configFor(sensors));
    const Cost c = drive(sc, est);
    report("track, two laps", c);
    check(c.keyframes > 500, "the drive ran");
    // Measured 2026-09-24: 2 iterations for nearly every keyframe, none
    // rejected. It was 8 -- the cap -- on every keyframe: a Marquardt damping
    // that throttled the window's weak directions, and steps below what the
    // cost resolves in ECEF rejected and retried.
    const std::size_t converged = c.stops.contains("converged") ? c.stops.at("converged") : 0;
    check(c.median_iterations() <= 3, fmt::format("a keyframe converges in a few iterations (median {})", c.median_iterations()));
    check(100 * converged >= 99 * c.keyframes, "and converges, rather than running out of them");
    check(100 * c.rejected <= c.keyframes, fmt::format("with almost no rejected steps ({})", c.rejected));
    // The covariance comes from the solve's last factorisation only while
    // the damping is negligible (EstimatorConfig::lm): raise initial_lambda
    // and every keyframe silently pays for a second one.
    check(100 * c.covariance_from_solve >= 95 * c.keyframes,
          fmt::format("and its covariance from the solve's factorisation ({} of {})", c.covariance_from_solve, c.keyframes));
    // The same drive's accuracy, so a faster solver cannot be a sloppier one.
    vehicle_estimator::Estimator again(harness::configFor(sensors));
    const auto r = harness::run(sc, again, 10.0);
    SPDLOG_INFO("accuracy: position {:.3f} m, yaw {:.3f} deg, sideslip {:.3f} deg", r.errors.position,
                r.errors.yaw / harness::kDeg, r.errors.sideslip / harness::kDeg);
    check(r.errors.position < 0.10 && r.errors.yaw < 0.3 * harness::kDeg && r.errors.sideslip < 0.5 * harness::kDeg,
          "and the estimate is as good as ever");

    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("solver: all passed");
    return 0;
}
