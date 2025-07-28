@0xc9f2e4d8b1a37755;

# Schema for vehicle warning telltales

struct BatteryWarning {
  # Battery warning status
  isWarningActive @0 :Bool;
  # True if battery warning is active, false otherwise

  batteryVoltage @1 :Float32;
  
  timestamp @2 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken
}
