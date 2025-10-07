@0xf1d4c3b2a190e7d5;

# MoTeC M1 Rev3 schema (subset of key signals for dashboard use)

struct MotecM1EngineAir {
  engineSpeedRpm @0 : UInt16;           # Engine_Speed (rpm)
  mapKpa @1 : Float32;                  # Inlet_Manifold_Pressure (kPa)
  inletManifoldTempC @2 : Float32;      # Inlet_Manifold_Temperature (C)
  throttlePositionPct @3 : Float32;     # Throttle_Position (%)
}

struct MotecM1FuelStatus {
  fuelVolumeUl @0 : UInt16;             # Fuel_Volume (uL)
  fuelMixtureAimLambda @1 : Float32;    # Fuel_Mixture_Aim (lambda)
  fuelPressureKpa @2 : Float32;         # Fuel_Pressure_Sensor (kPa)
  injectorDutyPct @3 : Float32;         # Fuel_Injector_Duty_Cycle (%)
  engineEfficiencyPct @4 : Float32;     # Engine_Efficiency (%)
}

struct MotecM1Temperatures {
  coolantTempC @0 : Int8;               # Coolant_Temperature (C)
  engineOilTempC @1 : Int8;             # Engine_Oil_Temperature (C)
  fuelTempC @2 : Int8;                  # Fuel_Temperature (C)
  ambientTempC @3 : Int8;               # Ambient_Temperature (C)
  airboxTempC @4 : Int8;                # Airbox_Temperature (C)
  ecuBatteryVolts @5 : Float32;         # ECU_Battery_Voltage (V)
  fuelUsedL @6 : Float32;               # Fuel_Used (L)
}

struct MotecM1Exhaust {
  exhaustLambda @0 : Float32;           # Exhaust_Lambda
  exhaustLambdaBank1 @1 : Float32;      # Exhaust_Lambda_Bank_1
  exhaustLambdaBank2 @2 : Float32;      # Exhaust_Lambda_Bank_2
  exhaustTempBank1C @3 : Float32;       # Exhaust_Temp_Bank_1 (C)
  exhaustTempBank2C @4 : Float32;       # Exhaust_Temp_Bank_2 (C)
}

struct MotecM1ThrottleTiming {
  throttlePedalPct @0 : Float32;        # Throttle_Pedal (%)
  engineLoadMg @1 : Float32;            # Engine_Load (mg)
  ignitionTimingDeg @2 : Float32;       # Ignition_Timing (deg)
  fuelTimingDeg @3 : Float32;           # Fuel_Timing (deg)
}

 

struct MotecM1CutsAndOilPressure {
  ignitionCutCount @0 : UInt8;          # Ignition_Output_Cut_Count
  fuelCutCount @1 : UInt8;              # Fuel_Output_Cut_Count
  ignitionCutAvgPct @2 : Float32;       # Ignition_Output_Cut_Average (%)
  fuelCutAvgPct @3 : Float32;           # Fuel_Output_Cut_Average (%)
  fuelCyl1PulseWidthMs @4 : Float32;    # Fuel_Cyl_1_Output_Pulse_Width (ms)
  ignitionCutRequestState @5 : UInt8;   # Ignition_Cut_Request_State
  ignitionTimingState @6 : UInt8;       # Ignition_Timing_State
  engineOilPressureKpa @7 : Float32;    # Engine_Oil_Pressure (kPa)
}

struct MotecM1BoostStatus {
  boostPressureKpa @0 : Float32;        # Boost_Pressure (kPa)
  boostAimKpa @1 : Float32;             # Boost_Aim (kPa)
  actuatorDutyPct @2 : Float32;         # Boost_Actuator_Output_Duty_Cycle (%)
  gearLeverForceN @3 : Float32;         # Gear_Lever_Force (N)
}

struct MotecM1InletCam {
  aimDeg @0 : Float32;                  # Inlet_Camshaft_Aim (deg)
  bank1PositionDeg @1 : Float32;        # Inlet_Camshaft_Bank_1_Position (deg)
  bank2PositionDeg @2 : Float32;        # Inlet_Camshaft_Bank_2_Position (deg)
  bank1DutyPct @3 : Float32;            # Inlet_Cam_Bk_1_Output_Duty_Cycle (%)
  bank2DutyPct @4 : Float32;            # Inlet_Cam_Bk_2_Output_Duty_Cycle (%)
}

struct MotecM1ExhaustCam {
  aimDeg @0 : Float32;                  # Exhaust_Camshaft_Aim (deg)
  bank1PositionDeg @1 : Float32;        # Exhaust_Camshaft_Bank_1_Position (deg)
  bank2PositionDeg @2 : Float32;        # Exhaust_Camshaft_Bank_2_Position (deg)
  bank1DutyPct @3 : Float32;            # Exh_Cam_Bk_1_Output_Duty_Cycle (%)
  bank2DutyPct @4 : Float32;            # Exh_Cam_Bk_2_Output_Duty_Cycle (%)
}

