---
title: helpers
parent: Libraries
---

# helpers

## Overview

Small, Qt-free value types and functions that more than one library needed: a
colour as a hex string, a CAN frame, hex text both ways, a rate gate, and unit
conversions. Header-only; its only link dependency is yaml-cpp, for the
`YAML::convert<helpers::Color>` specialization. Its Qt-using sibling is
[qt_helpers](qt_helpers.html), which turns a `Color` into a `QColor` and is
kept apart so that nothing here drags Qt into a node.

A second target, `helpers_ffmpeg`, routes ffmpeg's own logging into spdlog. It
is separate because it has to link libavutil and almost nothing that uses
`helpers` has anything to do with ffmpeg.

## Public headers

| Header | |
| --- | --- |
| `helpers/color.h` | `Color`: a hex-string wrapper with `isValidFormat()`, plus its YAML conversion. |
| `helpers/can_frame.h` | `CanFrame`: one classic or FD frame with the flags that make it unambiguous. |
| `helpers/hex.h` | `toHex()` and `fromHex()`, the one hex codec. |
| `helpers/rate_gate.h` | `RateGate`: minimum spacing between events, with the clock supplied by the caller. |
| `helpers/unit_conversion.h` | `constexpr` conversions: degrees to radians, kph/mph/mps, psi/bar, Celsius/Fahrenheit. Global namespace. |
| `helpers/ffmpeg_log.h` | `routeFfmpegLogsToSpdlog()`. Target `helpers_ffmpeg`. |

## Using it

Link the CMake target `helpers`, or `helpers_ffmpeg` for the log bridge.

```cpp
#include "helpers/hex.h"
#include "helpers/rate_gate.h"

std::string error;
const auto bytes = helpers::fromHex("01 ff:7a", &error);   // nullopt, with a reason, on bad input

helpers::RateGate gate(std::chrono::milliseconds(16));
const auto now = helpers::RateGate::clock::now();
if (gate.ready(now)) { send(); gate.mark(now); }
```

## Behaviour worth knowing

`fromHex` never skips or pads. It rejects a character that is not a hex digit
or an allowed separator, an odd number of digits, and a separator inside a byte
such as `"0 1"`. Whitespace and `:` are allowed between bytes and a leading
`0x` is accepted. The tree had grown six copies of this, and the ones that
skipped a bad digit or dropped a trailing nibble could make a test fixture
decode to bytes nobody wrote down while the test still passed.

`Color` stores the text as given and defaults to `#000000`. `isValidFormat()`
accepts `#RGB`, `#RRGGBB` and `#RRGGBBAA` and nothing else, because anything
else reaches `QColor` as invalid and paints as transparent black, silently. It
converts implicitly to `std::string` for older callers. Qt reads the eight-digit
form as `#AARRGGBB`, the opposite channel order; `qt_helpers::toQColor` is the
one place that disagreement is resolved.

`CanFrame::len` is a real byte count, not a DLC code. `isExtended` is what
distinguishes an 11-bit `0x123` from a 29-bit `0x123`, which are different
messages on the same bus; `isError` marks a controller report that is not a
message at all and should send you to the channel statistics rather than a
decoder. `timestampUs` of zero means the backend supplied none.

`RateGate` owns no clock, timer or thread, which is what makes every user of it
testable. "Never sent" is explicit state rather than a default-constructed
timestamp, because a `time_point{}` sits at the clock epoch and would
rate-limit the first event against time zero under a reset or a test clock.
`mark()` is separate from `ready()` on purpose: a caller that asks and then
decides not to send must not move the gate.

`routeFfmpegLogsToSpdlog()` is idempotent; call it once at startup before
opening a codec. ffmpeg's `AV_LOG_INFO` is mapped to debug, because that is
where codecs put per-run statistics; warnings and errors keep their severity.
The `HELPERS_WITH_FFMPEG` option defaults to ON, and OFF is a consumer saying
it has no ffmpeg, not a fallback taken when the probe fails: guessing is how a
build silently drops video logging.

## Tests

| Target | Labels | Proves |
| --- | --- | --- |
| `helpers_test_hex` | `helpers unit` | The rejections in `fromHex`: a bad digit, an odd nibble and a split byte all return `nullopt` with a reason rather than a shorter byte string. |
