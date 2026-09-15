#include "dbc_motec_m1_rev3_parser.h"

#include "pub_sub/node_identity.h"
#include "pub_sub/zenoh_publisher.h"
#include "pub_sub/zenoh_subscriber.h"
#include "can_frame.capnp.h"
#include "motec_m1.capnp.h"

#include <span>
#include "core/core.h"
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

#include <array>
#include <thread>
#include <chrono>

using namespace dbc_motec_m1_rev3;

static void publishEngineAir(const M1_GEN_0x640_t& m, pub_sub::ZenohPublisher<MotecM1EngineAir>& pub)
{
    auto& out = pub.fields();
    out.setEngineSpeedRpm(m.Engine_Speed);
    out.setMapKpa(m.Inlet_Manifold_Pressure);
    out.setInletManifoldTempC(m.Inlet_Manifold_Temperature);
    out.setThrottlePositionPct(m.Throttle_Position);
    pub.put();
}

static void publishFuelStatus(const M1_GEN_0x641_t& m, pub_sub::ZenohPublisher<MotecM1FuelStatus>& pub)
{
    auto& out = pub.fields();
    out.setFuelVolumeUl(m.Fuel_Volume);
    out.setFuelMixtureAimLambda(m.Fuel_Mixture_Aim);
    out.setFuelPressureKpa(m.Fuel_Pressure_Sensor);
    out.setInjectorDutyPct(m.Fuel_Injector_Duty_Cycle);
    out.setEngineEfficiencyPct(m.Engine_Efficiency);
    pub.put();
}

static void publishTemperatures(const M1_GEN_0x649_t& m, pub_sub::ZenohPublisher<MotecM1Temperatures>& pub)
{
    auto& out = pub.fields();
    out.setCoolantTempC(m.Coolant_Temperature);
    out.setEngineOilTempC(m.Engine_Oil_Temperature);
    out.setFuelTempC(m.Fuel_Temperature);
    out.setAmbientTempC(m.Ambient_Temperature);
    out.setAirboxTempC(m.Airbox_Temperature);
    out.setEcuBatteryVolts(m.ECU_Battery_Voltage);
    out.setFuelUsedL(m.Fuel_Used);
    pub.put();
}

static void publishExhaust(const M1_GEN_0x651_t& m, pub_sub::ZenohPublisher<MotecM1Exhaust>& pub)
{
    auto& out = pub.fields();
    out.setExhaustLambda(m.Exhaust_Lambda);
    out.setExhaustLambdaBank1(m.Exhaust_Lambda_Bank_1);
    out.setExhaustLambdaBank2(m.Exhaust_Lambda_Bank_2);
    out.setExhaustTempBank1C(m.Exhaust_Temp_Bank_1);
    out.setExhaustTempBank2C(m.Exhaust_Temp_Bank_2);
    pub.put();
}

static void publishThrottleTiming(const M1_GEN_0x642_t& m, pub_sub::ZenohPublisher<MotecM1ThrottleTiming>& pub)
{
    auto& out = pub.fields();
    out.setThrottlePedalPct(m.Throttle_Pedal);
    out.setEngineLoadMg(m.Engine_Load);
    out.setIgnitionTimingDeg(m.Ignition_Timing);
    out.setFuelTimingDeg(m.Fuel_Timing);
    pub.put();
}

static void publishCutsAndOilPressure(const M1_GEN_0x644_t& m, pub_sub::ZenohPublisher<MotecM1CutsAndOilPressure>& pub)
{
    auto& out = pub.fields();
    out.setIgnitionCutCount(m.Ignition_Output_Cut_Count);
    out.setFuelCutCount(m.Fuel_Output_Cut_Count);
    out.setIgnitionCutAvgPct(m.Ignition_Output_Cut_Average);
    out.setFuelCutAvgPct(m.Fuel_Output_Cut_Average);
    out.setFuelCyl1PulseWidthMs(m.Fuel_Cyl_1_Output_Pulse_Width);
    out.setIgnitionCutRequestState(m.Ignition_Cut_Request_State);
    out.setIgnitionTimingState(m.Ignition_Timing_State);
    out.setEngineOilPressureKpa(m.Engine_Oil_Pressure);
    pub.put();
}

