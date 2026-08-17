// SPDX-License-Identifier: GPL-3.0-or-later
//
// A point mass driving a Path.
//
// The state is two numbers -- distance along the path and speed -- and
// everything published is derived from them. That is what makes the signals
// coherent rather than merely plausible: heading is the direction the path goes
// at the place the vehicle is, speed is the rate that place is changing, and
// there is no way for them to disagree because there is only one of them.
//
// Free of zenoh, capnp and clocks: step() takes its own dt, so a test can drive
// a thousand ticks in no time at all and assert on every one.

#ifndef BD992_MOCK_VEHICLE_H
#define BD992_MOCK_VEHICLE_H

#include <cstdint>
#include <vector>

#include "node_config.h"
#include "path.h"

namespace bd992_mock
{

// Everything the publishers need, in the units the bus uses.
struct VehicleState
{
    double latitudeDeg { 0.0 };
    double longitudeDeg { 0.0 };

    // Course over ground, degrees clockwise from true north. NOT attitude: this
    // node models no vehicle body, and a BD992 has no IMU, so the two would only
    // differ where this simulation has nothing to say anyway.
    double headingDeg { 0.0 };
    double speedMps { 0.0 };

    // Metres travelled along the path since the start, not counting laps.
    double alongM { 0.0 };
    // Completed wraps of a closed path.
    std::uint32_t lap { 0 };

    // False once an open path has been driven to its end and the vehicle has
    // come to rest. It keeps publishing -- a parked car is a real state and a
    // consumer must handle it -- but nothing changes after this.
    bool moving { false };
};

class Vehicle
{
  public:
    // `path` must outlive the vehicle. The speed profile is built once, here.
    Vehicle(const Path& path, const VehicleConfig& config, bool loop);

    // Advance by dt seconds.
    void step(double dt);

    const VehicleState& state() const { return mState; }

    // The profile, exposed for --check so a human can see where the speed
    // ceilings landed without running the thing.
    const std::vector<double>& speedProfile() const { return mProfile; }

    // How long one traverse takes at the profile's speeds, in seconds.
    // Approximate: it ignores the acceleration ramps, so it is a floor.
    double estimatedDurationS() const;

  private:
    void refreshState();

    const Path& mPath;
    VehicleConfig mConfig;
    bool mLoop { false };

    std::vector<double> mProfile;

    double mS { 0.0 };
    double mV { 0.0 };
    std::uint32_t mLap { 0 };

    VehicleState mState;
};

} // namespace bd992_mock

#endif // BD992_MOCK_VEHICLE_H
