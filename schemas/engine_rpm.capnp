@0xa8c3d2e1f5b74436;

struct EngineRpm {
  # Engine RPM data
  rpm @0 :UInt32;
  # Current engine revolutions per minute
  
  timestamp @1 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken
}