static void publishBoostStatus(const M1_GEN_0x645_t& m, pub_sub::ZenohPublisher<MotecM1BoostStatus>& pub)
{
    auto& out = pub.fields();
    out.setBoostPressureKpa(m.Boost_Pressure);
    out.setBoostAimKpa(m.Boost_Aim);
    out.setActuatorDutyPct(m.Boost_Actuator_Output_Duty_Cycle);
    out.setGearLeverForceN(m.Gear_Lever_Force);
    pub.put();
}

static void publishInletCam(const M1_GEN_0x646_t& m, pub_sub::ZenohPublisher<MotecM1InletCam>& pub)
{
    auto& out = pub.fields();
    out.setAimDeg(m.Inlet_Camshaft_Aim);
    out.setBank1PositionDeg(m.Inlet_Camshaft_Bank_1_Position);
    out.setBank2PositionDeg(m.Inlet_Camshaft_Bank_2_Position);
    out.setBank1DutyPct(m.Inlet_Cam_Bk_1_Output_Duty_Cycle);
    out.setBank2DutyPct(m.Inlet_Cam_Bk_2_Output_Duty_Cycle);
    pub.put();
}

static void publishExhaustCam(const M1_GEN_0x647_t& m, pub_sub::ZenohPublisher<MotecM1ExhaustCam>& pub)
{
    auto& out = pub.fields();
    out.setAimDeg(m.Exhaust_Camshaft_Aim);
    out.setBank1PositionDeg(m.Exhaust_Camshaft_Bank_1_Position);
    out.setBank2PositionDeg(m.Exhaust_Camshaft_Bank_2_Position);
    out.setBank1DutyPct(m.Exh_Cam_Bk_1_Output_Duty_Cycle);
    out.setBank2DutyPct(m.Exh_Cam_Bk_2_Output_Duty_Cycle);
    pub.put();
}

static void publishWheelSpeeds(const M1_GEN_0x648_t& m, pub_sub::ZenohPublisher<MotecM1WheelSpeeds>& pub)
{
    auto& out = pub.fields();
    out.setFrontLeftKph(m.Wheel_Speed_Front_Left);
    out.setFrontRightKph(m.Wheel_Speed_Front_Right);
    out.setRearLeftKph(m.Wheel_Speed_Rear_Left);
    out.setRearRightKph(m.Wheel_Speed_Rear_Right);
    pub.put();
}

static void publishEnvironment(const M1_GEN_0x64A_t& m, pub_sub::ZenohPublisher<MotecM1Environment>& pub)
{
    auto& out = pub.fields();
    out.setExhaustTempC(m.Exhaust_Temperature);
    out.setEngineLoadAvgPct(m.Engine_Load_Average);
    out.setEngineSpeedLimitIgnRpm(m.Engine_Speed_Limit_Ignition);
    out.setAmbientPressureKpa(m.Ambient_Pressure);
    pub.put();
}

static void publishRuntimeWarnings(const M1_GEN_0x64C_t& m, pub_sub::ZenohPublisher<MotecM1RuntimeWarnings>& pub)
{
    auto& out = pub.fields();
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
    pub.put();
}

static void publishStates(const M1_GEN_0x64D_t& m, pub_sub::ZenohPublisher<MotecM1States>& pub)
{
    auto& out = pub.fields();
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
    pub.put();
}

static void publishDiagnostics(const M1_GEN_0x64E_t& m, pub_sub::ZenohPublisher<MotecM1Diagnostics>& pub)
{
    auto& out = pub.fields();
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
    pub.put();
}

