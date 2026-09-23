// SPDX-License-Identifier: GPL-3.0-or-later

#include "gnss_assembler.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace state_estimator
{

namespace
{

constexpr double kDeg = std::numbers::pi / 180.0;
constexpr double kWeekS = 604800.0;

}  // namespace

GnssAssembler::GnssAssembler(AssemblerOptions options) : options_(options) {}

GnssAssembler::Burst& GnssAssembler::burstFor(double arrival, bool already_has)
{
    if (open_ && (arrival - open_->first > options_.burst || already_has)) close();
    if (!open_) open_ = Burst{arrival, {}, {}, {}, {}};
    return *open_;
}

void GnssAssembler::addTime(const GpsTimeRecord& r, double arrival)
{
    week_ = r.week;
    Burst& b = burstFor(arrival, open_ && open_->time.has_value());
    b.time = r;
}

void GnssAssembler::addPosition(const LatLongHeightRecord& r, double arrival)
{
    Burst& b = burstFor(arrival, open_ && open_->position.has_value());
    b.position = r;
}

void GnssAssembler::addVelocity(const VelocityRecord& r, double arrival)
{
    Burst& b = burstFor(arrival, open_ && open_->velocity.has_value());
    b.velocity = r;
}

void GnssAssembler::addAttitude(const AttitudeRecord& r, double arrival)
{
    Burst& b = burstFor(arrival, open_ && open_->attitude.has_value());
    b.attitude = r;
}

void GnssAssembler::addSigma(const SigmaRecord& r, double arrival)
{
    sigma_ = std::make_pair(r, arrival);
}

void GnssAssembler::addFix(vehicle_estimator::FixQuality fix, double arrival)
{
    fix_ = std::make_pair(fix, arrival);
}

std::vector<vehicle_estimator::GnssEpoch> GnssAssembler::poll(double now)
{
    if (open_ && now - open_->first > options_.burst) close();
    std::vector<vehicle_estimator::GnssEpoch> out;
    out.swap(ready_);
    return out;
}

void GnssAssembler::close()
{
    if (!open_) return;
    const Burst b = *open_;
    open_.reset();
    if (!b.position && !b.velocity && !b.attitude) return;

    vehicle_estimator::GnssEpoch e;
    e.host_time = b.first;
    if (b.time)
    {
        e.gps_time = b.time->week * kWeekS + b.time->timeOfWeekMs * 1e-3;
    }
    else if (b.attitude)
    {
        if (!week_)
        {
            ++stats_.noWeek;
            return;
        }
        e.gps_time = *week_ * kWeekS + b.attitude->timeOfWeekMs * 1e-3;
    }
    else
    {
        ++stats_.noTime;
        return;
    }

    const bool fix_fresh = fix_ && b.first - fix_->second <= options_.fixFreshFor;
    // Without a fix type the receiver's word on its own quality is missing;
    // treat it as the weakest fix that is still a fix.
    e.fix = fix_fresh ? fix_->first : vehicle_estimator::FixQuality::autonomous;

    if (b.position)
    {
        vehicle_estimator::GnssPosition p;
        p.lat = b.position->latitudeDeg * kDeg;
        p.lon = b.position->longitudeDeg * kDeg;
        p.h = b.position->ellipsoidHeightM;
        if (sigma_ && b.first - sigma_->second <= options_.sigmaFreshFor)
        {
            const SigmaRecord& s = sigma_->first;
            p.cov_ned << s.sigmaNorthM * s.sigmaNorthM, s.covarianceEastNorth, 0.0,  //
                s.covarianceEastNorth, s.sigmaEastM * s.sigmaEastM, 0.0,              //
                0.0, 0.0, s.sigmaUpM * s.sigmaUpM;
        }
        else
        {
            const double h = options_.defaultSigmaHorizontalM, v = options_.defaultSigmaVerticalM;
            p.cov_ned = Eigen::Vector3d(h * h, h * h, v * v).asDiagonal();
        }
        e.position = p;
    }
    if (b.velocity && b.velocity->valid)
    {
        const double heading = b.velocity->headingDeg * kDeg;
        vehicle_estimator::GnssVelocity v;
        v.v_ned = Eigen::Vector3d(b.velocity->horizontalSpeedMps * std::cos(heading),
                                  b.velocity->horizontalSpeedMps * std::sin(heading),
                                  -b.velocity->verticalVelocityMps);
        e.velocity = v;
    }
    if (b.attitude && b.attitude->yawValid && b.attitude->pitchValid)
    {
        // Only the one that belongs to this epoch: a heading from another
        // time of week is another epoch's.
        const double att_t = (week_ ? *week_ : 0) * kWeekS + b.attitude->timeOfWeekMs * 1e-3;
        if (!b.time || std::fabs(att_t - e.gps_time) < 1e-3)
        {
            vehicle_estimator::DualAntenna a;
            a.yaw = b.attitude->yawDeg * kDeg;
            a.pitch = b.attitude->pitchDeg * kDeg;
            if (b.attitude->hasVariance)
            {
                const auto clampVar = [](double v) { return std::clamp(v, 1e-7, 0.04); };
                Eigen::Matrix2d c;
                c << clampVar(b.attitude->yawVariance), b.attitude->pitchYawCovariance,
                    b.attitude->pitchYawCovariance, clampVar(b.attitude->pitchVariance);
                if (c.determinant() > 0.0) a.cov = c;
            }
            e.attitude = a;
        }
    }
    if (!e.position && !e.velocity && !e.attitude) return;
    ++stats_.epochs;
    ready_.push_back(e);
}

}  // namespace state_estimator
