@0xda0b067d22b7d1b8;

# GSOF 27, the record that justifies a BD992 having two antennas.
#
# A single-antenna receiver knows where it is; two antennas a fixed distance
# apart also know which way the vehicle is POINTING, and that is different from
# the direction it is travelling. GsofVelocity's heading is a course over
# ground and is meaningless when stationary; this heading is not.
struct GsofAttitudeInfo {
  # Milliseconds into the GPS week. This record carries no week number -- pair
  # it with GsofPositionTime or GsofCurrentTimeUtc if the week matters.
  gpsTimeMs @0 :UInt32;

  attitudeFlags @1 :UInt8;
  calibrated @2 :Bool;
  pitchValid @3 :Bool;
  yawValid @4 :Bool;
  rollValid @5 :Bool;

  svsUsed @6 :UInt8;
  calculationMode @7 :UInt8;

  # Wire order is pitch, yaw, roll. Published in degrees; the wire is radians.
  #
  # Only yaw and pitch are observable on a two-antenna receiver: roll is a
  # rotation about the antenna baseline, which the baseline itself cannot see.
  # It is published because the receiver sends it, and `rollValid` is the field
  # that says whether to believe it.
  pitchDeg @8 :Float64;
  yawDeg @9 :Float64;
  rollDeg @10 :Float64;

  # The measured distance between the antennas. Compare it with the installed
  # separation: a value that has drifted means the solution is not converged.
  masterSlaveRangeM @11 :Float64;

  pdop @12 :Float32;

  # Present only in the long form of the record -- firmware 4.20 and later --
  # which is distinguished by its length alone, with no flag on the wire.
  hasVariance @13 :Bool;
  pitchVariance @14 :Float32;
  yawVariance @15 :Float32;
  rollVariance @16 :Float32;
  pitchYawCovariance @17 :Float32;
  pitchRollCovariance @18 :Float32;
  yawRollCovariance @19 :Float32;
  masterSlaveRangeVariance @20 :Float32;
}