struct MotecM1WheelSpeeds {
  frontLeftKph @0 : Float32;            # Wheel_Speed_Front_Left (km/h)
  frontRightKph @1 : Float32;           # Wheel_Speed_Front_Right (km/h)
  rearLeftKph @2 : Float32;             # Wheel_Speed_Rear_Left (km/h)
  rearRightKph @3 : Float32;            # Wheel_Speed_Rear_Right (km/h)
}

struct MotecM1Environment {
  exhaustTempC @0 : Float32;            # Exhaust_Temperature (C)
  engineLoadAvgPct @1 : Float32;        # Engine_Load_Average (%)
  engineSpeedLimitIgnRpm @2 : UInt16;   # Engine_Speed_Limit_Ignition (rpm)
  ambientPressureKpa @3 : Float32;      # Ambient_Pressure (kPa)
}

 

struct MotecM1RuntimeWarnings {
  engineRunTimeS @0 : UInt16;           # Engine_Run_Time (s)
  ecuUpTimeS @1 : UInt16;               # ECU_Up_Time (s)
  warningSource @2 : UInt8;             # Warning_Source
  fuelPressureWarn @3 : Bool;
  crankcasePressureWarn @4 : Bool;
  engineOilPressureWarn @5 : Bool;
  engineOilTempWarn @6 : Bool;
  engineSpeedWarn @7 : Bool;
  coolantPressureWarn @8 : Bool;
  coolantTempWarn @9 : Bool;
  knockWarn @10 : Bool;
}

struct MotecM1States {
  fuelPumpState @0 : UInt8;
  engineState @1 : UInt8;
  launchState @2 : UInt8;
  antiLagState @3 : UInt8;
  engineSpeedLimitState @4 : UInt8;
  boostAimState @5 : UInt8;
  fuelCutState @6 : UInt8;
  engineOverrunState @7 : UInt8;
  knockState @8 : UInt8;
  fuelPurgeState @9 : UInt8;
  fuelClosedLoopState @10 : UInt8;
  throttleAimState @11 : UInt8;
  gear @12 : Int8;
  engineSpeedRefState @13 : UInt8;
  engineSpeedLimitState2 @14 : UInt8;    # 8-bit state
}

struct MotecM1Diagnostics {
  launchDiagnostic @0 : Int8;           # 4-bit signed
  antiLagDiagnostic @1 : Int8;
  fuelCutState @2 : UInt8;
  boostControlDiagnostic @3 : Int8;
  fuelClosedLoopDiagnostic @4 : Int8;
  neutralSwitch @5 : Bool;
  engineRunSwitch @6 : Bool;
  antiLagSwitch @7 : Bool;
  brakeState @8 : Bool;
  tractionEnableSwitch @9 : Bool;
  launchEnableSwitch @10 : Bool;
  pitSwitch @11 : Bool;
  engineOilPressureLowSwitch @12 : Bool;
  boostLimitDisableSwitch @13 : Bool;
  throttlePedalTransSwitch @14 : Bool;
  raceTimeResetSwitch @15 : Bool;
}

struct MotecM1Totals {
  engineRunHoursTotal @0 : Float32;
  fuelClosedLoopTrimBk1 @1 : Float32;   # fraction
  fuelClosedLoopTrimBk2 @2 : Float32;   # fraction
  gearboxTempC @3 : Float32;
  fuelTankLevelL @4 : Float32;
}

struct MotecM1DriverControls {
  rotary1 @0 : UInt8;
  rotary2 @1 : UInt8;
  rotary3 @2 : UInt8;
  rotary4 @3 : UInt8;
  rotary5 @4 : UInt8;
  rotary6 @5 : UInt8;
  switch8 @6 : Bool;
  switch7 @7 : Bool;
  switch6 @8 : Bool;
  switch5 @9 : Bool;
  switch4 @10 : Bool;
  switch3 @11 : Bool;
  switch2 @12 : Bool;
  switch1 @13 : Bool;
}

struct MotecM1FuelSecondary {
  injectorSecondaryContributionPct @0 : Float32;  # Fuel_Injector_Sec_Contribution
  fuelTimingSecondaryDeg @1 : Float32;            # Fuel_Timing_Secondary
  injectorDutySecPct @2 : Float32;                # Fuel_Injector_Duty_Cycle_Secdry
}

 

