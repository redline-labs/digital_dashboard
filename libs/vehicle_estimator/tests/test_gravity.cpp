// SPDX-License-Identifier: GPL-3.0-or-later
//
// The deflection of the vertical in the estimator. In the Colorado foothills
// the plumb line leans ~28" east of the ellipsoid normal, and truth gravity
// here carries it. Left unmodelled, the estimator has to put that lean
// somewhere, and with GNSS holding position and velocity it goes into the
// attitude: a steady tilt, the deflection itself, east. Modelled, there is
// nothing to put anywhere. The tilt is averaged over minutes of laps, where
// the attitude's own noise averages out and a lean of a few arcseconds does
// not. (Through an outage the same arcseconds are real but small next to
// what a straight road does to an unaided MTi's heading; a scenario built to
// show them there measured the heading, not the gravity.)

#include "harness.h"

#include "core/core.h"
#include "deflec/model.h"

#include <cmath>
#include <numbers>

using harness::check;
namespace sim = vehicle_estimator::sim;

namespace
{

constexpr double kArcsec = std::numbers::pi / (180.0 * 3600.0);

struct Tilt
{
    double north = 0.0, east = 0.0;  // arcseconds, mean attitude tilt error in the local frame
    bool deflection_reported = false;
};

Tilt drive(const std::shared_ptr<const geodesy::GravityModel>& model,
           const std::shared_ptr<const geodesy::GravityModel>& truth, double lat_deg, double lon_deg)
{
    sim::SensorModel sensors;
    sensors.seed = 131;
    sensors.gravity = truth;
    sim::TrackParams tp;
    tp.laps = 10;
    sim::Scenario sc(sim::track(tp), sensors, lat_deg, lon_deg, 1700.0);
    auto config = harness::configFor(sensors);
    config.gravity = model;
    vehicle_estimator::Estimator est(config);
    const auto run = harness::run(sc, est, 10.0);
    Tilt t;
    t.deflection_reported = run.status.gravity_deflection;
    std::size_t n = 0;
    for (std::size_t k = 0; k < run.states.size(); ++k)
    {
        if (sc.toScenarioTime(run.states[k].gps_time) < 60.0) continue;  // settled
        // Attitude error as a small rotation in the local frame: its north
        // and east parts are the tilt.
        const auto& st = run.states[k];
        const Eigen::Matrix3d R_est = (Eigen::AngleAxisd(st.yaw, Eigen::Vector3d::UnitZ()) *
                                       Eigen::AngleAxisd(st.pitch, Eigen::Vector3d::UnitY()) *
                                       Eigen::AngleAxisd(st.roll, Eigen::Vector3d::UnitX()))
                                          .toRotationMatrix();
        const Eigen::AngleAxisd aa(R_est * run.truths[k].R_n_b.transpose());
        const Eigen::Vector3d rot_n = aa.angle() * aa.axis();
        t.north += rot_n.x() / kArcsec;
        t.east += rot_n.y() / kArcsec;
        ++n;
    }
    t.north /= static_cast<double>(n);
    t.east /= static_cast<double>(n);
    return t;
}

}  // namespace

int main()
{
    // NGS's model, verbatim from models/deflec2022, found as the node finds it.
    auto opened = deflec::Model::open(core::paths::resource("models/deflec2022"));
    if (!opened)
    {
        SPDLOG_WARN("SKIP: {}", opened.error().message);
        const bool absent = opened.error().error == deflec::LoadError::lfs_pointer ||
                            opened.error().error == deflec::LoadError::not_found;
        return absent ? PROJECT_TEST_SKIP_CODE : 1;
    }
    const auto model = std::make_shared<const deflec::Model>(std::move(*opened));
    const auto deflected = std::make_shared<const deflec::DeflectedGravity>(model);

    const double lat = 40.02, lon = -105.35;  // the Boulder foothills
    const auto d = *model->at(lat * std::numbers::pi / 180.0, lon * std::numbers::pi / 180.0);
    const Tilt consistent = drive(nullptr, nullptr, lat, lon);
    const Tilt modelled = drive(deflected, deflected, lat, lon);
    const Tilt ignored = drive(nullptr, deflected, lat, lon);
    SPDLOG_INFO("deflection here: xi {:.1f}\", eta {:.1f}\". Mean attitude tilt error (north, east): no deflection "
                "anywhere ({:.1f}, {:.1f})\", modelled ({:.1f}, {:.1f})\", ignored ({:.1f}, {:.1f})\"",
                d.xi_arcsec, d.eta_arcsec, consistent.north, consistent.east, modelled.north, modelled.east,
                ignored.north, ignored.east);
    check(modelled.deflection_reported && !ignored.deflection_reported, "the status says when the deflection is applied");
    // Modelled, it is as if there were none: the same tilt error as a world
    // without deflection (which is the attitude's own, a few arcseconds).
    check(std::hypot(modelled.north - consistent.north, modelled.east - consistent.east) < 0.5,
          "modelled, the deflection leaves no trace in the attitude");
    // Ignored, the attitude leans by the deflection: an eastward lean eta is a
    // rotation about north, of -eta; a northward xi one about east, of +xi.
    // Measured 2026-09-24: (-27.0", +2.5") against (-29.4", +0.6").
    const double dn = ignored.north - consistent.north, de = ignored.east - consistent.east;
    check(std::hypot(dn + d.eta_arcsec, de - d.xi_arcsec) < 0.2 * std::hypot(d.xi_arcsec, d.eta_arcsec),
          "ignored, the attitude leans by the deflection");

    if (harness::failures)
    {
        SPDLOG_ERROR("{} failure(s)", harness::failures);
        return 1;
    }
    SPDLOG_INFO("gravity: all passed");
    return 0;
}
