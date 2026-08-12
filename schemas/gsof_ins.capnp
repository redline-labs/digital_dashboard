@0x979032fdb16f79a4;

using Common = import "gsof_common.capnp";

# GSOF 49 and 50, the inertial navigation records.
#
# A BD992 DOES NOT PRODUCE THESE -- it has no IMU. They are here because the
# records cost one table row each in libs/gsof, because captures of them exist
# and are used as golden vectors, and because an Applanix unit on the same bus
# would publish them without any further work. If nothing on the vehicle has an
# IMU, these topics simply never appear.

struct GsofInsFullNav {
  time @0 :Common.GsofGpsTime;

  imuAlignmentStatus @1 :UInt8;
  gnssStatus @2 :UInt8;

  # Degrees on the wire in this record, unlike GSOF 2 and 41 which are radians.
  latitudeDeg @3 :Float64;
  longitudeDeg @4 :Float64;
  altitudeM @5 :Float64;

  velocityNorthMps @6 :Float32;
  velocityEastMps @7 :Float32;
  velocityDownMps @8 :Float32;
  totalSpeedMps @9 :Float32;

  rollDeg @10 :Float64;
  pitchDeg @11 :Float64;
  headingDeg @12 :Float64;

  # Direction of travel, as opposed to the direction the vehicle points.
  trackAngleDeg @13 :Float64;

  angularRateRollDps @14 :Float32;
  angularRatePitchDps @15 :Float32;
  angularRateHeadingDps @16 :Float32;

  accelerationXMps2 @17 :Float32;
  accelerationYMps2 @18 :Float32;
  accelerationZMps2 @19 :Float32;
}

struct GsofInsRms {
  time @0 :Common.GsofGpsTime;

  imuAlignmentStatus @1 :UInt8;
  gnssStatus @2 :UInt8;

  positionRmsNorthM @3 :Float32;
  positionRmsEastM @4 :Float32;
  positionRmsDownM @5 :Float32;

  velocityRmsNorthMps @6 :Float32;
  velocityRmsEastMps @7 :Float32;
  velocityRmsDownMps @8 :Float32;

  rollRmsDeg @9 :Float32;
  pitchRmsDeg @10 :Float32;
  headingRmsDeg @11 :Float32;
}
