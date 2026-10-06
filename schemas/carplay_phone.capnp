@0x87ffe4298ec56fed;

# The phone's own state, from iAP2 PowerUpdate and CommunicationsUpdate: what a
# head unit's status bar shows. Each value only when its has* flag is set --
# the phone reports what it chooses to, and a zero would read as an empty
# battery or no signal.
struct CarPlayPhone {
  # 0..100.
  batteryPercent   @0 :Float32;
  hasBattery       @1 :Bool;
  charging         @2 :Bool;
  hasCharging      @3 :Bool;
  # Bars, as the phone counts them (0..5).
  signalBars       @4 :UInt8;
  hasSignal        @5 :Bool;
  carrierName      @6 :Text;
  airplaneMode     @7 :Bool;
  hasAirplaneMode  @8 :Bool;
}
