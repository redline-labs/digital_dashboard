@0xc9a2a0983a4f9b8a;

# Megasquirt simplified dash broadcast (aggregated across 5 frames)
struct MegasquirtDash {
  # Frame 0
  rpm @0 : UInt16;                # engine RPM
  mapKpa @1 : Float32;            # manifold absolute pressure (kPa)
  tpsPct @2 : Float32;            # throttle position (%)
  coolantTempF @3 : Float32;      # coolant temp (F)

  # Frame 1
  ignitionAdvanceDeg @4 : Float32;  # spark advance (deg BTDC)
  intakeAirTempF @5 : Float32;      # manifold air temp (F)
  injPw1Ms @6 : Float32;            # pulsewidth bank1 (ms)
  injPw2Ms @7 : Float32;            # pulsewidth bank2 (ms)

  # Frame 2
  seqPw1Ms @8 : Float32;          # sequential pw cyl1 (ms)
  egt1F @9 : Float32;             # EGT cyl1 (F)
  egoCorrectionPct @10 : Float32; # EGO bank1 correction (%)
  afr1 @11 : Float32;             # AFR cyl1
  afrTarget1 @12 : Float32;       # AFR target bank1

  # Frame 3
  knockRetardDeg @13 : Float32;   # knock retard (deg)
  sensor1 @14 : Float32;          # generic sensor input 1
  sensor2 @15 : Float32;          # generic sensor input 2
  batteryVolts @16 : Float32;     # battery voltage (V)

  # Frame 4
  launchTimingDeg @17 : Float32;  # launch control timing (deg)
  tcRetard @18 : Float32;         # traction control retard (units per DBC)
  vssMps @19 : Float32;           # vehicle speed (m/s)
}


