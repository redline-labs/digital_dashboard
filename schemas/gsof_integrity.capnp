@0xb3d47e1c6a9f0852;

using Common = import "gsof_common.capnp";

# Is the signal being trusted, and should it be.
#
# These three records answer a question the rest of the GSOF stream cannot. A
# spoofed or ionospherically disturbed solution does not look wrong: the fix
# type stays where it was, the sigmas stay small, and the position is simply
# not where the vehicle is. Every other topic in this tree reports how confident
# the receiver is; these report whether that confidence is earned.

# Which service authenticated a navigation message.
enum GsofNmaSource {
  osnma @0;
  rtxNma @1;
  qzNma @2;
  unknown @3;
}

# Where a receiver's ionospheric correction came from.
enum GsofIonoGuardSource {
  unknown @0;
  rtkBase @1;
  roverComputed @2;
  rtx @3;
  invalid @4;
}

# How disturbed the ionosphere is, in the ICD's own four steps.
enum GsofIonoGuardLevel {
  green @0;
  yellow @1;
  orange @2;
  red @3;
  unknown @4;
}

# One authentication source's verdict on the satellites it covers.
struct GsofNmaEntry {
  source @0 :GsofNmaSource;
  sourceRaw @1 :UInt8;

  # 0..10 in the ICD, naming a constellation and a signal together. Raw,
  # because the list is receiver-specific and still growing.
  signalType @2 :UInt8;

  # The PRNs the masks cover: maskBytes * 8, one-based.
  maskBytes @3 :UInt8;

  # Expanded from the bit masks, because the masks are LITTLE-ENDIAN BY BIT --
  # bit 0 of byte 0 is PRN 1 -- while every scalar in GSOF is big-endian by
  # byte. Publishing the raw masks would hand every consumer the same trap.
  authenticatedPrns @4 :List(UInt8);

  # NON-EMPTY IS THE ALARM. A satellite whose navigation message failed
  # authentication is being spoofed, or is being replayed at you. The position
  # derived from it can be perfectly self-consistent and completely false.
  failedPrns @5 :List(UInt8);
}

# GSOF 91. Navigation message authentication.
struct GsofNavMessageAuth {
  time @0 :Common.GsofGpsTime;
  count @1 :UInt8;
  entries @2 :List(GsofNmaEntry);

  # Set when any entry reports a failure, so a consumer can subscribe without
  # walking the lists.
  anyFailed @3 :Bool;
}

# One satellite's ionospheric disturbance metric.
struct GsofIonoGuardSv {
  system @0 :Common.GsofSvSystem;
  systemRaw @1 :UInt8;
  prn @2 :UInt8;
  level @3 :GsofIonoGuardLevel;
  levelRaw @4 :UInt8;
}

# GSOF 92. Per-satellite ionospheric disturbance.
#
# Scintillation degrades an RTK fix in a way nothing else in the telemetry
# shows: the fix type stays fixed and the sigmas stay small right up until the
# solution walks. A receiver with no IonoGuard source reports `invalid` and an
# empty list, which is not an error -- it is the common case.
struct GsofIonoGuardInfo {
  time @0 :Common.GsofGpsTime;

  source @1 :GsofIonoGuardSource;
  sourceRaw @2 :UInt8;

  # 0 inside, 1 outside, 255 unknown. IonoGuard corrections are only valid
  # inside the region they were computed for.
  geofenceStatus @3 :UInt8;

  stationActivityLevel @4 :GsofIonoGuardLevel;

  count @5 :UInt8;
  satellites @6 :List(GsofIonoGuardSv);
}

# GSOF 96. GsofIonoGuardInfo, counted.
#
# For a consumer that only wants "is the ionosphere quiet" and should not have
# to subscribe to a per-satellite list to find out. It carries no time of its
# own; pair it with whatever else arrived in the same transmission.
struct GsofIonoGuardSummary {
  source @0 :GsofIonoGuardSource;
  sourceRaw @1 :UInt8;
  geofenceStatus @2 :UInt8;
  stationActivityLevel @3 :GsofIonoGuardLevel;

  greenSvs @4 :UInt8;
  yellowSvs @5 :UInt8;
  orangeSvs @6 :UInt8;
  redSvs @7 :UInt8;
}
