@0xa8c3d2e1f5b74436;

struct EngineRpm {
  timestamp @0 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken

  # Engine RPM data
  rpm @1 :UInt32;

  oilPressurePsi @2 :Float32;
}