@0x95250c227477e25c;

# Schema for vehicle odometer telemetry data

struct VehicleOdometer {
  timestamp @0 :UInt64;
  # Unix timestamp in milliseconds when this reading was taken

  # Odometer data
  totalMiles @1 : UInt32;
  # Total distance traveled in miles (0-999999)
}
