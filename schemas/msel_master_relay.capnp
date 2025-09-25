@0xdfc5a0b8b4a87a1b;

struct MselMasterRelayStatus {
    status @0 : UInt8;                       # raw status byte
    overTempWarn @1 : Bool;
    externalKill @2 : Bool;
    driverKill @3 : Bool;
    overTempKill @4 : Bool;
    highVoltageWarn @5 : Bool;
    lowVoltageWarn @6 : Bool;
    overCurrentWarn @7 : Bool;
    canKill @8 : Bool;
    temperatureInternal @9 : Float32;        # deg C
    loadCurrent @10 : Float32;               # A
    voltageOut @11 : Float32;                # V
}

struct MselMasterRelayInfo {
    shutdownCause2 @0 : UInt8;
    shutdownCause @1 : UInt8;
    timeSinceShutdown @2 : Float32;          # seconds
    configShutdownDelay @3 : Float32;        # seconds
    configCanKill @4 : UInt8;                # enum-like
    configCanBaud @5 : UInt8;                # enum-like
    configOutputDrive @6 : UInt8;            # enum-like
    serialNo @7 : UInt32;
    voltageIn @8 : Float32;                  # V
}


