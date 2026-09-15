// SPDX-License-Identifier: GPL-3.0-or-later
//
// The E888's inputs and diagnostics onto their schemas, field by field.
//
// Every analogue voltage is a voltage and every thermocouple is a temperature,
// so the numbers themselves say nothing about which channel they came from: a
// mapping that reads TC3 into temperature4 draws a perfectly plausible dash.
// Each source field gets a value nothing else has, which is what makes a
// transposition visible here.
#include "racegrade_tc8_messages.h"

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
    expect(std::abs(actual - expected) < 0.05, what + " (got " + std::to_string(actual) +
                                                   ", expected " + std::to_string(expected) + ")");
}

}  // namespace

int main()
{
    dbc_motec_e888_rev1::Inputs_t inputs;

    // 1.1, 2.2 ... one per channel, so channel n reads n.n wherever it lands.
    inputs.AV1 = 1.1f;
    inputs.AV2 = 2.2f;
    inputs.AV3 = 3.3f;
    inputs.AV4 = 4.4f;
    inputs.AV5 = 5.5f;
    inputs.AV6 = 6.6f;
    inputs.AV7 = 7.7f;
    inputs.AV8 = 8.8f;

    inputs.TC1 = 101.0f;
    inputs.TC2 = 202.0f;
    inputs.TC3 = 303.0f;
    inputs.TC4 = 404.0f;
    inputs.TC5 = 505.0f;
    inputs.TC6 = 606.0f;
    inputs.TC7 = 707.0f;
    inputs.TC8 = 808.0f;

    inputs.Freq1 = 11.0f;
    inputs.Freq2 = 22.0f;
    inputs.Freq3 = 33.0f;
    inputs.Freq4 = 44.0f;

    capnp::MallocMessageBuilder message;
    racegrade_tc8::fillInputs(inputs, message.initRoot<RaceGradeTc8Inputs>());
    const RaceGradeTc8Inputs::Reader out = message.getRoot<RaceGradeTc8Inputs>().asReader();

    expectNear(out.getVoltage1(), 1.1, "voltage 1");
    expectNear(out.getVoltage2(), 2.2, "voltage 2");
    expectNear(out.getVoltage3(), 3.3, "voltage 3");
    expectNear(out.getVoltage4(), 4.4, "voltage 4");
    expectNear(out.getVoltage5(), 5.5, "voltage 5");
    expectNear(out.getVoltage6(), 6.6, "voltage 6");
    expectNear(out.getVoltage7(), 7.7, "voltage 7");
    expectNear(out.getVoltage8(), 8.8, "voltage 8");

    expectNear(out.getTemperature1(), 101.0, "thermocouple 1");
    expectNear(out.getTemperature2(), 202.0, "thermocouple 2");
    expectNear(out.getTemperature3(), 303.0, "thermocouple 3");
    expectNear(out.getTemperature4(), 404.0, "thermocouple 4");
    expectNear(out.getTemperature5(), 505.0, "thermocouple 5");
    expectNear(out.getTemperature6(), 606.0, "thermocouple 6");
    expectNear(out.getTemperature7(), 707.0, "thermocouple 7");
    expectNear(out.getTemperature8(), 808.0, "thermocouple 8");

    expectNear(out.getFrequency1(), 11.0, "frequency 1");
    expectNear(out.getFrequency2(), 22.0, "frequency 2");
    expectNear(out.getFrequency3(), 33.0, "frequency 3");
    expectNear(out.getFrequency4(), 44.0, "frequency 4");

    // Diagnostics. The two cold-junction channels are the pair a swap would be
    // hardest to notice on a dash, so they get different values.
    dbc_motec_e888_rev1::Diagnostics_t diagnostics;
    diagnostics.Cold_Junct_Comp1 = 21.0f;
    diagnostics.Cold_Junct_Comp2 = 43;
    diagnostics.E888_Int_Temp = 57;
    diagnostics.Battery_Volts = 13.4f;

    capnp::MallocMessageBuilder diagnostics_message;
    racegrade_tc8::fillDiagnostics(diagnostics,
                                   diagnostics_message.initRoot<RaceGradeTc8Diagnostics>());
    const RaceGradeTc8Diagnostics::Reader diag =
        diagnostics_message.getRoot<RaceGradeTc8Diagnostics>().asReader();

    expectNear(diag.getColdJunctionComp1(), 21.0, "cold junction 1");
    expectNear(diag.getColdJunctionComp2(), 43.0, "cold junction 2");
    expectNear(diag.getE888IntTemp(), 57.0, "internal temperature");
    expectNear(diag.getBatteryVolts(), 13.4, "battery volts");

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
