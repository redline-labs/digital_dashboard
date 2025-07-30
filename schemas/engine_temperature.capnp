@0xa8c3d2e1f5b74437;

struct EngineTemperature {
  # Engine temperature data
  temperatureCelsius @0 :Float32;
  # Current engine temperature in degrees Celsius
  
  timestamp @1 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken
} 