static void publishTotals(const M1_GEN_0x64F_t& m, pub_sub::ZenohPublisher<MotecM1Totals>& pub)
{
    auto& out = pub.fields();
    out.setEngineRunHoursTotal(m.Engine_Run_Hours_Total);
    out.setFuelClosedLoopTrimBk1(m.Fuel_Closed_Loop_Ctrl_Bk_1_Trim);
    out.setFuelClosedLoopTrimBk2(m.Fuel_Closed_Loop_Ctrl_Bk_2_Trim);
    out.setGearboxTempC(m.Gearbox_Temperature);
    out.setFuelTankLevelL(m.Fuel_Tank_Level);
    pub.put();
}

static void publishDriverControls(const M1_GEN_0x650_t& m, pub_sub::ZenohPublisher<MotecM1DriverControls>& pub)
{
    auto& out = pub.fields();
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
    pub.put();
}

static void publishFuelSecondary(const M1_GEN_0x652_t& m, pub_sub::ZenohPublisher<MotecM1FuelSecondary>& pub)
{
    auto& out = pub.fields();
    out.setInjectorSecondaryContributionPct(m.Fuel_Injector_Sec_Contribution);
    out.setFuelTimingSecondaryDeg(m.Fuel_Timing_Secondary);
    out.setInjectorDutySecPct(m.Fuel_Injector_Duty_Cycle_Secdry);
    pub.put();
}

static void publishPressures(const M1_GEN_0x655_t& m, pub_sub::ZenohPublisher<MotecM1Pressures>& pub)
{
    auto& out = pub.fields();
    out.setBrakePressureFrontBar(m.Brake_Pressure_Front);
    out.setBrakePressureRearBar(m.Brake_Pressure_Rear);
    out.setCoolantPressureKpa(m.Coolant_Pressure);
    out.setPowerSteerPressureKpa(m.Power_Steer_Pressure);
    pub.put();
}

static void publishFlows(const M1_GEN_0x656_t& m, pub_sub::ZenohPublisher<MotecM1Flows>& pub)
{
    auto& out = pub.fields();
    out.setSteeringAngleDeg(m.Steering_Angle);
    out.setInletMassFlowGs(m.Inlet_Mass_Flow);
    out.setAirboxMassFlowGs(m.Airbox_Mass_Flow);
    out.setFuelFlowMlPerS(m.Fuel_Flow);
    pub.put();
}

static void publishInjectorPressures(const M1_GEN_0x657_t& m, pub_sub::ZenohPublisher<MotecM1InjectorPressures>& pub)
{
    auto& out = pub.fields();
    out.setFuelInjectorPrimaryPressureKpa(m.Fuel_Injector_Primary_Pressure);
    out.setFuelInjectorSecondaryPressureKpa(m.Fuel_Injector_Secondary_Pressure);
    out.setGearInputShaftRpm(m.Gear_Input_Shaft_Speed);
    out.setGearOutputShaftRpm(m.Gear_Output_Shaft_Speed);
    pub.put();
}

static void publishVehicleDynamics(const M1_GEN_0x658_t& m, pub_sub::ZenohPublisher<MotecM1VehicleDynamics>& pub)
{
    auto& out = pub.fields();
    out.setAccelLateralG(m.Vehicle_Accel_Lateral);
    out.setAccelLongitudinalG(m.Vehicle_Accel_Longitudinal);
    out.setAccelVerticalG(m.Vehicle_Accel_Vertical);
    out.setYawRateDegPerS(m.Vehicle_Yaw_Rate);
    pub.put();
}

// Aggregated publishers
static void publishFuelDirectAll(const M1_GEN_0x653_t& b1, const M1_GEN_0x654_t& b2, pub_sub::ZenohPublisher<MotecM1FuelDirectAll>& pub)
{
    auto& out = pub.fields();
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
    pub.put();
}

