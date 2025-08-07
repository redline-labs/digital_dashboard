@0xc9f2e4d8b1a37755;

# Schema for vehicle warning telltales

struct BatteryWarning {
  timestamp @0 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken

  # Battery warning status
  isWarningActive @1 :Bool;
  # True if battery warning is active, false otherwise

  batteryVoltage @2 :Float32;
  
  
}
