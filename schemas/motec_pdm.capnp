@0xa2b3c4d5e6f70123;

# MoTeC PDM Generic Output schema (derived from PDM_Generic_Output.dbc)

enum PdmOutputStatusEnum {
  off @0;               # 0
  on @1;                # 1
  faultError @2;        # 2
  overCurrentError @3;  # 4
  retriesReached @4;    # 8
}

struct MotecPdmInputState {
  resetSource @0 : UInt8;                 # PDM_Reset_Source
  rail9v5Volts @1 : Float32;              # PDM_9V5_Internal_Rail_Voltage (V)
  totalCurrentA @2 : Float32;             # PDM_Total_Current (A)
  globalErrorFlag @3 : UInt8;             # PDM_Global_Error_Flag
  batteryVolts @4 : Float32;              # PDM_Battery_Voltage (V)
  internalTempC @5 : Float32;             # PDM_Internal_Temperature (C)
  inputs @6 : List(Bool);                 # PDM_Input_1..23
}

struct MotecPdmInfo {
  serialNumberLow @0 : UInt8;             # PDM_Serial_Number_Low
  serialNumberHigh @1 : UInt8;            # PDM_Serial_Number_High
  fwVersionLetter @2 : UInt8;             # PDM_Firmware_Version_Letter
  fwVersionMinor @3 : UInt8;              # PDM_Firmware_Version_Minor
  fwVersionMajor @4 : UInt8;              # PDM_Firmware_Version_Major
}

struct MotecPdmOutputCurrent {
  values @0 : List(Float32);              # 32 outputs (A)
}

struct MotecPdmOutputLoad {
  values @0 : List(Float32);              # 32 outputs (%)
}

struct MotecPdmOutputVoltage {
  values @0 : List(Float32);              # 32 outputs (V)
}

struct MotecPdmOutputStatus {
  values @0 : List(PdmOutputStatusEnum);  # 32 outputs
}

struct MotecPdmInputVoltage {
  values @0 : List(Float32);              # up to 23 inputs (V)
}


