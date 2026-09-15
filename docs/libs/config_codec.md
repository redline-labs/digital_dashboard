---
title: config_codec
parent: Libraries
---

# config_codec

## Limits

`config_codec::limits` holds the clamps a widget's `validate()` calls:
`clampInto`, `clampFullScale`, `orderRange`, and `clampStaleAfter` for a
binding's loss-of-comm timeout. That last one keeps zero, which means never,
and pulls anything else into 50 ms to ten minutes: under 50 ms is inside three
delivery ticks, so a gauge on a healthy stream would flicker between fresh and
stale.

## Overview

Serialization, validation and clamping for configuration built out of
`REFLECT_STRUCT` types: YAML both ways, JSON both ways plus a self-description
for tooling, a range-clamping helper, and a validator that reports problems by
field path. Nothing has to be registered per type; a nested struct or enum
declared with the reflection macros converts on its own.

All of this used to live in the dashboard's include tree. None of it was ever
about a dashboard, and the scope app needs the same behaviour for its
workspaces and panel configs, so it moved out rather than have a second
top-level app reaching into the first one's headers. It is header-only and has
no Qt: the headless config tests exercise the codecs, and Qt in their link line
would buy nothing.

## Public headers

| Header | |
| --- | --- |
| `config_codec/config_yaml.h` | `YAML::convert<>` for every reflected struct and enum, and a generated `operator==` for every reflected struct. |
| `config_codec/config_json.h` | `toJson`, `applyJson` (a partial patch with per-path errors) and `describeType` (fields, types, defaults, labels). |
| `config_codec/config_validation.h` | `Issue` and `detail::validateStruct<T>`: walks a YAML node against a struct and reports unknown keys and unacceptable values by path. |
| `config_codec/config_limits.h` | The `validate()` clamping contract and the helpers behind it: `clampInto`, `clampFullScale`, `orderRange`, `capLength`. |

## Using it

Link the CMake target `config_codec`. Any reflected struct converts through
yaml-cpp's ordinary `as<>()` and assignment, and patches from JSON through
`applyJson`, which is how the scope app applies `scope.panel_set_config`:

```cpp
#include "config_codec/config_json.h"
#include "config_codec/config_yaml.h"

auto cfg = YAML::LoadFile(path).as<my_config_t>();
YAML::Node out;
out = cfg;

std::vector<std::string> errors;
config_codec::applyJson(patch, cfg, "", errors);   // touches only the fields present
const json schema = config_codec::describeType<my_config_t>();
```

`validateStruct` lives in `config_codec::detail` and is called from each
application's own top-level validator (`validate_app_config` in
dashboard_widgets, `validate_workspace` in scope), which handle the parts of a
file that are not a plain reflected struct.

## Behaviour worth knowing

{: .warning }
No `uint8_t` in a reflected config. yaml-cpp treats `unsigned char` as a
character type: the value 14 is written as the unprintable byte 0x0E and read
back as a bad conversion, which throws out of the YAML decoder and takes the
whole layout with it. Nothing in the type says so and it fails at load time.
Use `uint16_t`.

The enum decoder is silent on failure by design: `convert<Enum>::decode`
returns false and yaml-cpp reports a bare "bad conversion" naming neither the
field nor the value. The validation pass reports the same problem with the
field's path and the valid alternatives, so run it before loading; two messages
for one typo is worse than one good one.

`applyJson` is partial by design, so an agent adjusting one colour does not
resend the whole config. Unknown keys are errors, not ignored, and the error
lists the known fields. Arrays are replaced wholesale rather than patched
element by element. An unsupported field type is a `static_assert`, so a field
cannot silently go missing from the agent interface.

`describeType` reports each field's JSON type, `cpp_type`, `default`, and the
`title` and `description` the author wrote in the struct declaration. A
`helpers::Color` is a hex string on the wire and is reported as type `color`,
so a client can show a picker.

The validator accepts colours in `#RGB`, `#RRGGBB` and `#RRGGBBAA`; an invalid
colour otherwise reaches `QColor` as invalid, paints as transparent black or
makes Qt drop a stylesheet rule, both silently. Absent keys are legitimate
everywhere below the top level and mean "use the default".

`operator==` is generated for every reflected struct through a constrained
template, comparing through the member pointers reflection already holds. That
is what lets `std::variant` over widget configs compare for free, and what the
editor uses to answer "did anything change" without serialising to YAML.

The `convert<>` specializations here are constrained partial specializations
with the same argument list as yaml-cpp's primary template, which C++20 allows
when the specialization is more constrained. A hand-written full specialization
such as `helpers::Color` or `widget_config_t` is more specialized still and
keeps winning.

`config_limits.h` defines a contract rather than a codec. A config may declare
a free function `validate(cfg)` returning `std::vector<std::string>`;
`widget_factory::createWidgetFromConfig` finds it by ADL and calls it before
constructing the widget. It clamps and reports, it does not reject: a dashboard
that comes up with one clamped gauge beats one that refuses to start on the way
to a track day. The ceilings are chosen so a typo cannot become a hang:

| Constant | Value |
| --- | --- |
| `kMaxRpmCeiling` | 30000 |
| `kMaxUpdateRateHz` | 240 |
| `kMaxMarkers` | 64 |

## Tests

This directory registers no tests of its own. The codecs are exercised, over
the whole widget table, by `dashboard_widgets_test_config_roundtrip`,
`dashboard_widgets_test_config_validation` and
`dashboard_widgets_test_config_colors` (labels `dashboard_widgets unit`), and
the YAML side again by `bag_test_metadata` (`bag unit`), whose index is a
reflected struct.
