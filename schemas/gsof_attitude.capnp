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

# GSOF 74. GsofPositionSigma, for the SECOND antenna.
#
# Same fields, same units. It is how you tell "the attitude solution is bad"
# from "both antennas are bad": a heading that will not converge with clean
# sigmas here is a baseline or multipath problem, and one with sigmas as poor
# as these is simply a receiver that cannot see the sky.
#
# THE EPOCH COUNT IS OPTIONAL, and not because the ICD says so. The ICD gives
# this record 38 body bytes ending in a two-byte epoch count; the receiver on
# our bench sent 42, with a float where the count should have been. Which four
# bytes moved could not be settled, because that receiver had no second antenna
# and every field but the saturated range RMS read zero. So the count is
# reported only when the record is exactly the documented length, and
# hasEpochCount says whether it was. Publishing the bytes at the documented
# offset regardless would have reported 17027 epochs for a one-epoch fix.
struct GsofSecondAntennaSigma {
  rangeRms @0 :Float32;

  sigmaEastM @1 :Float32;
  sigmaNorthM @2 :Float32;
  covarianceEastNorth @3 :Float32;
  sigmaUpM @4 :Float32;

  semiMajorM @5 :Float32;
  semiMinorM @6 :Float32;
  orientationDeg @7 :Float32;

  unitVariance @8 :Float32;

  hasEpochCount @9 :Bool;
  epochCount @10 :UInt16;
}
