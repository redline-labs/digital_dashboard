@0x85150b117366d14b;

# Schema for vehicle speed telemetry data

struct VehicleSpeed {
  # Vehicle speed data
  speedMps @0 :Float32;
  # Current vehicle speed in meters per second
  
  timestamp @1 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken
} 