static void publishTurboBoth(const M1_GEN_0x6A6_t& b1, const M1_GEN_0x6A7_t& b2, pub_sub::ZenohPublisher<MotecM1Turbo>& pub)
{
    auto& out = pub.fields();
    out.setBank1SpeedHz(b1.Turbo_Bank_1_Speed);
    out.setBank1InletTempC(b1.Turbo_Bank_1_Inlet_Temp);
    out.setBank1OutletTempC(b1.Turbo_Bank_1_Temp_Outlet);
    out.setBank1InletPressureKpa(b1.Turbo_Bank_1_Pressure_Inlet);
    out.setBank2SpeedHz(b2.Turbo_Bank_2_Speed);
    out.setBank2InletTempC(b2.Turbo_Bank_2_Inlet_Temp);
    out.setBank2OutletTempC(b2.Turbo_Bank_2_Temp_Outlet);
    pub.put();
}

static void publishKnock1to12(const M1_GEN_0x643_t& k1, const M1_GEN_0x659_t& k2, pub_sub::ZenohPublisher<MotecM1KnockLevels1to12>& pub)
{
    auto& out = pub.fields();
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
    pub.put();
}

static void publishIgnTrim1to12(const M1_GEN_0x64B_t& t1, const M1_GEN_0x65A_t& t2, pub_sub::ZenohPublisher<MotecM1IgnitionTrim1to12>& pub)
{
    auto& out = pub.fields();
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
    pub.put();
}

static void publishLapTiming(const M1_GEN_0x65B_t& m, pub_sub::ZenohPublisher<MotecM1LapTiming>& pub)
{
    auto& out = pub.fields();
    out.setLapTimeS(m.Lap_Time);
    out.setLapTimeRunningS(m.Lap_Time_Running);
    out.setLapNumber(m.Lap_Number);
    out.setLapDistanceM(m.Lap_Distance);
    pub.put();
}

static void publishDiffAndRotary(const M1_GEN_0x65C_t& m, pub_sub::ZenohPublisher<MotecM1DiffAndRotary>& pub)
{
    auto& out = pub.fields();
    out.setDifferentialTempFrontC(m.Differential_Temperature_Front);
    out.setRotary7(m.Driver_Rotary_Switch_7);
    out.setRotary8(m.Driver_Rotary_Switch_8);
    pub.put();
}

static void publishBrakeTemperatures(const M1_GEN_0x65D_t& m, pub_sub::ZenohPublisher<MotecM1BrakeTemperatures>& pub)
{
    auto& out = pub.fields();
    out.setFrontLeftC(m.Brake_Temperature_Front_Left);
    out.setFrontRightC(m.Brake_Temperature_Front_Right);
    out.setRearLeftC(m.Brake_Temperature_Rear_Left);
    out.setRearRightC(m.Brake_Temperature_Rear_Right);
    pub.put();
}

static void publishExhaustPressures(const M1_GEN_0x65E_t& m, pub_sub::ZenohPublisher<MotecM1ExhaustPressures>& pub)
{
    auto& out = pub.fields();
    out.setExhaustPressureB1Kpa(m.Exhaust_Pressure_Bank_1);
    out.setExhaustPressureB2Kpa(m.Exhaust_Pressure_Bank_2);
    out.setEngineCrankCasePressureKpa(m.Engine_Crank_Case_Pressure);
    out.setAlternatorCurrentA(m.Alternator_Current);
    pub.put();
}

static void publishThresholdsAndLimits(const M1_GEN_0x65F_t& m, pub_sub::ZenohPublisher<MotecM1ThresholdsAndLimits>& pub)
{
    auto& out = pub.fields();
    out.setKnockThresholdPct(m.Knock_Threshold);
    out.setLoggingSystem1UsedPct(m.Logging_System_1_Used);
    out.setVehiclePitSpeedLimitKph(m.Vehicle_Pit_Speed_Limit);
    pub.put();
}

