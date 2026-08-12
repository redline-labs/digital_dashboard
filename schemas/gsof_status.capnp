@0xfb15b41a0841dea8;

using Common = import "gsof_common.capnp";

# Receiver identity, the correction source, and the L-band link.

# GSOF 15.
struct GsofReceiverSerial {
  serialNumber @0 :Int32;
}

# GSOF 35. Who is sending the corrections.
struct GsofReceivedBase {
  baseFlags @0 :UInt8;
  baseInfoValid @1 :Bool;

  # Space padded on the wire, trimmed here.
  baseName @2 :Text;
  baseId @3 :UInt16;

  latitudeDeg @4 :Float64;
  longitudeDeg @5 :Float64;
  ellipsoidHeightM @6 :Float64;
}

# GSOF 41. The same base position, with a timestamp and a quality indicator.
# Both records exist because they are produced by different parts of the
# receiver; they agree when both are enabled.
struct GsofBasePosition {
  time @0 :Common.GsofGpsTime;
  latitudeDeg @1 :Float64;
  longitudeDeg @2 :Float64;
  ellipsoidHeightM @3 :Float64;
  baseQuality @4 :UInt8;
}

# GSOF 37.
struct GsofBatteryMemory {
  batteryPercent @0 :UInt16;
  remainingMemoryHours @1 :Float64;
}

# GSOF 40. The satellite correction link -- Trimble RTX and OmniSTAR ride on
# L-band around 1.5 GHz.
struct GsofLbandStatus {
  satelliteName @0 :Text;
  nominalFrequencyMhz @1 :Float32;
  satelliteBitRate @2 :UInt16;

  # Carrier to noise. The number to watch: it falls before the correction link
  # drops, whereas correction age only moves once it already has.
  cnoDb @3 :Float32;

  hpxpSubscribedEngine @4 :UInt8;
  hpxpLibraryMode @5 :UInt8;
  vbsLibraryMode @6 :UInt8;
  beamMode @7 :UInt8;
  omniStarMotion @8 :UInt8;

  horizontalSigmaThresholdM @9 :Float32;
  verticalSigmaThresholdM @10 :Float32;
  nmeaEncryptionState @11 :UInt8;

  iqRatio @12 :Float32;
  estimatedBitErrorRate @13 :Float32;

  totalMessages @14 :UInt32;
  totalUniqueWordsWithErrors @15 :UInt32;
  totalBadUniqueWordBits @16 :UInt32;
  totalViterbiSymbols @17 :UInt32;
  correctedViterbiSymbols @18 :UInt32;
  badMessages @19 :UInt32;

  measuredFrequencyValid @20 :UInt8;
  measuredFrequencyHz @21 :Float64;
}
