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

# GSOF 28. Receiver diagnostics: the correction link, as the receiver sees it.
#
# Eleven of its eighteen bytes are RESERVED in the ICD and read zero on real
# hardware, so what is published is the seven that are not. Two of them restate
# things other records carry -- rtkPositionAgeS is GsofPositionType's
# correctionAgeS, diffSvsInUse is GsofPositionTime's svsUsed -- and that
# redundancy is useful: they come from different parts of the receiver, so a
# disagreement is a real fault rather than a parse error.
#
# The link fields are the ones nothing else reports. A correction stream that is
# arriving but arriving late shows up here as latency and integrity long before
# it shows up anywhere else as a degraded fix.
struct GsofReceiverDiagnostics {
  baseFlags @0 :UInt8;

  # The raw byte runs 0..255, so a percentage is byte * 100 / 256 -- NOT / 100,
  # despite the ICD calling the field "link integrity 100". Both are published
  # so the trap is visible rather than inherited.
  linkIntegrity @1 :UInt8;
  linkIntegrityPercent @2 :Float32;

  # Satellites the base and rover both see, per frequency. Zero with no base.
  commonL1Svs @3 :UInt8;
  commonL2Svs @4 :UInt8;

  # Tenths of a second on the wire; seconds here.
  datalinkLatencyS @5 :Float32;

  diffSvsInUse @6 :UInt8;

  # Also tenths on the wire. Equals GsofPositionType.correctionAgeS.
  rtkPositionAgeS @7 :Float32;
}
