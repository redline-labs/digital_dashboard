@0xc9f2e4d8b1a37755;

# Schema for vehicle warning telltales

struct BatteryWarning {
  # Battery warning status
  isWarningActive @0 :Bool;
  # True if battery warning is active, false otherwise
  
  timestamp @1 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken
}

# Future warning types can be added here:
# struct OilPressureWarning { ... }
# struct EngineWarning { ... }
# struct BrakeWarning { ... } 