struct MotecM1FuelDirectAll {
  # Bank 1
  fuelPressureDirectKpa @0 : Float32;
  fuelPressureDirectAimKpa @1 : Float32;
  fuelPressureDirectControlPct @2 : Float32;
  fuelPressureDirectFeedFwdPct @3 : Float32;
  fuelPressureDirectPropPct @4 : Float32;
  fuelPressureDirectIntegralPct @5 : Float32;
  # Bank 2
  fuelPressureDirectB2Kpa @6 : Float32;
  fuelPressureDirectB2AimKpa @7 : Float32;
  fuelPressureDirectB2ControlPct @8 : Float32;
  fuelPressureDirectB2FeedFwdPct @9 : Float32;
  fuelPressureDirectB2PropPct @10 : Float32;
  fuelPressureDirectB2IntegralPct @11 : Float32;
}

struct MotecM1Pressures {
  brakePressureFrontBar @0 : Float32;
  brakePressureRearBar @1 : Float32;
  coolantPressureKpa @2 : Float32;
  powerSteerPressureKpa @3 : Float32;
}

struct MotecM1Flows {
  steeringAngleDeg @0 : Float32;
  inletMassFlowGs @1 : Float32;
  airboxMassFlowGs @2 : Float32;
  fuelFlowMlPerS @3 : Float32;
}

struct MotecM1InjectorPressures {
  fuelInjectorPrimaryPressureKpa @0 : Float32;
  fuelInjectorSecondaryPressureKpa @1 : Float32;
  gearInputShaftRpm @2 : UInt16;
  gearOutputShaftRpm @3 : UInt16;
}

struct MotecM1VehicleDynamics {
  accelLateralG @0 : Float32;
  accelLongitudinalG @1 : Float32;
  accelVerticalG @2 : Float32;
  yawRateDegPerS @3 : Float32;
}

 

struct MotecM1LapTiming {
  lapTimeS @0 : Float32;
  lapTimeRunningS @1 : Float32;
  lapNumber @2 : UInt16;
  lapDistanceM @3 : Float32;
}

struct MotecM1DiffAndRotary {
  differentialTempFrontC @0 : Float32;
  rotary7 @1 : Int8;  # Driver_Rotary_Switch_7
  rotary8 @2 : Int8;  # Driver_Rotary_Switch_8
}

struct MotecM1BrakeTemperatures {
  frontLeftC @0 : Float32;
  frontRightC @1 : Float32;
  rearLeftC @2 : Float32;
  rearRightC @3 : Float32;
}

struct MotecM1ExhaustPressures {
  exhaustPressureB1Kpa @0 : Float32;
  exhaustPressureB2Kpa @1 : Float32;
  engineCrankCasePressureKpa @2 : Float32;
  alternatorCurrentA @3 : Float32;
}

struct MotecM1ThresholdsAndLimits {
  knockThresholdPct @0 : Float32;
  loggingSystem1UsedPct @1 : Float32;
  vehiclePitSpeedLimitKph @2 : Float32;
}

struct MotecM1AuxOutputs {
  auxOut1DutyPct @0 : Float32;
  auxOut2DutyPct @1 : Float32;
  auxOut3DutyPct @2 : Float32;
  auxOut4DutyPct @3 : Float32;
}

struct MotecM1AuxOutput5 {
  auxOut5DutyPct @0 : Float32;
}

 

struct MotecM1Turbo {
  # Bank 1
  bank1SpeedHz @0 : Float32;
  bank1InletTempC @1 : Float32;
  bank1OutletTempC @2 : Float32;
  bank1InletPressureKpa @3 : Float32;
  # Bank 2
  bank2SpeedHz @4 : Float32;
  bank2InletTempC @5 : Float32;
  bank2OutletTempC @6 : Float32;
}

struct MotecM1KnockLevels1to12 {
  cyl1 @0 : Float32;
  cyl2 @1 : Float32;
  cyl3 @2 : Float32;
  cyl4 @3 : Float32;
  cyl5 @4 : Float32;
  cyl6 @5 : Float32;
  cyl7 @6 : Float32;
  cyl8 @7 : Float32;
  cyl9 @8 : Float32;
  cyl10 @9 : Float32;
  cyl11 @10 : Float32;
  cyl12 @11 : Float32;
}

struct MotecM1IgnitionTrim1to12 {
  cyl1Deg @0 : Float32;
  cyl2Deg @1 : Float32;
  cyl3Deg @2 : Float32;
  cyl4Deg @3 : Float32;
  cyl5Deg @4 : Float32;
  cyl6Deg @5 : Float32;
  cyl7Deg @6 : Float32;
  cyl8Deg @7 : Float32;
  cyl9Deg @8 : Float32;
  cyl10Deg @9 : Float32;
  cyl11Deg @10 : Float32;
  cyl12Deg @11 : Float32;
}


