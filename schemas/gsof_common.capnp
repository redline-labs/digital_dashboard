@0x8a2f64015fd3562d;

# Types shared by the GSOF schemas.
#
# The GSOF records a Trimble receiver emits are published one topic per record
# type, mirroring the ICD exactly. That means a consumer wanting position and
# fix quality subscribes to two topics -- which is the trade made deliberately:
# a new record type is then a schema and a table row and nothing else, and
# nothing in the node has to decide which fields belong together.
#
# UNITS ARE IN THE FIELD NAMES, and they are NOT always the wire's. The GSOF
# records carry latitude in radians (records 2, 27, 41) or degrees (record 49)
# depending on which record you are reading; everything published here is
# degrees, converted once in the node. The library structs keep the wire units
# so they still describe the bytes -- see libs/gsof/include/gsof/records.h.

# Which constellation a satellite belongs to. `unknown` is first and the raw
# byte travels alongside, because the ICD reserves 6..255 and a receiver newer
# than this build will use them.
enum GsofSvSystem {
  unknown @0;
  gps @1;
  sbas @2;
  glonass @3;
  galileo @4;
  qzss @5;
  beidou @6;
}

# How the position was computed: the ICD's list entire, with its own gaps (34,
# 35, 45, 46, 47 are RESERVED there).
#
# This was once the subset "a BD992 in a vehicle can produce". That was a guess,
# and the first live receiver disproved it inside an hour by reporting
# rtxFastLowLatency. `positionFixTypeRaw` meant nothing was lost, but a consumer
# switching on this enum saw `unknown` for an ordinary RTX fix.
#
# THE ORDINALS ARE APPEND-ONLY and are not the wire values -- `unknown` is 0 so
# it is the default, which shifts everything by one. Anything new goes on the
# end regardless of its ICD number, or already-published data changes meaning.
enum GsofPositionFixType {
  unknown @0;
  noFixOrOld @1;
  autonomous @2;
  propagatedAutonomous @3;
  differentialSbas @4;
  propagatedSbas @5;
  differential @6;
  propagatedDifferential @7;
  floatRtk @8;
  propagatedFloatRtk @9;
  fixedRtk @10;
  propagatedFixedRtk @11;
  omniStarHp @12;
  omniStarXp @13;
  locationRtk @14;
  omniStarVbs @15;
  beaconDifferential @16;
  rtxCodePhase @17;
  xFillRtx @18;
  omniStarHpXp @19;
  omniStarHpG2 @20;
  omniStarG2 @21;
  synchronousRtx @22;
  lowLatencyRtx @23;
  omniStarMultipleSource @24;
  omniStarL1Only @25;
  insAutonomous @26;
  insSbas @27;
  insCodePhaseDgnss @28;
  insRtxCodePhase @29;
  insRtxCarrierPhase @30;
  insOmniStar @31;
  insRtk @32;
  insDeadReckoning @33;
  rtxFastSync @34;
  rtxFastLowLatency @35;
  lowLatencyRtxRangePoint @36;
  synchronousRtxRangePoint @37;
  lowLatencyRtxViewPoint @38;
  synchronousRtxViewPoint @39;
  lowLatencyRtxFieldPoint @40;
  synchronousRtxFieldPoint @41;
  omniStarG2Plus @42;
  omniStarG4Plus @43;
  l1sSlas @44;
  insXFillRtx @45;
  clas @46;
  insClas @47;
  has @48;
  insHas @49;
}

# GPS week plus milliseconds into it, as most records carry time.
#
# Not converted to Unix time here. That conversion needs the leap-second offset,
# which only record 16 carries, and a node that guessed it would publish a
# timestamp that is wrong by 18 seconds and looks right. Consumers that need
# wall-clock time have the zenoh sample stamp; consumers that need GNSS time
# have this.
struct GsofGpsTime {
  week @0 :UInt16;
  timeOfWeekMs @1 :UInt32;
}

# One satellite, as reported by the brief list (record 33).
struct GsofSvBrief {
  prn @0 :UInt8;
  system @1 :GsofSvSystem;
  systemRaw @2 :UInt8;

  flags1 @3 :UInt8;
  flags2 @4 :UInt8;

  # Decoded from flags1, because these are what a display actually wants and
  # the bit positions are not obvious from the byte.
  aboveHorizon @5 :Bool;
  usedInPosition @6 :Bool;
  usedInRtk @7 :Bool;
}

# One satellite, as reported by the detailed list (record 34).
struct GsofSvDetail {
  prn @0 :UInt8;
  system @1 :GsofSvSystem;
  systemRaw @2 :UInt8;

  flags1 @3 :UInt8;
  flags2 @4 :UInt8;

  aboveHorizon @5 :Bool;
  usedInPosition @6 :Bool;
  usedInRtk @7 :Bool;

  elevationDeg @8 :UInt8;
  azimuthDeg @9 :UInt16;

  # The wire carries these as dB times four. Published as dB.
  snrFirstDb @10 :Float32;
  snrSecondDb @11 :Float32;
  snrThirdDb @12 :Float32;
}
