@0xd010449ad9609a91;

using Common = import "gsof_common.capnp";

# Position, velocity, time and quality, as a Trimble receiver reports them.
# One struct per GSOF record; see gsof_common.capnp for why.

# GSOF 1. The only record that puts time of week before the week number on the
# wire -- irrelevant here, but it is why the library struct has a comment about
# it and this one does not.
struct GsofPositionTime {
  time @0 :Common.GsofGpsTime;
  svsUsed @1 :UInt8;

  positionFlags1 @2 :UInt8;
  positionFlags2 @3 :UInt8;

  # Increments each time the RTK engine re-initialises. A value that keeps
  # climbing means the fix is not holding.
  initCounter @4 :UInt8;

  # Decoded from the two flag bytes. The raw bytes are published as well, so a
  # bit this build does not name is still on the bus.
  newPosition @5 :Bool;
  clockFix @6 :Bool;
  horizontalComputedHere @7 :Bool;
  heightComputedHere @8 :Bool;
  weightedLeastSquares @9 :Bool;
  usedL1Pseudorange @10 :Bool;
  differential @11 :Bool;
  differentialPhase @12 :Bool;
  fixedInteger @13 :Bool;
  omniStar @14 :Bool;
  staticConstrained @15 :Bool;
  networkRtk @16 :Bool;
  locationRtk @17 :Bool;
  beaconDgps @18 :Bool;
}

# GSOF 2. WGS-84. The wire carries radians; these are degrees.
struct GsofLatLongHeight {
  latitudeDeg @0 :Float64;
  longitudeDeg @1 :Float64;

  # Above the WGS-84 ELLIPSOID, not above sea level. The difference is tens of
  # metres in most of the world, so a consumer that wants altitude above mean
  # sea level needs a geoid model this node does not carry.
  ellipsoidHeightM @2 :Float64;
}

# GSOF 3.
struct GsofEcefPosition {
  xM @0 :Float64;
  yM @1 :Float64;
  zM @2 :Float64;
}

# GSOF 6. Rover minus base, in ECEF. Zero unless corrections are being received.
struct GsofEcefDelta {
  deltaXM @0 :Float64;
  deltaYM @1 :Float64;
  deltaZM @2 :Float64;
}

# GSOF 7. The same baseline as GsofEcefDelta, in the base's local tangent plane.
struct GsofTangentPlaneDelta {
  eastM @0 :Float64;
  northM @1 :Float64;
  upM @2 :Float64;
}

# GSOF 8.
struct GsofVelocity {
  velocityFlags @0 :UInt8;
  valid @1 :Bool;

  # Clear means the velocity was differenced between consecutive positions,
  # which is noisier and lags. Set means it came from Doppler.
  dopplerDerived @2 :Bool;

  horizontalSpeedMps @3 :Float32;
  headingDeg @4 :Float32;
  verticalVelocityMps @5 :Float32;

  # Present only in the long form of the record, which is distinguished purely
  # by its length -- there is no flag for it on the wire.
  hasLocalHeading @6 :Bool;
  localHeadingDeg @7 :Float32;
}

# GSOF 9.
struct GsofDopInfo {
  pdop @0 :Float32;
  hdop @1 :Float32;
  vdop @2 :Float32;
  tdop @3 :Float32;
}

# GSOF 10.
struct GsofClockInfo {
  clockFlags @0 :UInt8;
  clockOffsetValid @1 :Bool;
  frequencyOffsetValid @2 :Bool;
  anywhereFix @3 :Bool;

  receiverClockOffsetMs @4 :Float64;
  frequencyOffsetPpm @5 :Float64;
}

# GSOF 11. The ECEF variance-covariance matrix, upper triangle -- it is
# symmetric, so xy is also yx.
struct GsofPositionVcv {
  positionRms @0 :Float32;
  xx @1 :Float32;
  xy @2 :Float32;
  xz @3 :Float32;
  yy @4 :Float32;
  yz @5 :Float32;
  zz @6 :Float32;
  unitVariance @7 :Float32;
  epochCount @8 :UInt16;
}

# GSOF 12. The same uncertainty as GsofPositionVcv in the local horizontal
# plane, which is the form worth putting on a display.
struct GsofPositionSigma {
  positionRms @0 :Float32;
  sigmaEastM @1 :Float32;
  sigmaNorthM @2 :Float32;
  covarianceEastNorth @3 :Float32;
  sigmaUpM @4 :Float32;

  # The error ellipse: axes in metres, orientation in degrees from true north.
  semiMajorM @5 :Float32;
  semiMinorM @6 :Float32;
  orientationDeg @7 :Float32;

  unitVariance @8 :Float32;
  epochCount @9 :UInt16;
}

# GSOF 16.
struct GsofCurrentTimeUtc {
  time @0 :Common.GsofGpsTime;

  # GPS minus UTC in whole seconds. 18 since 2017. This is the only record that
  # carries it, which is why nothing else in these schemas converts GPS time to
  # wall-clock time.
  utcOffsetS @1 :Int16;

  timeValid @2 :Bool;
  utcOffsetValid @3 :Bool;
  timeFlags @4 :UInt8;
}

# GSOF 38. Fix quality -- the record that says how much the position is worth.
struct GsofPositionType {
  positionFixType @0 :Common.GsofPositionFixType;

  # The ICD's list has 48 entries with gaps and grows with firmware. The
  # enumeration above names what a BD992 can produce; this is what actually
  # arrived, so a value this build has never heard of is reported rather than
  # flattened to `unknown`.
  positionFixTypeRaw @1 :UInt8;

  errorScale @2 :Float32;

  solutionFlags @3 :UInt8;
  networkSolution @4 :Bool;

  # THE field to watch. Clear means RTK float, set means RTK fixed, and the
  # difference between them is two orders of magnitude of accuracy.
  rtkFixed @5 :Bool;
  initialisationIntegrity @6 :UInt8;

  rtkCondition @7 :UInt8;

  # Seconds since the last correction arrived. Climbing means the correction
  # link has gone, and the fix is coasting.
  correctionAgeS @8 :Float32;

  networkFlags @9 :UInt8;
  networkFlags2 @10 :UInt8;
  frameFlag @11 :UInt8;
  itrfEpochCentiYears @12 :Int16;
  tectonicPlate @13 :UInt8;
  rtxSubscriptionMinutesLeft @14 :Int32;
  poleWobbleStatus @15 :UInt8;
  poleWobbleDistanceM @16 :Float32;
}
