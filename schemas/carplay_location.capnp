@0x975c5a878a4b2c9e;

# A GPS fix the head unit provides to the phone, so CarPlay can dead-reckon
# where its own signal is weak. Published by any GPS source; the CarPlay driver
# subscribes and uplinks it as NMEA over iAP2 when the phone requests location.
struct CarPlayLocation {
  latitudeDeg   @0 :Float64;  # positive north
  longitudeDeg  @1 :Float64;  # positive east
  altitudeM     @2 :Float64;  # metres above mean sea level
  speedKnots    @3 :Float64;  # speed over ground
  courseDeg     @4 :Float64;  # true course over ground, 0..360
  satellites    @5 :UInt32;   # satellites used in the fix
  hdop          @6 :Float64;  # horizontal dilution of precision
  utcEpochMs    @7 :UInt64;   # fix time; 0 = use current wall clock
  valid         @8 :Bool;     # false emits a "no fix" sentence
}
