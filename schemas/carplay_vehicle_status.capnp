@0xb2c48a9e4dfdda32;

# What the vehicle reports to the phone, from whatever knows it (a CAN bridge,
# a trip computer). Each field is sent only when its has* flag is set, and an
# unset one leaves the value the node already has: a publisher that knows only
# the range does not erase the temperature.
struct CarPlayVehicleStatus {
  rangeKm             @0 :UInt16;
  hasRangeKm          @1 :Bool;
  outsideTemperatureC @2 :Int16;
  hasOutsideTemperature @3 :Bool;
  # True when the range is low enough that CarPlay should say so.
  rangeWarning        @4 :Bool;
  hasRangeWarning     @5 :Bool;
}
