@0x85150b117366d14b;

# Schema for vehicle speed telemetry data

struct VehicleSpeed {
  timestamp @0 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken

  # Vehicle speed data
  speedMps @1 : Float32;
  # Current vehicle speed in meters per second
} 