@0x820e62d30122c618;

# Shared by every MTData2 measurement an MTi-610 publishes.

# The packet metadata every measurement in a sample carries with it.
#
# THIS IS THE ONE PLACE THIS NODE DEPARTS FROM THE BD992'S TOPIC MODEL, and it
# is worth saying why. A GSOF record carries its own gpsTimeMs, so publishing
# one record per topic with no shared context loses nothing. An MTData2 item
# does not: the packet counter, the sample time and the status word are
# properties of the PACKET, shared by every measurement beside them. Publishing
# them on their own topics would leave a consumer unable to say which
# acceleration belongs to which timestamp except by arrival order -- which is
# the pair-by-arrival guess that libs/map_match already has to make for GNSS,
# and there it is unavoidable. Here it is not.
#
# So this rides on every measurement message instead. It is the join key.
#
# Each part is optional because each is a configurable output: a device asked
# for acceleration and nothing else sends acceleration and nothing else. The
# `has` flags say which of them the packet actually held, rather than letting a
# consumer read an absent counter as zero.
struct XbusSampleHeader {
  # Increments once per packet and WRAPS AT 65536 -- about eleven minutes at
  # 100 Hz. Published raw; unwrapping is a consumer's decision because it needs
  # a memory of what came before.
  hasPacketCounter @0 :Bool;
  packetCounter @1 :UInt16;

  # The sample instant in 10 kHz ticks.
  #
  # WHERE THIS WRAPS ON A 600-SERIES IS UNDOCUMENTED. The LLCP gives the
  # 1-series 0xFFFFFFFF and the 10/100-series exactly one day, and says nothing
  # about the 600s. Published raw for that reason -- a seconds field computed
  # here would bake in whichever assumption turned out to be wrong.
  hasSampleTimeFine @2 :Bool;
  sampleTimeFineTicks @3 :UInt32;

  hasSampleTimeCoarse @4 :Bool;
  sampleTimeCoarseS @5 :UInt32;

  # The status word, raw and decoded. The raw value is published alongside the
  # bits because Xsens reserves bits 27..31 and firmware adds to them: a
  # consumer that only ever saw our decode could not tell a new bit from a
  # clear one.
  hasStatus @6 :Bool;
  statusWord @7 :UInt32;

  selfTestPassed @8 :Bool;
  filterValid @9 :Bool;

  # Bit 19: set when any individual clip bit is. The one bit worth watching,
  # because a sensor driven out of range reports values that look entirely
  # reasonable.
  clipping @10 :Bool;

  clipAccelerationX @11 :Bool;
  clipAccelerationY @12 :Bool;
  clipAccelerationZ @13 :Bool;
  clipGyroscopeX @14 :Bool;
  clipGyroscopeY @15 :Bool;
  clipGyroscopeZ @16 :Bool;
  clipMagnetometerX @17 :Bool;
  clipMagnetometerY @18 :Bool;
  clipMagnetometerZ @19 :Bool;

  syncInMarker @20 :Bool;
  syncOutMarker @21 :Bool;

  # Bits 3:4 of the status word, raw. The no-rotation procedure's state: 3 is
  # running, 2 is rotation detected, 0 is complete. Only meaningful after a
  # SetNoRotation, which this node does not send.
  noRotationStatus @22 :UInt8;

  # How the device encoded this measurement on the wire, as the low two bits of
  # its data identifier. Published because it is the difference between a
  # reading good to 2^-32 and one good to a float32's seven digits, and nothing
  # else in the message says which arrived.
  precision @23 :XbusPrecision;
}

# LLCP Table 18's precision field. Ordinal 0 is unknown, per the convention in
# this directory: the ordinals here are NOT the wire values.
enum XbusPrecision {
  unknown @0;
  float32 @1;
  fp1220 @2;
  fp1632 @3;
  float64 @4;
}
