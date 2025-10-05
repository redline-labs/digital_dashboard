@0xd1f4a3b0d2f7a9c1;

# MoTeC LTC Rev1 schema (from Motec_LTC_Rev1.dbc)

enum LtcSensorState {
  start @0;
  diagnostics @1;
  preCal @2;
  calibration @3;
  postCal @4;
  paused @5;
  heating @6;
  running @7;
  cooling @8;
  pumpStart @9;
  pumpOff @10;
}

struct MotecLtcTelemetry {
  index @0 : UInt8;                 # LTC1_Index
  lambda @1 : Float32;              # LTC1_Lambda (afr:LA)
  ipn @2 : Float32;                 # LTC1_Ipn (mA)
  internalTempC @3 : Float32;       # LTC1_InternalTemp (C)

  sensorControlFault @4 : Bool;     # LTC1_SensorControlFault
  internalFault @5 : Bool;          # LTC1_InternalFault
  sensorWireShort @6 : Bool;        # LTC1_SensorWireShort
  heaterFailedToHeat @7 : Bool;     # LTC1_HeaterFailedtoHeat
  heaterOpenCircuit @8 : Bool;      # LTC1_HeaterOpenCircuit
  heaterShortToVbatt @9 : Bool;     # LTC1_HeaterShorttoVBATT
  heaterShortToGnd @10 : Bool;      # LTC1_HeaterShorttoGND

  heaterDutyCyclePct @11 : Float32; # LTC1_HeaterDutyCycle (%)

  sensorState @12 : LtcSensorState; # LTC1_SensorState (enum)
  battVolts @13 : Float32;          # LTC1_BattVolts (V)
  ip @14 : Float32;                 # LTC1_Ip (mA)
  ri @15 : Float32;                 # LTC1_Ri (ohm)
}


