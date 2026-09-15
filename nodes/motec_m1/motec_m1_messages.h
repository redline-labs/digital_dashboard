// SPDX-License-Identifier: GPL-3.0-or-later
//
// The decoded M1 messages onto their schemas.
//
// Out of main.cpp because there are about two hundred fields here and a
// differential test cannot see any of them: the DBC decoder is tested against
// cantools, the schema is tested by its own round trip, and the wiring between
// the two -- which signal becomes which field -- was checked by nobody. Every
// way of getting it wrong produces a number a gauge will happily draw.
//
// No zenoh here, only the generated decoder and the generated schema, so
// motec_m1_test_messages runs without a bus.
#ifndef MOTEC_M1_MESSAGES_H_
#define MOTEC_M1_MESSAGES_H_

#include "dbc_motec_m1_rev3_parser.h"
#include "motec_m1.capnp.h"

namespace motec_m1
{

using namespace dbc_motec_m1_rev3;

void fillEngineAir(const M1_GEN_0x640_t& m, MotecM1EngineAir::Builder out);
void fillFuelStatus(const M1_GEN_0x641_t& m, MotecM1FuelStatus::Builder out);
void fillTemperatures(const M1_GEN_0x649_t& m, MotecM1Temperatures::Builder out);
void fillExhaust(const M1_GEN_0x651_t& m, MotecM1Exhaust::Builder out);
void fillThrottleTiming(const M1_GEN_0x642_t& m, MotecM1ThrottleTiming::Builder out);
void fillCutsAndOilPressure(const M1_GEN_0x644_t& m, MotecM1CutsAndOilPressure::Builder out);
void fillBoostStatus(const M1_GEN_0x645_t& m, MotecM1BoostStatus::Builder out);
void fillInletCam(const M1_GEN_0x646_t& m, MotecM1InletCam::Builder out);
void fillExhaustCam(const M1_GEN_0x647_t& m, MotecM1ExhaustCam::Builder out);
void fillWheelSpeeds(const M1_GEN_0x648_t& m, MotecM1WheelSpeeds::Builder out);
void fillEnvironment(const M1_GEN_0x64A_t& m, MotecM1Environment::Builder out);
void fillRuntimeWarnings(const M1_GEN_0x64C_t& m, MotecM1RuntimeWarnings::Builder out);
void fillStates(const M1_GEN_0x64D_t& m, MotecM1States::Builder out);
void fillDiagnostics(const M1_GEN_0x64E_t& m, MotecM1Diagnostics::Builder out);
void fillTotals(const M1_GEN_0x64F_t& m, MotecM1Totals::Builder out);
void fillDriverControls(const M1_GEN_0x650_t& m, MotecM1DriverControls::Builder out);
void fillFuelSecondary(const M1_GEN_0x652_t& m, MotecM1FuelSecondary::Builder out);
void fillPressures(const M1_GEN_0x655_t& m, MotecM1Pressures::Builder out);
void fillFlows(const M1_GEN_0x656_t& m, MotecM1Flows::Builder out);
void fillInjectorPressures(const M1_GEN_0x657_t& m, MotecM1InjectorPressures::Builder out);
void fillVehicleDynamics(const M1_GEN_0x658_t& m, MotecM1VehicleDynamics::Builder out);
void fillFuelDirectAll(const M1_GEN_0x653_t& b1, const M1_GEN_0x654_t& b2, MotecM1FuelDirectAll::Builder out);
void fillTurboBoth(const M1_GEN_0x6A6_t& b1, const M1_GEN_0x6A7_t& b2, MotecM1Turbo::Builder out);
void fillKnock1to12(const M1_GEN_0x643_t& k1, const M1_GEN_0x659_t& k2, MotecM1KnockLevels1to12::Builder out);
void fillIgnTrim1to12(const M1_GEN_0x64B_t& t1, const M1_GEN_0x65A_t& t2, MotecM1IgnitionTrim1to12::Builder out);
void fillLapTiming(const M1_GEN_0x65B_t& m, MotecM1LapTiming::Builder out);
void fillDiffAndRotary(const M1_GEN_0x65C_t& m, MotecM1DiffAndRotary::Builder out);
void fillBrakeTemperatures(const M1_GEN_0x65D_t& m, MotecM1BrakeTemperatures::Builder out);
void fillExhaustPressures(const M1_GEN_0x65E_t& m, MotecM1ExhaustPressures::Builder out);
void fillThresholdsAndLimits(const M1_GEN_0x65F_t& m, MotecM1ThresholdsAndLimits::Builder out);
void fillAuxOutputs(const M1_GEN_0x6A0_t& m, MotecM1AuxOutputs::Builder out);
void fillAuxOutput5(const M1_GEN_0x6A1_t& m, MotecM1AuxOutput5::Builder out);

}  // namespace motec_m1

#endif  // MOTEC_M1_MESSAGES_H_
