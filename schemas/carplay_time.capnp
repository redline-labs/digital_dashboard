@0xd66db679fcd70a1c;

# The phone's clock and time zone, from iAP2 DeviceTimeUpdate. The vehicle's
# GNSS gives UTC but no zone; this is where the driver's local time comes from.
struct CarPlayTime {
  # Minutes east of UTC, daylight saving already included -- do not add
  # dstOffsetMinutes to it.
  utcOffsetMinutes @0 :Int16;
  hasUtcOffset     @1 :Bool;
  # How much of utcOffsetMinutes is daylight saving.
  dstOffsetMinutes @2 :Int8;
  # The phone's wall clock, Unix seconds, as it last reported it. Not a time
  # source: the phone's clock is the user's to set.
  unixSeconds      @3 :Int64;
  hasUnixSeconds   @4 :Bool;
}
