@0x872e12aeddb4c406;

# A display module's backlight and the sensors on the module, as published by
# nodes/backlight from what the rootfs bound at boot. See docs/backlight.md.
#
# Topics hang off the node's prefix, nodes/backlight/<role> by default:
#   <prefix>/status          DisplayBacklightStatus, every poll period
#   <prefix>/set_brightness  DisplayBrightnessRequest -> DisplayBrightnessResponse

struct DisplayBacklightStatus {
  timestamp @0 :UInt64;
  # Unix time in milliseconds when this was read.

  role @1 :Text;
  # The display role, "primary" or "secondary".

  connector @2 :Text;
  # The DRM connector the display is on, e.g. "HDMI-A-2".

  backlightType @3 :Text;
  # The backlight driver the record names, e.g. "lp8863".

  writable @4 :Bool;
  # True when a kernel backlight device is there to write. False when the rootfs
  # drives the backlight through the serializer instead, or the device is missing,
  # in which case set_brightness refuses and the backlight fields are all zero.

  statusValid @5 :Bool;
  # The backlight class attributes were read this period.

  maxBrightness @6 :UInt32;
  # max_brightness, raw units.

  brightness @7 :UInt32;
  # brightness: the value last requested, raw units.

  actualBrightness @8 :UInt32;
  # actual_brightness: what the driver reports applied, raw units.

  percent @9 :Float32;
  # actualBrightness as a percentage of maxBrightness.

  blPower @10 :UInt32;
  # bl_power. 0 is on; anything else is a blanked backlight.

  faults @11 :List(UInt16);
  # The lp8863 fault register words, as the driver prints them. Empty when the
  # driver has no such attribute.

  fsmState @12 :UInt8;
  # The lp8863 state machine code, e.g. 0xd.

  fsmStateName @13 :Text;
  # The state's name as the driver prints it, e.g. "NORMAL". Empty when absent.

  ledCurrent @14 :UInt32;
  # The lp8863 LED current register.

  pwmOutput @15 :UInt32;
  # The lp8863 PWM output register.

  boost @16 :UInt32;
  # The lp8863 boost register.

  lightSensors @17 :List(DisplayLightReading);
  # One per ambient light sensor in the record, in the record's order.

  luxAverage @18 :Float32;
  # Mean of the sensors that read this period. NaN when none did.

  temperatures @19 :List(DisplayTemperatureReading);
  # Every channel of every temperature sensor in the record.
}

struct DisplayLightReading {
  path @0 :Text;
  # The IIO device directory.

  name @1 :Text;
  # The IIO device's name, e.g. "opt3001".

  ok @2 :Bool;
  # The sensor was read this period. `lux` is meaningless when false.

  lux @3 :Float32;
  # in_illuminance_input, lux.
}

struct DisplayTemperatureReading {
  path @0 :Text;
  # The hwmon device directory.

  channel @1 :Text;
  # e.g. "temp1".

  name @2 :Text;
  # The hwmon device's name, e.g. "tmp1075".

  label @3 :Text;
  # temp<n>_label, where the driver has one.

  ok @4 :Bool;
  # The channel was read this period. `celsius` is meaningless when false.

  celsius @5 :Float32;
  # Degrees Celsius.
}

struct DisplayBrightnessRequest {
  unit @0 :Unit;
  # What `value` is in. Required: `unknown` is refused.

  value @1 :Float64;
  # The brightness wanted. Clamped to the node's floor (min_percent) and to
  # max_brightness; the response says what was applied.

  enum Unit {
    unknown @0;
    percent @1;
    # 0..100 of max_brightness.
    raw @2;
    # The driver's own units, 0..max_brightness.
  }
}

struct DisplayBrightnessResponse {
  ok @0 :Bool;
  # The brightness was written.

  appliedRaw @1 :UInt32;
  # The raw value written, after clamping.

  appliedPercent @2 :Float32;
  # appliedRaw as a percentage of max_brightness.

  message @3 :Text;
  # Why it was refused, or what the clamp changed. Empty on a plain success.
}