static void publishAuxOutputs(const M1_GEN_0x6A0_t& m, pub_sub::ZenohPublisher<MotecM1AuxOutputs>& pub)
{
    auto& out = pub.fields();
    out.setAuxOut1DutyPct(m.Aux_Output_1_Duty_Cycle);
    out.setAuxOut2DutyPct(m.Aux_Output_2_Duty_Cycle);
    out.setAuxOut3DutyPct(m.Aux_Output_3_Duty_Cycle);
    out.setAuxOut4DutyPct(m.Aux_Output_4_Duty_Cycle);
    pub.put();
}

static void publishAuxOutput5(const M1_GEN_0x6A1_t& m, pub_sub::ZenohPublisher<MotecM1AuxOutput5>& pub)
{
    auto& out = pub.fields();
    out.setAuxOut5DutyPct(m.Aux_Output_5_Duty_Cycle);
    pub.put();
}

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    core::setupLogging({.program = "motec_m1"});

    cxxopts::Options options("motec_m1", "MoTeC M1 node");
    options.add_options()
        ("h,help", "Print usage");

    auto result = options.parse(argc, argv);
    if (result.count("help"))
    {
        SPDLOG_INFO("{}", options.help());
        return 0;
    }

    // Announce this process so tools can put a name to the session id that
    // appears on every topic it advertises and every sample it stamps. See
    // pub_sub/node_identity.h.
    pub_sub::NodeIdentity node_identity("motec_m1");

    pub_sub::ZenohPublisher<MotecM1EngineAir> pubEngineAir("nodes/motec_m1/engine_air");
    pub_sub::ZenohPublisher<MotecM1FuelStatus> pubFuelStatus("nodes/motec_m1/fuel_status");
    pub_sub::ZenohPublisher<MotecM1Temperatures> pubTemps("nodes/motec_m1/temperatures");
    pub_sub::ZenohPublisher<MotecM1Exhaust> pubExhaust("nodes/motec_m1/exhaust");
    pub_sub::ZenohPublisher<MotecM1ThrottleTiming> pubThrottleTiming("nodes/motec_m1/throttle_timing");
    pub_sub::ZenohPublisher<MotecM1CutsAndOilPressure> pubCutsOil("nodes/motec_m1/cuts_oil_pressure");
    pub_sub::ZenohPublisher<MotecM1BoostStatus> pubBoost("nodes/motec_m1/boost_status");
    pub_sub::ZenohPublisher<MotecM1InletCam> pubInletCam("nodes/motec_m1/inlet_cam");
    pub_sub::ZenohPublisher<MotecM1ExhaustCam> pubExhaustCam("nodes/motec_m1/exhaust_cam");
    pub_sub::ZenohPublisher<MotecM1WheelSpeeds> pubWheelSpeeds("nodes/motec_m1/wheel_speeds");
    pub_sub::ZenohPublisher<MotecM1Environment> pubEnv("nodes/motec_m1/environment");
    pub_sub::ZenohPublisher<MotecM1RuntimeWarnings> pubWarnings("nodes/motec_m1/runtime_warnings");
    pub_sub::ZenohPublisher<MotecM1States> pubStates("nodes/motec_m1/states");
    pub_sub::ZenohPublisher<MotecM1Diagnostics> pubDiag("nodes/motec_m1/diagnostics");
    pub_sub::ZenohPublisher<MotecM1Totals> pubTotals("nodes/motec_m1/totals");
    pub_sub::ZenohPublisher<MotecM1DriverControls> pubDriver("nodes/motec_m1/driver_controls");
    pub_sub::ZenohPublisher<MotecM1FuelSecondary> pubFuelSec("nodes/motec_m1/fuel_secondary");
    pub_sub::ZenohPublisher<MotecM1FuelDirectAll> pubFuelDirectAll("nodes/motec_m1/fuel_direct_all");
    pub_sub::ZenohPublisher<MotecM1Pressures> pubPressures("nodes/motec_m1/pressures");
    pub_sub::ZenohPublisher<MotecM1Flows> pubFlows("nodes/motec_m1/flows");
    pub_sub::ZenohPublisher<MotecM1InjectorPressures> pubInjPress("nodes/motec_m1/injector_pressures");
    pub_sub::ZenohPublisher<MotecM1VehicleDynamics> pubVehDyn("nodes/motec_m1/vehicle_dynamics");
    pub_sub::ZenohPublisher<MotecM1LapTiming> pubLap("nodes/motec_m1/lap_timing");
    pub_sub::ZenohPublisher<MotecM1DiffAndRotary> pubDiffRot("nodes/motec_m1/diff_rotary");
    pub_sub::ZenohPublisher<MotecM1BrakeTemperatures> pubBrakeTemps("nodes/motec_m1/brake_temperatures");
    pub_sub::ZenohPublisher<MotecM1ExhaustPressures> pubExhPress("nodes/motec_m1/exhaust_pressures");
    pub_sub::ZenohPublisher<MotecM1ThresholdsAndLimits> pubThresh("nodes/motec_m1/thresholds_limits");
    pub_sub::ZenohPublisher<MotecM1AuxOutputs> pubAuxOuts("nodes/motec_m1/aux_outputs");
    pub_sub::ZenohPublisher<MotecM1AuxOutput5> pubAux5("nodes/motec_m1/aux_output5");
    pub_sub::ZenohPublisher<MotecM1Turbo> pubTurbo("nodes/motec_m1/turbo");

    pub_sub::ZenohPublisher<MotecM1KnockLevels1to12> pubKnock1to12("nodes/motec_m1/knock_levels_1_12");
    pub_sub::ZenohPublisher<MotecM1IgnitionTrim1to12> pubIgnTrim1to12("nodes/motec_m1/ignition_trim_1_12");

    dbc_motec_m1_rev3_parser parser;
    parser.on_M1_GEN_0x640([&](const M1_GEN_0x640_t& m){ publishEngineAir(m, pubEngineAir); });
    parser.on_M1_GEN_0x641([&](const M1_GEN_0x641_t& m){ publishFuelStatus(m, pubFuelStatus); });
    parser.on_M1_GEN_0x649([&](const M1_GEN_0x649_t& m){ publishTemperatures(m, pubTemps); });
    parser.on_M1_GEN_0x651([&](const M1_GEN_0x651_t& m){ publishExhaust(m, pubExhaust); });
    parser.on_M1_GEN_0x642([&](const M1_GEN_0x642_t& m){ publishThrottleTiming(m, pubThrottleTiming); });
    parser.on_M1_GEN_0x644([&](const M1_GEN_0x644_t& m){ publishCutsAndOilPressure(m, pubCutsOil); });
    parser.on_M1_GEN_0x645([&](const M1_GEN_0x645_t& m){ publishBoostStatus(m, pubBoost); });
    parser.on_M1_GEN_0x646([&](const M1_GEN_0x646_t& m){ publishInletCam(m, pubInletCam); });
    parser.on_M1_GEN_0x647([&](const M1_GEN_0x647_t& m){ publishExhaustCam(m, pubExhaustCam); });
    parser.on_M1_GEN_0x648([&](const M1_GEN_0x648_t& m){ publishWheelSpeeds(m, pubWheelSpeeds); });
    parser.on_M1_GEN_0x64A([&](const M1_GEN_0x64A_t& m){ publishEnvironment(m, pubEnv); });
    parser.on_M1_GEN_0x64C([&](const M1_GEN_0x64C_t& m){ publishRuntimeWarnings(m, pubWarnings); });
    parser.on_M1_GEN_0x64D([&](const M1_GEN_0x64D_t& m){ publishStates(m, pubStates); });
    parser.on_M1_GEN_0x64E([&](const M1_GEN_0x64E_t& m){ publishDiagnostics(m, pubDiag); });
    parser.on_M1_GEN_0x64F([&](const M1_GEN_0x64F_t& m){ publishTotals(m, pubTotals); });
    parser.on_M1_GEN_0x650([&](const M1_GEN_0x650_t& m){ publishDriverControls(m, pubDriver); });
    parser.on_M1_GEN_0x652([&](const M1_GEN_0x652_t& m){ publishFuelSecondary(m, pubFuelSec); });
    parser.add_message_aggregator<
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x653,
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x654
    >([&](const dbc_motec_m1_rev3::dbc_motec_m1_rev3_t& db){
        publishFuelDirectAll(db.M1_GEN_0x653, db.M1_GEN_0x654, pubFuelDirectAll);
    });
    parser.on_M1_GEN_0x655([&](const M1_GEN_0x655_t& m){ publishPressures(m, pubPressures); });
    parser.on_M1_GEN_0x656([&](const M1_GEN_0x656_t& m){ publishFlows(m, pubFlows); });
    parser.on_M1_GEN_0x657([&](const M1_GEN_0x657_t& m){ publishInjectorPressures(m, pubInjPress); });
    parser.on_M1_GEN_0x658([&](const M1_GEN_0x658_t& m){ publishVehicleDynamics(m, pubVehDyn); });
    parser.add_message_aggregator<
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x643,
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x659
    >([&](const dbc_motec_m1_rev3::dbc_motec_m1_rev3_t& db){
        publishKnock1to12(db.M1_GEN_0x643, db.M1_GEN_0x659, pubKnock1to12);
    });
    parser.add_message_aggregator<
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x64B,
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x65A
    >([&](const dbc_motec_m1_rev3::dbc_motec_m1_rev3_t& db){
        publishIgnTrim1to12(db.M1_GEN_0x64B, db.M1_GEN_0x65A, pubIgnTrim1to12);
    });
    parser.on_M1_GEN_0x65B([&](const M1_GEN_0x65B_t& m){ publishLapTiming(m, pubLap); });
    parser.on_M1_GEN_0x65C([&](const M1_GEN_0x65C_t& m){ publishDiffAndRotary(m, pubDiffRot); });
    parser.on_M1_GEN_0x65D([&](const M1_GEN_0x65D_t& m){ publishBrakeTemperatures(m, pubBrakeTemps); });
    parser.on_M1_GEN_0x65E([&](const M1_GEN_0x65E_t& m){ publishExhaustPressures(m, pubExhPress); });
    parser.on_M1_GEN_0x65F([&](const M1_GEN_0x65F_t& m){ publishThresholdsAndLimits(m, pubThresh); });
    parser.on_M1_GEN_0x6A0([&](const M1_GEN_0x6A0_t& m){ publishAuxOutputs(m, pubAuxOuts); });
    parser.on_M1_GEN_0x6A1([&](const M1_GEN_0x6A1_t& m){ publishAuxOutput5(m, pubAux5); });
    parser.add_message_aggregator<
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x6A6,
        dbc_motec_m1_rev3::dbc_motec_m1_rev3_t::Messages::M1_GEN_0x6A7
    >([&](const dbc_motec_m1_rev3::dbc_motec_m1_rev3_t& db){
        publishTurboBoth(db.M1_GEN_0x6A6, db.M1_GEN_0x6A7, pubTurbo);
    });

    pub_sub::ZenohTypedSubscriber<CanFrame> can_subscriber(
        "vehicle/can0/rx",
        [&parser](CanFrame::Reader frame)
        {
            uint32_t id = frame.getId();
            uint8_t len = frame.getLen();
            auto dataList = frame.getData();

            std::array<uint8_t, 8u> bytes{};
            const size_t n = std::min<size_t>(8u, std::min<size_t>(len, dataList.size()));
            for (size_t i = 0; i < n; ++i)
            {
                bytes[i] = static_cast<uint8_t>(dataList[i]);
            }
            // The real length, not the padded buffer: a frame shorter than the
            // message it claims to be must be rejected, not decoded as though
            // the padding were readings.
            parser.handle_can_frame(id, std::span<const uint8_t>(bytes.data(), n));
        });

    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}


