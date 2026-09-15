// SPDX-License-Identifier: GPL-3.0-or-later
//
// XBus data items onto their schemas.
//
// An inertial reading is three numbers whose meaning is entirely in which axis
// each one is. Two of them swapped is a vehicle that corners when it brakes,
// and every value stays plausible -- so each axis is given a number nothing
// else has, which is the only way a transposition shows up.
#include "xbus_fields.h"

#include <capnp/message.h>

#include <cmath>
#include <cstdio>
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

void expectNear(double actual, double expected, const std::string& what)
{
    expect(std::abs(actual - expected) < 1e-9,
           what + " (got " + std::to_string(actual) + ", expected " + std::to_string(expected) + ")");
}

}  // namespace

int main()
{
    {
        xbus::Acceleration acceleration;
        acceleration.xMps2 = 1.5;
        acceleration.yMps2 = -2.5;
        acceleration.zMps2 = 9.81;

        capnp::MallocMessageBuilder message;
        mti610_node::fill(message.initRoot<::XbusAcceleration>(), acceleration);
        const auto out = message.getRoot<::XbusAcceleration>().asReader();

        expectNear(out.getAccelerationXMps2(), 1.5, "acceleration X");
        expectNear(out.getAccelerationYMps2(), -2.5, "acceleration Y, including its sign");
        expectNear(out.getAccelerationZMps2(), 9.81, "acceleration Z, which is the one with gravity in it");
    }

    {
        xbus::RateOfTurn rate;
        rate.xRadps = 0.1;
        rate.yRadps = 0.2;
        rate.zRadps = 0.3;

        capnp::MallocMessageBuilder message;
        mti610_node::fill(message.initRoot<::XbusRateOfTurn>(), rate);
        const auto out = message.getRoot<::XbusRateOfTurn>().asReader();

        expectNear(out.getRateOfTurnXRadps(), 0.1, "rate of turn X");
        expectNear(out.getRateOfTurnYRadps(), 0.2, "rate of turn Y");
        expectNear(out.getRateOfTurnZRadps(), 0.3, "rate of turn Z, which is yaw rate");
    }

    {
        // The high-rate variants carry the same axes and are the pair most
        // easily crossed with the ordinary ones.
        xbus::AccelerationHr acceleration;
        acceleration.xMps2 = 11.0;
        acceleration.yMps2 = 12.0;
        acceleration.zMps2 = 13.0;

        capnp::MallocMessageBuilder message;
        mti610_node::fill(message.initRoot<::XbusAccelerationHr>(), acceleration);
        const auto out = message.getRoot<::XbusAccelerationHr>().asReader();

        expectNear(out.getAccelerationXMps2(), 11.0, "high-rate acceleration X");
        expectNear(out.getAccelerationYMps2(), 12.0, "high-rate acceleration Y");
        expectNear(out.getAccelerationZMps2(), 13.0, "high-rate acceleration Z");
    }

    {
        xbus::MagneticField field;
        field.xAu = 0.4;
        field.yAu = -0.5;
        field.zAu = 0.6;

        capnp::MallocMessageBuilder message;
        mti610_node::fill(message.initRoot<::XbusMagneticField>(), field);
        const auto out = message.getRoot<::XbusMagneticField>().asReader();

        expectNear(out.getMagneticFieldXAu(), 0.4, "magnetic field X");
        expectNear(out.getMagneticFieldYAu(), -0.5, "magnetic field Y");
        expectNear(out.getMagneticFieldZAu(), 0.6, "magnetic field Z");
    }

    {
        xbus::DeltaV delta;
        delta.xMps = 0.01;
        delta.yMps = 0.02;
        delta.zMps = 0.03;

        capnp::MallocMessageBuilder message;
        mti610_node::fill(message.initRoot<::XbusDeltaV>(), delta);
        const auto out = message.getRoot<::XbusDeltaV>().asReader();

        expectNear(out.getDeltaVXMps(), 0.01, "delta-v X");
        expectNear(out.getDeltaVYMps(), 0.02, "delta-v Y");
        expectNear(out.getDeltaVZMps(), 0.03, "delta-v Z");
    }

    {
        xbus::Temperature temperature;
        temperature.temperatureC = 41.5;

        capnp::MallocMessageBuilder message;
        mti610_node::fill(message.initRoot<::XbusTemperature>(), temperature);
        expectNear(message.getRoot<::XbusTemperature>().asReader().getTemperatureC(), 41.5,
                   "device temperature");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
