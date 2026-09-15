// SPDX-License-Identifier: GPL-3.0-or-later
//
// GSOF records onto their schemas.
//
// The receiver speaks radians and every schema here is in degrees, so the
// conversion happens in exactly one place -- and a position published in
// radians is still a position, a few thousand kilometres from where the vehicle
// is. Nothing downstream can tell, which is what makes this worth a test rather
// than a reading of the code.
#include "gsof_fields.h"

#include <capnp/message.h>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

void expectNear(double actual, double expected, double tolerance, const std::string& what)
{
    expect(std::abs(actual - expected) < tolerance,
           what + " (got " + std::to_string(actual) + ", expected " + std::to_string(expected) + ")");
}

constexpr double toRadians(double degrees)
{
    return degrees * std::numbers::pi / 180.0;
}

}  // namespace

int main()
{
    // Willow Springs, in the units the receiver uses.
    {
        gsof::LatLongHeight position;
        position.latitudeRad = toRadians(34.8711);
        position.longitudeRad = toRadians(-118.2614);
        position.heightM = 894.5;

        capnp::MallocMessageBuilder message;
        bd992_node::fill(message.initRoot<::GsofLatLongHeight>(), position);
        const auto out = message.getRoot<::GsofLatLongHeight>().asReader();

        expectNear(out.getLatitudeDeg(), 34.8711, 1e-6, "latitude is published in degrees");
        expectNear(out.getLongitudeDeg(), -118.2614, 1e-6, "longitude is published in degrees");
        expectNear(out.getEllipsoidHeightM(), 894.5, 1e-6, "height is metres either way");

        // The failure this is really about: radians published as degrees.
        expect(out.getLatitudeDeg() > 1.0, "latitude is not still in radians");
    }

    // Attitude: three angles that are all plausible as each other.
    {
        gsof::AttitudeInfo attitude;
        attitude.gpsTimeMs = 123456;
        attitude.pitchRad = toRadians(3.0);
        attitude.yawRad = toRadians(197.0);
        attitude.rollRad = toRadians(-1.5);
        attitude.masterSlaveRangeM = 1.25;

        capnp::MallocMessageBuilder message;
        bd992_node::fill(message.initRoot<::GsofAttitudeInfo>(), attitude);
        const auto out = message.getRoot<::GsofAttitudeInfo>().asReader();

        expectNear(out.getPitchDeg(), 3.0, 1e-4, "pitch");
        expectNear(out.getYawDeg(), 197.0, 1e-4, "yaw, which is the heading");
        expectNear(out.getRollDeg(), -1.5, 1e-4, "roll, including its sign");
        expectNear(out.getMasterSlaveRangeM(), 1.25, 1e-6, "the antenna baseline is metres");
        expect(out.getGpsTimeMs() == 123456, "the time of the fix");
    }

    // Velocity: a heading in radians beside speeds in m/s.
    {
        gsof::Velocity velocity;
        velocity.horizontalSpeedMps = 42.5F;
        velocity.headingRad = static_cast<float>(toRadians(275.0));
        velocity.verticalVelocityMps = -1.5F;

        capnp::MallocMessageBuilder message;
        bd992_node::fill(message.initRoot<::GsofVelocity>(), velocity);
        const auto out = message.getRoot<::GsofVelocity>().asReader();

        expectNear(out.getHorizontalSpeedMps(), 42.5, 1e-4, "horizontal speed");
        expectNear(out.getHeadingDeg(), 275.0, 1e-2, "heading in degrees");
        expectNear(out.getVerticalVelocityMps(), -1.5, 1e-4, "vertical velocity keeps its sign");
    }

    // The time every record is read against.
    {
        capnp::MallocMessageBuilder message;
        bd992_node::fillTime(message.initRoot<::GsofGpsTime>(), 2345, 86400123);
        const auto out = message.getRoot<::GsofGpsTime>().asReader();
        expect(out.getWeek() == 2345, "GPS week");
        expect(out.getTimeOfWeekMs() == 86400123, "time of week, in milliseconds");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
