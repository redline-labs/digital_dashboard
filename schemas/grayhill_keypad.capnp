@0xe6c9d9c6e1b4d5aa;

# What the keypad reports, and what can be asked of it.
#
# The brightness requests used to carry one channel each, and the node sent a
# frame with the other channel zeroed -- so setting the backlight blanked the
# indicators and vice versa. RPDO2 carries both channels in one frame and there
# is no way to send half of it, so the fix is for the request to say what should
# happen to the channel it is not about. `keep` is the default, which makes the
# old single-channel usage do the obvious thing.

struct GrayhillButtons {
  # Buttons 1..24, one bit each, exactly as TPDO1 carries them. Bit 0 of
  # buttons1To8 is button 1.
  buttons1To8 @0 :UInt8;
  buttons9To16 @1 :UInt8;
  buttons17To24 @2 :UInt8;
}

struct GrayhillStatus {
  # The NMT state the keypad last reported in a heartbeat, or `unknown` if it
  # has not sent one. A keypad configured with heartbeat disabled -- which is
  # what MoTeC's configuration does -- stays `unknown` and that is not a fault.
  state @0 :State;
  # How many boot-up frames have been seen. A number that climbs while the
  # vehicle is running means the keypad is resetting.
  bootCount @1 :UInt32;
  # The last emergency the keypad raised, 0 if none.
  lastEmergencyCode @2 :UInt16;

  enum State {
    unknown @0;
    bootUp @1;
    stopped @2;
    operational @3;
    preOperational @4;
  }
}

# 0x6411:01, the indicator LED brightness. Range 1..255: zero is outside what
# the device accepts, which is why the old "write 0 to the channel I am not
# setting" behaviour was wrong twice over.
struct GrayhillSetIndicatorBrightnessRequest {
  value @0 :UInt16;
  # What to do with the backlight channel, which shares the frame.
  backlight @1 :OtherChannel = keep;
}

struct GrayhillSetIndicatorBrightnessResponse {
  ok @0 :Bool;
  # Empty when ok. Otherwise says what the device or the node objected to.
  error @1 :Text;
}

# 0x6411:02, the backlight brightness. Range 0..255.
struct GrayhillSetBacklightBrightnessRequest {
  value @0 :UInt16;
  indicator @1 :OtherChannel = keep;
}

struct GrayhillSetBacklightBrightnessResponse {
  ok @0 :Bool;
  error @1 :Text;
}

# What happens to the channel a request is not about.
enum OtherChannel {
  # Leave it at whatever it was last set to. The sensible default, and the one
  # that makes a single-channel request behave the way its name suggests.
  keep @0;
  # Set it to zero. Only meaningful for the backlight -- zero indicator
  # brightness is outside the device's range.
  zero @1;
}

# 0x6200 over RPDO1: the indicator LEDs.
#
# Three indicators per button, always addressed whether the LED is physically
# fitted or not. Within a byte the least significant bit is the top-left
# indicator, then right to left, then top to bottom.
struct GrayhillSetIndicatorsRequest {
  # Up to eight bytes, indicators 1..64. Fewer than eight leaves the rest off.
  indicators @0 :Data;
}

struct GrayhillSetIndicatorsResponse {
  ok @0 :Bool;
  error @1 :Text;
}
