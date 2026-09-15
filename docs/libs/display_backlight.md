---
title: display_backlight
parent: Libraries
---

# display_backlight

## Overview

What the rootfs says about a display module, and the sysfs behind it: the
backlight class device, the IIO ambient light sensors and the hwmon
temperature sensors. The rootfs (`redline-display-setup`, in the Yocto tree)
detects the module at boot, binds the kernel drivers, and writes a record per
display role to `/run/redline/displays/<role>`. This library reads that record
and the files it points at. It does no detection of its own and never touches
the backlight enable; both belong to `redline-display`.

No zenoh and no platform code: it is file I/O, so it builds and tests
everywhere against a fake tree in a temp directory. Wiring it to the bus is
[backlight](../nodes/backlight.html).

## Public headers

| Header | |
| --- | --- |
| `display_backlight/display_record.h` | `parseIni`, `splitList`, `DisplayRecord`, `parseDisplayRecord`, and `loadDisplayRecord` with a `RecordError` that distinguishes missing from unreadable from malformed. |
| `display_backlight/sysfs.h` | `readBacklight` into `BacklightStatus`, `writeBrightness`, `readLightSensor`, `readTemperatures`, the attribute parsers, and the percent conversions. |

## Using it

Link the CMake target `display_backlight`. The node loads the record for its
role, treats a missing one as nothing to do, and reads and writes through the
paths the record names:

```cpp
auto loaded = display_backlight::loadDisplayRecord(recordDir / role);
if (!loaded && loaded.error().kind == RecordError::Kind::missing) {
    return 0;   // no module in this slot this boot
}
const auto& record = *loaded;

auto status = display_backlight::readBacklight(record.backlightDevice);
auto raw = display_backlight::percentToRaw(percent, *status.maxBrightness);
auto written = display_backlight::writeBrightness(record.backlightDevice, raw);

for (const auto& path : record.temperatureSensors)
    for (const auto& t : display_backlight::readTemperatures(path)) { /* ... */ }
```

## Behaviour worth knowing

The record only exists when a module was detected in that slot, so a missing
record is the normal state of every machine without one, not an error.
`RecordError::Kind::missing` is what lets the node exit 0 there; under
`Restart=on-failure` an error would be retried forever on a board with no
panel.

The real file ends both sensor lines with a trailing space, which a split on a
single space turns into an empty path; `splitList` tolerates it. A `#` or `;`
starts a comment only at the start of a line or after whitespace, because a
name or a path may contain either. A `[backlight]` device of `serializer`
means no kernel driver owns the backlight and the rootfs drives it through the
serializer; `backlightViaSerializer()` reports it and nothing here can write
it.

Every sysfs attribute is optional, and one that is not there is reported as
absent rather than as zero. The set modelled is what the LattePanda's
`lp8863_bl`, `opt3001` and `tmp1075` drivers expose, read off the board on
2026-09-12; another module's drivers will expose a different subset. The
lp8863 driver prints its register attributes in hex and the backlight class
prints decimal, so `parseNumber` takes both. `readTemperatures` on a directory
with no `temp<n>_input`, or one that is not there, still yields one reading
with no value, so a configured sensor that has gone missing shows up as
missing rather than vanishing.

{: .important }
`writeBrightness` is the only write in the library. `bl_power` and the
module's enable are `redline-display`'s, which sequences them against valid
video as the panel requires.

`percentToRaw` clamps to 0 to 100 and rounds to nearest. The record's `max`
and the driver's `max_brightness` can disagree; the node uses the driver's and
says so.

## Tests

```bash
ctest --test-dir build -L display_backlight    # display_backlight_test_record, display_backlight_test_sysfs
```

Both are labelled `display_backlight` and `unit`. `display_backlight_test_record`
parses, verbatim, the file a real boot wrote at
`/run/redline/displays/primary` on the LattePanda on 2026-09-12, trailing
spaces included, and checks comment stripping, a hash inside a value, junk
lines being skipped and reported, refusal of text that is not a record, and
that loading distinguishes a missing file from a broken one.
`display_backlight_test_sysfs` builds a fake tree laid out the way the
LattePanda's kernel lays out the real one, with the values it held that day,
and checks the lp8863 as the board has it, that missing attributes read as
absent, the brightness write, the light sensors, the temperatures, the
parsers and the percent mapping.
