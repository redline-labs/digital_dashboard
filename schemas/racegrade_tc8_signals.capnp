@0xaa8d7b1df7a2b428;

struct RaceGradeTc8Inputs {
    voltage1 @0 : Float32;
    voltage2 @1 : Float32;
    voltage3 @2 : Float32;
    voltage4 @3 : Float32;
    voltage5 @4 : Float32;
    voltage6 @5 : Float32;
    voltage7 @6 : Float32;
    voltage8 @7 : Float32;

    temperature1 @8 : Float32;
    temperature2 @9 : Float32;
    temperature3 @10 : Float32;
    temperature4 @11 : Float32;
    temperature5 @12 : Float32;
    temperature6 @13 : Float32;
    temperature7 @14 : Float32;
    temperature8 @15 : Float32;

    frequency1 @16 : Float32;
    frequency2 @17 : Float32;
    frequency3 @18 : Float32;
    frequency4 @19 : Float32;
}

struct RaceGradeTc8Diagnostics {
    coldJunctionComp1 @0 : Float32;
    coldJunctionComp2 @1 : Float32;
    e888IntTemp @2 : Float32;

    dig1InState @3 : Bool;
    dig2InState @4 : Bool;
    dig3InState @5 : Bool;
    dig4InState @6 : Bool;
    dig5InState @7 : Bool;
    dig6InState @8 : Bool;

    batteryVolts @9 : Float32;
    e888StatusFlags @10 : UInt16;
    firmwareVersion @11 : Float32;
}
