// SPDX-License-Identifier: GPL-3.0-or-later
#include "motec_m1_messages.h"

namespace motec_m1
{

using namespace dbc_motec_m1_rev3;

void fillEngineAir(const M1_GEN_0x640_t& m, MotecM1EngineAir::Builder out)
{
    out.setEngineSpeedRpm(m.Engine_Speed);
    out.setMapKpa(m.Inlet_Manifold_Pressure);
    out.setInletManifoldTempC(m.Inlet_Manifold_Temperature);
    out.setThrottlePositionPct(m.Throttle_Position);
}

void fillFuelStatus(const M1_GEN_0x641_t& m, MotecM1FuelStatus::Builder out)
{
    out.setFuelVolumeUl(m.Fuel_Volume);
    out.setFuelMixtureAimLambda(m.Fuel_Mixture_Aim);
    out.setFuelPressureKpa(m.Fuel_Pressure_Sensor);
    out.setInjectorDutyPct(m.Fuel_Injector_Duty_Cycle);
    out.setEngineEfficiencyPct(m.Engine_Efficiency);
}

void fillTemperatures(const M1_GEN_0x649_t& m, MotecM1Temperatures::Builder out)
{
    out.setCoolantTempC(m.Coolant_Temperature);
    out.setEngineOilTempC(m.Engine_Oil_Temperature);
    out.setFuelTempC(m.Fuel_Temperature);
    out.setAmbientTempC(m.Ambient_Temperature);
    out.setAirboxTempC(m.Airbox_Temperature);
    out.setEcuBatteryVolts(m.ECU_Battery_Voltage);
    out.setFuelUsedL(m.Fuel_Used);
}

void fillExhaust(const M1_GEN_0x651_t& m, MotecM1Exhaust::Builder out)
{
    out.setExhaustLambda(m.Exhaust_Lambda);
    out.setExhaustLambdaBank1(m.Exhaust_Lambda_Bank_1);
    out.setExhaustLambdaBank2(m.Exhaust_Lambda_Bank_2);
    out.setExhaustTempBank1C(m.Exhaust_Temp_Bank_1);
    out.setExhaustTempBank2C(m.Exhaust_Temp_Bank_2);
}

void fillThrottleTiming(const M1_GEN_0x642_t& m, MotecM1ThrottleTiming::Builder out)
{
    out.setThrottlePedalPct(m.Throttle_Pedal);
    out.setEngineLoadMg(m.Engine_Load);
    out.setIgnitionTimingDeg(m.Ignition_Timing);
    out.setFuelTimingDeg(m.Fuel_Timing);
}

void fillCutsAndOilPressure(const M1_GEN_0x644_t& m, MotecM1CutsAndOilPressure::Builder out)
{
    out.setIgnitionCutCount(m.Ignition_Output_Cut_Count);
    out.setFuelCutCount(m.Fuel_Output_Cut_Count);
    out.setIgnitionCutAvgPct(m.Ignition_Output_Cut_Average);
    out.setFuelCutAvgPct(m.Fuel_Output_Cut_Average);
    out.setFuelCyl1PulseWidthMs(m.Fuel_Cyl_1_Output_Pulse_Width);
    out.setIgnitionCutRequestState(m.Ignition_Cut_Request_State);
    out.setIgnitionTimingState(m.Ignition_Timing_State);
    out.setEngineOilPressureKpa(m.Engine_Oil_Pressure);
}

void fillBoostStatus(const M1_GEN_0x645_t& m, MotecM1BoostStatus::Builder out)
{
    out.setBoostPressureKpa(m.Boost_Pressure);
    out.setBoostAimKpa(m.Boost_Aim);
    out.setActuatorDutyPct(m.Boost_Actuator_Output_Duty_Cycle);
    out.setGearLeverForceN(m.Gear_Lever_Force);
}

void fillInletCam(const M1_GEN_0x646_t& m, MotecM1InletCam::Builder out)
{
    out.setAimDeg(m.Inlet_Camshaft_Aim);
    out.setBank1PositionDeg(m.Inlet_Camshaft_Bank_1_Position);
    out.setBank2PositionDeg(m.Inlet_Camshaft_Bank_2_Position);
    out.setBank1DutyPct(m.Inlet_Cam_Bk_1_Output_Duty_Cycle);
    out.setBank2DutyPct(m.Inlet_Cam_Bk_2_Output_Duty_Cycle);
}

void fillExhaustCam(const M1_GEN_0x647_t& m, MotecM1ExhaustCam::Builder out)
{
    out.setAimDeg(m.Exhaust_Camshaft_Aim);
    out.setBank1PositionDeg(m.Exhaust_Camshaft_Bank_1_Position);
    out.setBank2PositionDeg(m.Exhaust_Camshaft_Bank_2_Position);
    out.setBank1DutyPct(m.Exh_Cam_Bk_1_Output_Duty_Cycle);
    out.setBank2DutyPct(m.Exh_Cam_Bk_2_Output_Duty_Cycle);
}

void fillWheelSpeeds(const M1_GEN_0x648_t& m, MotecM1WheelSpeeds::Builder out)
{
    out.setFrontLeftKph(m.Wheel_Speed_Front_Left);
    out.setFrontRightKph(m.Wheel_Speed_Front_Right);
    out.setRearLeftKph(m.Wheel_Speed_Rear_Left);
    out.setRearRightKph(m.Wheel_Speed_Rear_Right);
}

void fillEnvironment(const M1_GEN_0x64A_t& m, MotecM1Environment::Builder out)
{
    out.setExhaustTempC(m.Exhaust_Temperature);
    out.setEngineLoadAvgPct(m.Engine_Load_Average);
    out.setEngineSpeedLimitIgnRpm(m.Engine_Speed_Limit_Ignition);
    out.setAmbientPressureKpa(m.Ambient_Pressure);
}

void fillRuntimeWarnings(const M1_GEN_0x64C_t& m, MotecM1RuntimeWarnings::Builder out)
{
    out.setEngineRunTimeS(m.Engine_Run_Time);
    out.setEcuUpTimeS(m.ECU_Up_Time);
    out.setWarningSource(m.Warning_Source);
    out.setFuelPressureWarn(m.Fuel_Pressure_Warning);
    out.setCrankcasePressureWarn(m.Crankcase_Pressure_Warning);
    out.setEngineOilPressureWarn(m.Engine_Oil_Pressure_Warning);
    out.setEngineOilTempWarn(m.Engine_Oil_Temperature_Warning);
    out.setEngineSpeedWarn(m.Engine_Speed_Warning);
    out.setCoolantPressureWarn(m.Coolant_Pressure_Warning);
    out.setCoolantTempWarn(m.Coolant_Temperature_Warning);
    out.setKnockWarn(m.Knock_Warning);
}

void fillStates(const M1_GEN_0x64D_t& m, MotecM1States::Builder out)
{
    out.setFuelPumpState(m.Fuel_Pump_State);
    out.setEngineState(m.Engine_State);
    out.setLaunchState(m.Launch_State);
    out.setAntiLagState(m.Anti_Lag_State);
    out.setEngineSpeedLimitState(m.Engine_Speed_Limit_State);
    out.setBoostAimState(m.Boost_Aim_State);
    out.setFuelCutState(m.Fuel_Cut_State);
    out.setEngineOverrunState(m.Engine_Overrun_State);
    out.setKnockState(m.Knock_State);
    out.setFuelPurgeState(m.Fuel_Purge_State);
    out.setFuelClosedLoopState(m.Fuel_Closed_Loop_State);
    out.setThrottleAimState(m.Throttle_Aim_State);
    out.setGear(m.Gear);
    out.setEngineSpeedRefState(m.Engine_Speed_Reference_State);
    out.setEngineSpeedLimitState2(m.Engine_Speed_Limit_State);
}

void fillDiagnostics(const M1_GEN_0x64E_t& m, MotecM1Diagnostics::Builder out)
{
    out.setLaunchDiagnostic(m.Launch_Diagnostic);
    out.setAntiLagDiagnostic(m.Anti_Lag_Diagnostic);
    out.setFuelCutState(m.Fuel_Cut_State);
    out.setBoostControlDiagnostic(m.Boost_Control_Diagnostic);
    out.setFuelClosedLoopDiagnostic(m.Fuel_Closed_Loop_Diagnostic);
    out.setNeutralSwitch(m.Neutral_Switch);
    out.setEngineRunSwitch(m.Engine_Run_Switch);
    out.setAntiLagSwitch(m.Anti_Lag_Switch);
    out.setBrakeState(m.Brake_State);
    out.setTractionEnableSwitch(m.Traction_Enable_Switch);
    out.setLaunchEnableSwitch(m.Launch_Enable_Switch);
    out.setPitSwitch(m.Pit_Switch);
    out.setEngineOilPressureLowSwitch(m.Engine_Oil_Pressure_Low_Switch);
    out.setBoostLimitDisableSwitch(m.Boost_Limit_Disable_Switch);
    out.setThrottlePedalTransSwitch(m.Throttle_Pedal_Trans_Switch);
    out.setRaceTimeResetSwitch(m.Race_Time_Reset_Switch);
}

void fillTotals(const M1_GEN_0x64F_t& m, MotecM1Totals::Builder out)
{
    out.setEngineRunHoursTotal(m.Engine_Run_Hours_Total);
    out.setFuelClosedLoopTrimBk1(m.Fuel_Closed_Loop_Ctrl_Bk_1_Trim);
    out.setFuelClosedLoopTrimBk2(m.Fuel_Closed_Loop_Ctrl_Bk_2_Trim);
    out.setGearboxTempC(m.Gearbox_Temperature);
    out.setFuelTankLevelL(m.Fuel_Tank_Level);
}

void fillDriverControls(const M1_GEN_0x650_t& m, MotecM1DriverControls::Builder out)
{
    out.setRotary1(m.Driver_Rotary_Switch_1);
    out.setRotary2(m.Driver_Rotary_Switch_2);
    out.setRotary3(m.Driver_Rotary_Switch_3);
    out.setRotary4(m.Driver_Rotary_Switch_4);
    out.setRotary5(m.Driver_Rotary_Switch_5);
    out.setRotary6(m.Driver_Rotary_Switch_6);
    out.setSwitch8(m.Driver_Switch_8);
    out.setSwitch7(m.Driver_Switch_7);
    out.setSwitch6(m.Driver_Switch_6);
    out.setSwitch5(m.Driver_Switch_5);
    out.setSwitch4(m.Driver_Switch_4);
    out.setSwitch3(m.Driver_Switch_3);
    out.setSwitch2(m.Driver_Switch_2);
    out.setSwitch1(m.Driver_Switch_1);
}

void fillFuelSecondary(const M1_GEN_0x652_t& m, MotecM1FuelSecondary::Builder out)
{
    out.setInjectorSecondaryContributionPct(m.Fuel_Injector_Sec_Contribution);
    out.setFuelTimingSecondaryDeg(m.Fuel_Timing_Secondary);
    out.setInjectorDutySecPct(m.Fuel_Injector_Duty_Cycle_Secdry);
}

void fillPressures(const M1_GEN_0x655_t& m, MotecM1Pressures::Builder out)
{
    out.setBrakePressureFrontBar(m.Brake_Pressure_Front);
    out.setBrakePressureRearBar(m.Brake_Pressure_Rear);
    out.setCoolantPressureKpa(m.Coolant_Pressure);
    out.setPowerSteerPressureKpa(m.Power_Steer_Pressure);
}

void fillFlows(const M1_GEN_0x656_t& m, MotecM1Flows::Builder out)
{
    out.setSteeringAngleDeg(m.Steering_Angle);
    out.setInletMassFlowGs(m.Inlet_Mass_Flow);
    out.setAirboxMassFlowGs(m.Airbox_Mass_Flow);
    out.setFuelFlowMlPerS(m.Fuel_Flow);
}

void fillInjectorPressures(const M1_GEN_0x657_t& m, MotecM1InjectorPressures::Builder out)
{
    out.setFuelInjectorPrimaryPressureKpa(m.Fuel_Injector_Primary_Pressure);
    out.setFuelInjectorSecondaryPressureKpa(m.Fuel_Injector_Secondary_Pressure);
    out.setGearInputShaftRpm(m.Gear_Input_Shaft_Speed);
    out.setGearOutputShaftRpm(m.Gear_Output_Shaft_Speed);
}

void fillVehicleDynamics(const M1_GEN_0x658_t& m, MotecM1VehicleDynamics::Builder out)
{
    out.setAccelLateralG(m.Vehicle_Accel_Lateral);
    out.setAccelLongitudinalG(m.Vehicle_Accel_Longitudinal);
    out.setAccelVerticalG(m.Vehicle_Accel_Vertical);
    out.setYawRateDegPerS(m.Vehicle_Yaw_Rate);
}

void fillFuelDirectAll(const M1_GEN_0x653_t& b1, const M1_GEN_0x654_t& b2, MotecM1FuelDirectAll::Builder out)
{
    out.setFuelPressureDirectKpa(b1.Fuel_Pressure_Direct);
    out.setFuelPressureDirectAimKpa(b1.Fuel_Pressure_Direct_Aim);
    out.setFuelPressureDirectControlPct(b1.Fuel_Pressure_Direct_Control);
    out.setFuelPressureDirectFeedFwdPct(b1.Fuel_Pressure_Direct_Feed_Fwd);
    out.setFuelPressureDirectPropPct(b1.Fuel_Pressure_Direct_Prop);
    out.setFuelPressureDirectIntegralPct(b1.Fuel_Pressure_Direct_Integral);
    out.setFuelPressureDirectB2Kpa(b2.Fuel_Pressure_Direct_B2);
    out.setFuelPressureDirectB2AimKpa(b2.Fuel_Pressure_Direct_B2_Aim);
    out.setFuelPressureDirectB2ControlPct(b2.Fuel_Pressure_Direct_B2_Control);
    out.setFuelPressureDirectB2FeedFwdPct(b2.Fuel_Pressure_Direct_B2_Feed_Fwd);
    out.setFuelPressureDirectB2PropPct(b2.Fuel_Pressure_Direct_B2_Prop);
    out.setFuelPressureDirectB2IntegralPct(b2.Fuel_Pressure_Direct_B2_Integral);
}

void fillTurboBoth(const M1_GEN_0x6A6_t& b1, const M1_GEN_0x6A7_t& b2, MotecM1Turbo::Builder out)
{
    out.setBank1SpeedHz(b1.Turbo_Bank_1_Speed);
    out.setBank1InletTempC(b1.Turbo_Bank_1_Inlet_Temp);
    out.setBank1OutletTempC(b1.Turbo_Bank_1_Temp_Outlet);
    out.setBank1InletPressureKpa(b1.Turbo_Bank_1_Pressure_Inlet);
    out.setBank2SpeedHz(b2.Turbo_Bank_2_Speed);
    out.setBank2InletTempC(b2.Turbo_Bank_2_Inlet_Temp);
    out.setBank2OutletTempC(b2.Turbo_Bank_2_Temp_Outlet);
}

void fillKnock1to12(const M1_GEN_0x643_t& k1, const M1_GEN_0x659_t& k2, MotecM1KnockLevels1to12::Builder out)
{
    out.setCyl1(k1.Engine_Cylinder_1_Knock_Level);
    out.setCyl2(k1.Engine_Cylinder_2_Knock_Level);
    out.setCyl3(k1.Engine_Cylinder_3_Knock_Level);
    out.setCyl4(k1.Engine_Cylinder_4_Knock_Level);
    out.setCyl5(k1.Engine_Cylinder_5_Knock_Level);
    out.setCyl6(k1.Engine_Cylinder_6_Knock_Level);
    out.setCyl7(k1.Engine_Cylinder_7_Knock_Level);
    out.setCyl8(k1.Engine_Cylinder_8_Knock_Level);
    out.setCyl9(k2.Engine_Cylinder_9_Knock_Level);
    out.setCyl10(k2.Engine_Cylinder_10_Knock_Level);
    out.setCyl11(k2.Engine_Cylinder_11_Knock_Level);
    out.setCyl12(k2.Engine_Cylinder_12_Knock_Level);
}

void fillIgnTrim1to12(const M1_GEN_0x64B_t& t1, const M1_GEN_0x65A_t& t2, MotecM1IgnitionTrim1to12::Builder out)
{
    out.setCyl1Deg(t1.Ignition_Cyl_1_Trim_Knock);
    out.setCyl2Deg(t1.Ignition_Cyl_2_Trim_Knock);
    out.setCyl3Deg(t1.Ignition_Cyl_3_Trim_Knock);
    out.setCyl4Deg(t1.Ignition_Cyl_4_Trim_Knock);
    out.setCyl5Deg(t1.Ignition_Cyl_5_Trim_Knock);
    out.setCyl6Deg(t1.Ignition_Cyl_6_Trim_Knock);
    out.setCyl7Deg(t1.Ignition_Cyl_7_Trim_Knock);
    out.setCyl8Deg(t1.Ignition_Cyl_8_Trim_Knock);
    out.setCyl9Deg(t2.Ignition_Cyl_9_Trim_Knock);
    out.setCyl10Deg(t2.Ignition_Cyl_10_Trim_Knock);
    out.setCyl11Deg(t2.Ignition_Cyl_11_Trim_Knock);
    out.setCyl12Deg(t2.Ignition_Cyl_12_Trim_Knock);
}

void fillLapTiming(const M1_GEN_0x65B_t& m, MotecM1LapTiming::Builder out)
{
    out.setLapTimeS(m.Lap_Time);
    out.setLapTimeRunningS(m.Lap_Time_Running);
    out.setLapNumber(m.Lap_Number);
    out.setLapDistanceM(m.Lap_Distance);
}

void fillDiffAndRotary(const M1_GEN_0x65C_t& m, MotecM1DiffAndRotary::Builder out)
{
    out.setDifferentialTempFrontC(m.Differential_Temperature_Front);
    out.setRotary7(m.Driver_Rotary_Switch_7);
    out.setRotary8(m.Driver_Rotary_Switch_8);
}

void fillBrakeTemperatures(const M1_GEN_0x65D_t& m, MotecM1BrakeTemperatures::Builder out)
{
    out.setFrontLeftC(m.Brake_Temperature_Front_Left);
    out.setFrontRightC(m.Brake_Temperature_Front_Right);
    out.setRearLeftC(m.Brake_Temperature_Rear_Left);
    out.setRearRightC(m.Brake_Temperature_Rear_Right);
}

void fillExhaustPressures(const M1_GEN_0x65E_t& m, MotecM1ExhaustPressures::Builder out)
{
    out.setExhaustPressureB1Kpa(m.Exhaust_Pressure_Bank_1);
    out.setExhaustPressureB2Kpa(m.Exhaust_Pressure_Bank_2);
    out.setEngineCrankCasePressureKpa(m.Engine_Crank_Case_Pressure);
    out.setAlternatorCurrentA(m.Alternator_Current);
}

void fillThresholdsAndLimits(const M1_GEN_0x65F_t& m, MotecM1ThresholdsAndLimits::Builder out)
{
    out.setKnockThresholdPct(m.Knock_Threshold);
    out.setLoggingSystem1UsedPct(m.Logging_System_1_Used);
    out.setVehiclePitSpeedLimitKph(m.Vehicle_Pit_Speed_Limit);
}

void fillAuxOutputs(const M1_GEN_0x6A0_t& m, MotecM1AuxOutputs::Builder out)
{
    out.setAuxOut1DutyPct(m.Aux_Output_1_Duty_Cycle);
    out.setAuxOut2DutyPct(m.Aux_Output_2_Duty_Cycle);
    out.setAuxOut3DutyPct(m.Aux_Output_3_Duty_Cycle);
    out.setAuxOut4DutyPct(m.Aux_Output_4_Duty_Cycle);
}

void fillAuxOutput5(const M1_GEN_0x6A1_t& m, MotecM1AuxOutput5::Builder out)
{
    out.setAuxOut5DutyPct(m.Aux_Output_5_Duty_Cycle);
}

}  // namespace motec_m1
