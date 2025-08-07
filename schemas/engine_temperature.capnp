@0xa8c3d2e1f5b74437;

struct EngineTemperature {
  timestamp @0 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken

  # Engine temperature data
  temperatureCelsius @1 :Float32;
  # Current engine temperature in degrees Celsius
  
  
} 
