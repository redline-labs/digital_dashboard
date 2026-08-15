@0xc5b25c1ec8d3a626;

using Common = import "gsof_common.capnp";

# One GNSS epoch: position, velocity, time and quality that belong together.
#
# THIS IS THE EXCEPTION TO gsof_common.capnp's ONE-TOPIC-PER-RECORD RULE, and
# it does not replace it -- every per-record topic still publishes exactly as
# before. That rule is right for what it was written for: a record type is a
# schema and a table row and nothing else, and nothing in the node has to decide
# which fields belong together.
#
# The problem is that for one consumer, SOMETHING has to decide, and only the
# node can. A map matcher needs position, heading and fix quality FOR THE SAME
# INSTANT: it weights a candidate road by how well the vehicle's heading agrees
# with the road's bearing, and it sizes its search by how accurate the fix is.
# Those live in records 2, 8 and 12/38 -- three topics, none of which except
# record 1 carries a time at all. A consumer left to pair them by arrival order
# gets a heading from one epoch against a position from another, and the result
# is not a dropout: it is a plausible heading, off by however far the vehicle
# turned in between, which matches the vehicle onto the frontage road beside the
# freeway and renders perfectly.
#
# The pairing is available in exactly one place. GSOF batches records into a
# transmission, and the receiver puts one epoch's records in one transmission --
# so bd992::StreamClient's transmission boundary is the only point in the system
# that knows these bytes were sent together. Downstream of the node that
# information is gone and cannot be recovered. Hence this message.
#
# FUSION IS STRICTLY WITHIN ONE TRANSMISSION. A component missing from this
# transmission is reported absent rather than filled in from the last one --
# a stale heading presented as current is the exact bug this message exists to
# prevent, and it would be invisible. If the has* flags are persistently false,
# the receiver is not configured to send those records together; that is a
# configuration fault, it is counted in the node's status, and it is fixed in
# the output configuration rather than papered over here.

struct GsofEpoch {
  # Increments by one per published epoch, from node start. The bus carries no
  # sequence number, so this is how a consumer notices it missed one -- and
  # comparing its rate against the receiver's configured output rate is how you
  # find out you are dropping transmissions.
  sequence @0 :UInt32;

  # ---- Position. Always present: an epoch without one is never published, ----
  # ---- because there is nothing for the other fields to be about.         ----

  # WGS-84, degrees. The wire carries radians; converted once, in the node.
  latitudeDeg @1 :Float64;
  longitudeDeg @2 :Float64;

  # Above the WGS-84 ELLIPSOID, not above sea level -- tens of metres of
  # difference in most of the world. Same caveat as GsofLatLongHeight.
  ellipsoidHeightM @3 :Float64;

  # ---- Time (GSOF 1) ----

  # False when record 1 was not in this transmission. Consumers that need GNSS
  # time must check it: the alternative is a week number of zero, which is
  # January 1980 and looks like a real timestamp.
  hasTime @4 :Bool;
  time @5 :Common.GsofGpsTime;

  # Satellites used in the solution. Zero when hasTime is false, since this
  # comes from record 1 too.
  svsUsed @6 :UInt8;

  # ---- Velocity (GSOF 8) ----

  hasVelocity @7 :Bool;

  # The record's own validity bit. A velocity record can arrive saying it has
  # nothing, which is NOT the same as the record being absent -- a stationary
  # vehicle with no valid heading is a real state a matcher must handle by
  # falling back on distance alone.
  velocityValid @8 :Bool;

  # Clear means differenced between consecutive positions, which is noisier and
  # lags; set means Doppler. A matcher weighting heading should care.
  dopplerDerived @9 :Bool;

  horizontalSpeedMps @10 :Float32;
  # Course over ground, degrees clockwise from true north. This is the vehicle's
  # direction of travel, NOT its attitude -- a BD992 has no IMU, and the two
  # differ whenever the vehicle is sliding or reversing.
  headingDeg @11 :Float32;
  verticalVelocityMps @12 :Float32;

  # ---- How much the position is worth (GSOF 38) ----

  hasFixType @13 :Bool;
  positionFixType @14 :Common.GsofPositionFixType;
  # What actually arrived, so a value newer than this build is reported rather
  # than flattened to `unknown`.
  positionFixTypeRaw @15 :UInt8;

  # THE bit to watch: clear is RTK float, set is RTK fixed, and between them is
  # two orders of magnitude of accuracy. A matcher tuned for centimetres that
  # silently receives metres jumps roads.
  rtkFixed @16 :Bool;

  # Seconds since the last correction. Climbing means the correction link is
  # gone and the fix is coasting -- accuracy degrades long before the fix type
  # changes.
  correctionAgeS @17 :Float32;

  # ---- How accurate it claims to be (GSOF 12) ----

  hasSigma @18 :Bool;

  # The receiver's own horizontal position RMS, in metres. This is what a
  # consumer should use to size a search radius or an emission sigma, INSTEAD OF
  # a hardcoded constant: it is the difference between the 2 cm this receiver
  # reports on a good fix and the several metres it reports when coasting, and
  # a consumer that assumes one gets the other wrong exactly when it matters.
  positionRmsM @19 :Float32;

  sigmaEastM @20 :Float32;
  sigmaNorthM @21 :Float32;
  sigmaUpM @22 :Float32;
}
