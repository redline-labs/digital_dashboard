// SPDX-License-Identifier: GPL-3.0-or-later
#include "vehicle.h"

#include <algorithm>

namespace bd992_mock
{

Vehicle::Vehicle(const Path& path, const VehicleConfig& config, bool loop) :
    mPath(path), mConfig(config), mLoop(loop), mProfile(buildSpeedProfile(path, config))
{
    refreshState();
}

void Vehicle::step(double dt)
{
    if (dt <= 0.0 || mPath.size() < kMinPathPoints)
    {
        return;
    }

    const double length = mPath.lengthM();
    if (length <= 0.0)
    {
        return;
    }

    // The ceiling here already accounts for what is coming: buildSpeedProfile
    // relaxed it backwards, so "the limit at the place I am" is also "slow
    // enough to make the next corner".
    const double target = std::max(speedCapAt(mPath, mProfile, mS), 0.0);

    if (mV < target)
    {
        mV = std::min(target, mV + (mConfig.accelMps2 * dt));
    }
    else if (mV > target)
    {
        mV = std::max(target, mV - (mConfig.brakeMps2 * dt));
    }
    mV = std::max(mV, 0.0);

    mS += mV * dt;

    if (mPath.closed)
    {
        while (mS >= length)
        {
            mS -= length;
            ++mLap;
        }
    }
    else if (mS >= length)
    {
        mS = length;

        if (mLoop)
        {
            // Round again from the top. A route driven as a loop teleports back
            // to its start, which is honest -- there is no path connecting the
            // destination to the origin and inventing one would put the vehicle
            // on roads the router never chose. The jump is deliberate and
            // visible; a consumer seeing it knows the run restarted.
            mS = 0.0;
            mV = 0.0;
            ++mLap;
        }

        // NOT `mV = 0` otherwise. Arriving with a couple of m/s still on the
        // clock and zeroing it is a 20 m/s^2 deceleration in the published
        // velocity -- a discontinuity of exactly the kind this model exists to
        // avoid, and one that reads downstream as a receiver glitch rather than
        // as a car parking. The profile ends at rest, so the next tick sees a
        // target of zero here and brakes to it at the configured rate.
    }

    refreshState();
}

void Vehicle::refreshState()
{
    const PathPoint point = samplePath(mPath, mS, mConfig.headingLookaheadM);

    mState.latitudeDeg = road_graph::toDegrees(point.lat);
    mState.longitudeDeg = road_graph::toDegrees(point.lon);
    mState.headingDeg = point.headingDeg;
    mState.speedMps = mV;
    mState.alongM = mS;
    mState.lap = mLap;
    mState.moving = mV > 0.0;
}

double Vehicle::estimatedDurationS() const
{
    const std::size_t n = mPath.size();
    if (n < kMinPathPoints || mProfile.size() != n)
    {
        return 0.0;
    }

    double seconds = 0.0;
    for (std::size_t i = 1; i < n; ++i)
    {
        const double ds = mPath.distanceM[i] - mPath.distanceM[i - 1];
        // The mean of the two endpoint speeds, which is exact for constant
        // acceleration over the leg and close enough everywhere else.
        const double speed = (mProfile[i - 1] + mProfile[i]) * 0.5;
        if (speed > 0.0)
        {
            seconds += ds / speed;
        }
    }

    if (mPath.closed)
    {
        const double leg = mPath.lengthM() - mPath.distanceM[n - 1];
        const double speed = (mProfile[n - 1] + mProfile[0]) * 0.5;
        if (speed > 0.0 && leg > 0.0)
        {
            seconds += leg / speed;
        }
    }

    return seconds;
}

} // namespace bd992_mock
