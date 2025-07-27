@0x85150b117366d14b;

# Schema for engine telemetry data

struct EngineRpm {
  # Engine RPM data
  rpm @0 :UInt32;
  # Current engine revolutions per minute
  
  timestamp @1 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken
} 