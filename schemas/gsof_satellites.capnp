@0xdfad6f652169e0cc;

using Common = import "gsof_common.capnp";

# The satellites in view, briefly (GSOF 33) and in detail (GSOF 34).
#
# Both lists are bounded by the record's one-byte length field: (255-1)/4 for
# the brief form and (255-1)/10 for the detailed one. Those are the caps the
# library enforces, and they are why neither list is annotated with a fixed
# length here -- the count varies with the sky, not with the configuration.

struct GsofAllSvBrief {
  count @0 :UInt8;
  satellites @1 :List(Common.GsofSvBrief);
}

struct GsofAllSvDetailed {
  count @0 :UInt8;
  satellites @1 :List(Common.GsofSvDetail);
}

# One satellite as the GPS-only lists (records 13 and 14) report it.
#
# Separate from Common.GsofSvBrief/GsofSvDetail because there is no SV SYSTEM
# byte -- every entry is a GPS or SBAS PRN -- and because the detailed form
# carries two signal-to-noise figures where record 34 carries three. Folding
# them together would mean a system field that is always `gps` and a third SNR
# that is always zero, both of which read as data.
struct GsofGpsSvBrief {
  prn @0 :UInt8;

  flags1 @1 :UInt8;
  flags2 @2 :UInt8;

  aboveHorizon @3 :Bool;
  usedInPosition @4 :Bool;
  usedInRtk @5 :Bool;
}

struct GsofGpsSvDetail {
  prn @0 :UInt8;

  flags1 @1 :UInt8;
  flags2 @2 :UInt8;

  aboveHorizon @3 :Bool;
  usedInPosition @4 :Bool;
  usedInRtk @5 :Bool;

  elevationDeg @6 :UInt8;
  azimuthDeg @7 :UInt16;

  # dB times four on the wire, dB here. These label DIFFERENT signals from
  # GsofSvDetail's -- do not compare the second one across the two records.
  snrFirstDb @8 :Float32;
  snrSecondDb @9 :Float32;
}

# GSOF 13. The GPS-only brief list.
#
# Not redundant with GsofAllSvBrief even when both are enabled: on a live
# receiver this one listed a satellite record 33 omitted -- above the horizon
# and assigned a channel, but not yet tracked.
struct GsofSvBriefInfo {
  count @0 :UInt8;
  satellites @1 :List(GsofGpsSvBrief);
}

# GSOF 14. The GPS-only detailed list.
struct GsofSvDetailInfo {
  count @0 :UInt8;
  satellites @1 :List(GsofGpsSvDetail);
}

# GSOF 48. The way past GsofAllSvDetailed's ceiling.
#
# Record 34 carries a count and 10 bytes per satellite in a body the length byte
# caps at 255, so it stops at 25 satellites and silently truncates the rest. A
# BD992 tracking five constellations routinely sees more: on the bench, 31
# entries here against record 34's 24.
#
# ONE MESSAGE ARRIVES AS SEVERAL OF THESE. The pages are separate, complete GSOF
# records that all land in the same transmission, so a consumer sees page 1 of 2
# and page 2 of 2 as two samples on this topic and has to join them itself. That
# is why the page numbers are published rather than hidden.
#
# AN ENTRY IS A SIGNAL GROUP, NOT A SATELLITE. The same PRN can appear on more
# than one page with identical elevation and azimuth but a different SNR triple.
# Concatenate the pages; a consumer that keys a map by PRN keeps whichever entry
# it saw last and loses the other.
struct GsofAllSvDetailedPage {
  version @0 :UInt8;

  # Both one-based: page 1 of 2, then page 2 of 2.
  pageNumber @1 :UInt8;
  totalPages @2 :UInt8;
  lastPage @3 :Bool;

  count @4 :UInt8;
  satellites @5 :List(Common.GsofSvDetail);
}
