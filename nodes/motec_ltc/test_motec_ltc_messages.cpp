// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LTC's message onto its schema.
//
// Two things here are worth a test and neither is visible in a differential
// test of the decoder. The seven fault flags are one-bit value tables declared
// one after another, so a mapping that reads the wrong one reports a heater
// short as an open circuit -- a workshop chases the wrong wire. And the sensor
// state is mapped between two enumerations by hand, which compiles whatever it
// says.
#include "motec_ltc_messages.h"

#include <capnp/message.h>

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace
{

using Ltc = dbc_motec_ltc_rev1::LTC_1_ID1_t;
using SensorState = Ltc::sig_LTC1_SensorState_t::Values;

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
    expect(std::abs(actual - expected) < 0.05, what + " (got " + std::to_string(actual) +
                                                   ", expected " + std::to_string(expected) + ")");
}

// One filled message, so each case starts from a message with nothing set.
template <typename Fn>
MotecLtcTelemetry::Reader filled(capnp::MallocMessageBuilder& message, Fn&& prepare)
{
    Ltc m;
    prepare(m);
    motec_ltc::fillTelemetry(m, message.initRoot<MotecLtcTelemetry>());
    return message.getRoot<MotecLtcTelemetry>().asReader();
}

}  // namespace

int main()
{
    {
        capnp::MallocMessageBuilder message;
        const auto out = filled(message, [](Ltc& m) {
            m.LTC1_Index = 2;
            m.LTC1_Lambda = 0.92f;
            m.LTC1_Ipn = 1.75f;
            m.LTC1_InternalTemp = 61.0f;
            // Whole percent: the signal is an 8-bit integer in the DBC.
            m.LTC1_HeaterDutyCycle = 43;
            m.LTC1_BattVolts = 13.8f;
            m.LTC1_Ip = 2.5f;
            m.LTC1_Ri = 88.0f;
        });

        expect(out.getIndex() == 2, "index");
        expectNear(out.getLambda(), 0.92, "lambda");
        expectNear(out.getIpn(), 1.75, "normalised pump current");
        expectNear(out.getInternalTempC(), 61.0, "internal temperature");
        expectNear(out.getHeaterDutyCyclePct(), 43.0, "heater duty cycle");
        expectNear(out.getBattVolts(), 13.8, "battery volts");
        expectNear(out.getIp(), 2.5, "pump current");
        expectNear(out.getRi(), 88.0, "internal resistance");

        // Ip and Ipn are adjacent, differently scaled, and both currents.
        expect(out.getIp() != out.getIpn(), "pump current and its normalised form are not the same field");
    }

    // Each fault flag on its own, with every other flag clear: the only way to
    // catch a mapping that reads its neighbour.
    struct Flag
    {
        const char* name;
        std::function<void(Ltc&)> set;
        std::function<bool(const MotecLtcTelemetry::Reader&)> get;
    };

    const std::vector<Flag> flags = {
        {"sensor control fault",
         [](Ltc& m) { m.LTC1_SensorControlFault = static_cast<decltype(m.LTC1_SensorControlFault)>(1); },
         [](const MotecLtcTelemetry::Reader& r) { return r.getSensorControlFault(); }},
        {"internal fault",
         [](Ltc& m) { m.LTC1_InternalFault = static_cast<decltype(m.LTC1_InternalFault)>(1); },
         [](const MotecLtcTelemetry::Reader& r) { return r.getInternalFault(); }},
        {"sensor wire short",
         [](Ltc& m) { m.LTC1_SensorWireShort = static_cast<decltype(m.LTC1_SensorWireShort)>(1); },
         [](const MotecLtcTelemetry::Reader& r) { return r.getSensorWireShort(); }},
        {"heater failed to heat",
         [](Ltc& m) { m.LTC1_HeaterFailedtoHeat = static_cast<decltype(m.LTC1_HeaterFailedtoHeat)>(1); },
         [](const MotecLtcTelemetry::Reader& r) { return r.getHeaterFailedToHeat(); }},
        {"heater open circuit",
         [](Ltc& m) { m.LTC1_HeaterOpenCircuit = static_cast<decltype(m.LTC1_HeaterOpenCircuit)>(1); },
         [](const MotecLtcTelemetry::Reader& r) { return r.getHeaterOpenCircuit(); }},
        {"heater short to battery",
         [](Ltc& m) { m.LTC1_HeaterShorttoVBATT = static_cast<decltype(m.LTC1_HeaterShorttoVBATT)>(1); },
         [](const MotecLtcTelemetry::Reader& r) { return r.getHeaterShortToVbatt(); }},
        {"heater short to ground",
         [](Ltc& m) { m.LTC1_HeaterShorttoGND = static_cast<decltype(m.LTC1_HeaterShorttoGND)>(1); },
         [](const MotecLtcTelemetry::Reader& r) { return r.getHeaterShortToGnd(); }},
    };

    for (const Flag& flag : flags)
    {
        capnp::MallocMessageBuilder message;
        const auto out = filled(message, flag.set);

        expect(flag.get(out), std::string(flag.name) + " reaches its own field");

        int others = 0;
        for (const Flag& other : flags)
        {
            if (&other != &flag && other.get(out))
            {
                ++others;
            }
        }
        expect(others == 0, std::string(flag.name) + " sets no other flag");
    }

    // Every state the sensor can report, mapped to its own wire value.
    const std::vector<std::pair<SensorState, LtcSensorState>> states = {
        {SensorState::START, LtcSensorState::START},
        {SensorState::DIAGNOSTICS, LtcSensorState::DIAGNOSTICS},
        {SensorState::PRE_CAL, LtcSensorState::PRE_CAL},
        {SensorState::CALIBRATION, LtcSensorState::CALIBRATION},
        {SensorState::POST_CAL, LtcSensorState::POST_CAL},
        {SensorState::PAUSED, LtcSensorState::PAUSED},
        {SensorState::HEATING, LtcSensorState::HEATING},
        {SensorState::RUNNING, LtcSensorState::RUNNING},
        {SensorState::COOLING, LtcSensorState::COOLING},
        {SensorState::PUMP_START, LtcSensorState::PUMP_START},
        {SensorState::PUMP_OFF, LtcSensorState::PUMP_OFF},
    };

    for (const auto& [decoded, wire] : states)
    {
        capnp::MallocMessageBuilder message;
        const auto out = filled(message, [state = decoded](Ltc& m) { m.LTC1_SensorState = state; });
        expect(out.getSensorState() == wire,
               "sensor state " + std::to_string(static_cast<int>(decoded)) + " maps to its own wire value");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
