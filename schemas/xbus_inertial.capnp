@0xed1f8b5ccf0ec2da;

using Common = import "xbus_common.capnp";

# The inertial outputs of an MTi-610. One struct per data identifier; see
# xbus_common.capnp for why each carries the packet header.
#
# AXIS CONVENTION IS THE DEVICE'S OWN and is NOT verified. No MTi has been on
# the bench, so which way x points relative to the housing, and the sign of a
# rotation about it, are taken from the datasheet and nothing else. It is on
# the hardware list in docs/nodes/mti610_bridge.md, and it is the kind of thing that is
# obvious in thirty seconds with a device and invisible without one.

# XDI 0x4020. Calibrated acceleration.
#
# THIS INCLUDES GRAVITY. It is specific force, so a device sitting still reads
# about 9.81 on whichever axis points up, not zero. The gravity-removed output
# is FreeAcceleration, which needs the orientation filter an MTi-610 does not
# have -- so on this device there is no way to get one, and a consumer that
# wants it has to remove gravity itself.
struct XbusAcceleration {
  header @0 :Common.XbusSampleHeader;

  accelerationXMps2 @1 :Float64;
  accelerationYMps2 @2 :Float64;
  accelerationZMps2 @3 :Float64;
}

# XDI 0x8020. Calibrated rate of turn.
struct XbusRateOfTurn {
  header @0 :Common.XbusSampleHeader;

  rateOfTurnXRadps @1 :Float64;
  rateOfTurnYRadps @2 :Float64;
  rateOfTurnZRadps @3 :Float64;
}

# XDI 0x4010. The velocity increment over the sample interval.
#
# Not an acceleration: it is already multiplied by dt, which is what makes it
# immune to the aliasing a sampled acceleration suffers under vibration. For
# anything integrating motion this is the output to use, not acceleration.
struct XbusDeltaV {
  header @0 :Common.XbusSampleHeader;

  deltaVXMps @1 :Float64;
  deltaVYMps @2 :Float64;
  deltaVZMps @3 :Float64;
}

# XDI 0x8030. The orientation increment over the sample interval.
#
# Wire order is w, x, y, z -- the LLCP writes it dq0..dq3. It is a rotation
# increment and not an orientation: an MTi-610 has no filter and therefore no
# opinion about which way it is pointing. Composing these gives dead-reckoned
# attitude that drifts, which is what a VRU or AHRS exists to fix.
struct XbusDeltaQ {
  header @0 :Common.XbusSampleHeader;

  deltaQW @1 :Float64;
  deltaQX @2 :Float64;
  deltaQY @3 :Float64;
  deltaQZ @4 :Float64;
}

# XDI 0x4040. The high-rate accelerometer tap, about 2000 Hz on a 600-series.
#
# IT IS NOT TIME-ALIGNED WITH ANYTHING ELSE. The LLCP is explicit: on the
# 600-series this output has not been through the strapdown integration and is
# not grouped with the messages coming out at the same instant. Its header is
# whatever the packet carrying it held, which may be a different moment from
# the Acceleration published alongside it -- so pairing the two by packet
# counter is pairing two different instants.
struct XbusAccelerationHr {
  header @0 :Common.XbusSampleHeader;

  accelerationXMps2 @1 :Float64;
  accelerationYMps2 @2 :Float64;
  accelerationZMps2 @3 :Float64;
}

# XDI 0x8040. The high-rate gyroscope tap, about 1600 Hz on a 600-series. The
# same alignment caveat as XbusAccelerationHr.
struct XbusRateOfTurnHr {
  header @0 :Common.XbusSampleHeader;

  rateOfTurnXRadps @1 :Float64;
  rateOfTurnYRadps @2 :Float64;
  rateOfTurnZRadps @3 :Float64;
}
