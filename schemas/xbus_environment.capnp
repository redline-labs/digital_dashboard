@0xa5aaccce2d3cf705;

using Common = import "xbus_common.capnp";

# The non-inertial outputs of an MTi-610.

# XDI 0xC020. The magnetic field.
#
# THE UNIT IS ARBITRARY UNITS, and that is the LLCP's own wording rather than a
# gap in this comment. The device normalises to roughly 1.0 at the local field
# strength when it was calibrated. It is not tesla, not gauss, and not
# convertible to either without knowing the field where the calibration
# happened -- so the field names say Au, and calling them Ut would be a lie
# that every consumer would then inherit.
struct XbusMagneticField {
  header @0 :Common.XbusSampleHeader;

  magneticFieldXAu @1 :Float64;
  magneticFieldYAu @2 :Float64;
  magneticFieldZAu @3 :Float64;
}

# XDI 0x0810. The sensor's internal temperature, not the ambient temperature --
# it reads high by however much the device is dissipating.
struct XbusTemperature {
  header @0 :Common.XbusSampleHeader;

  temperatureC @1 :Float64;
}

# XDI 0x3010. Barometric pressure.
#
# The wire carries whole pascals as a UInt32 and so does this: converting to
# an altitude would need a reference pressure the device has no way to know,
# and a consumer that wants one should say which it is assuming.
struct XbusBaroPressure {
  header @0 :Common.XbusSampleHeader;

  pressurePa @1 :UInt32;
}
