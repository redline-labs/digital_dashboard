// SPDX-License-Identifier: GPL-3.0-or-later
#include "racegrade_tc8_messages.h"

namespace racegrade_tc8
{

void fillInputs(const dbc_motec_e888_rev1::Inputs_t& msg, RaceGradeTc8Inputs::Builder outputs)
{
    outputs.setVoltage1(msg.AV1);
    outputs.setVoltage2(msg.AV2);
    outputs.setVoltage3(msg.AV3);
    outputs.setVoltage4(msg.AV4);
    outputs.setVoltage5(msg.AV5);
    outputs.setVoltage6(msg.AV6);
    outputs.setVoltage7(msg.AV7);
    outputs.setVoltage8(msg.AV8);
    outputs.setTemperature1(msg.TC1);
    outputs.setTemperature2(msg.TC2);
    outputs.setTemperature3(msg.TC3);
    outputs.setTemperature4(msg.TC4);
    outputs.setTemperature5(msg.TC5);
    outputs.setTemperature6(msg.TC6);
    outputs.setTemperature7(msg.TC7);
    outputs.setTemperature8(msg.TC8);
    outputs.setFrequency1(msg.Freq1);
    outputs.setFrequency2(msg.Freq2);
    outputs.setFrequency3(msg.Freq3);
    outputs.setFrequency4(msg.Freq4);

}

void fillDiagnostics(const dbc_motec_e888_rev1::Diagnostics_t& msg, RaceGradeTc8Diagnostics::Builder outputs)
{
    outputs.setColdJunctionComp1(msg.Cold_Junct_Comp1);
    // Whole degrees from a 16-bit field offset by -200, so int32_t in the
    // decoder. Every value in that range is exact in the schema's Float32.
    outputs.setColdJunctionComp2(static_cast<float>(msg.Cold_Junct_Comp2));
    outputs.setE888IntTemp(static_cast<float>(msg.E888_Int_Temp));
    outputs.setDig1InState(msg.Dig_1_In_State);
    outputs.setDig2InState(msg.Dig_2_In_State);
    outputs.setDig3InState(msg.Dig_3_In_State);
    outputs.setDig4InState(msg.Dig_4_In_State);
    outputs.setDig5InState(msg.Dig_5_In_State);
    outputs.setDig6InState(msg.Dig_6_In_State);
    outputs.setBatteryVolts(msg.Battery_Volts);
    outputs.setE888StatusFlags(msg.E888_Status_Flags);
    outputs.setFirmwareVersion(msg.Firmware_Version);

}

}  // namespace racegrade_tc8
