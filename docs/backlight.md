# Display backlight node

`nodes/backlight` puts a display module's backlight and the sensors on the
module onto the bus:

- **status out:** brightness, the driver's own state, ambient light and
  temperatures;
- **brightness in:** a single service that sets it.

All of the file reading lives in `libs/display_backlight`. The node adds the
config, a lock and the schema translation.

It deliberately does **not**:

- **Turn the backlight on or off.** The panel needs valid video before LED_EN,
  and `redline-display-backlight.service` handles that ordering. This node
  writes `brightness` and nothing else.
- **Dim automatically.** The lux-to-brightness curve is still a product
  decision. The sensors are published so that whatever makes that decision can
  read them. `redline-display monitor` has a placeholder curve. It ships disabled
  and must stay disabled while anything calls `set_brightness`, or the two will
  fight over the value.
- **Detect anything.** The rootfs does detection at boot.

## Inputs

The node reads the record `redline-display-setup` writes for its role, at
`/run/redline/displays/<role>`. As a real boot on the LattePanda wrote it:

```ini
[display]
role=primary
name=Rivian Gen1 IC, LG LA123WF9-SL07, 1920x720
connector=HDMI-A-2
[backlight]
type=lp8863
device=/sys/class/backlight/lp8863
max=65535
[sensors]
ambient_light=/sys/bus/iio/devices/iio:device0 /sys/bus/iio/devices/iio:device1
temperature=/sys/class/hwmon/hwmon1 /sys/class/hwmon/hwmon2
```

What it reads from each of those, with the values seen on the board on
2026-09-12:

| Device | Attributes |
|---|---|
| backlight class (`lp8863_bl`) | `brightness` 24576, `actual_brightness` 24576, `max_brightness` 65535, `bl_power` 0, `type` raw |
| lp8863 driver (`<device>/device/`) | `faults` `0x0000 0x0000 0x0800`, `fsm_state` `0xd NORMAL`, `led_current` `0x0fff`, `pwm_output` `0x6000`, `boost` `0x0549` |
| IIO (`opt3001` ×2) | `name`, `in_illuminance_input` 3.42 / 2.12 lux |
| hwmon (`tmp1075` ×2) | `name`, `temp1_input` 45187 / 44500 m°C (no label) |

Every attribute is optional. A different module's drivers will expose a
different subset, and a missing attribute is reported as absent, never as zero.

**No record** means no module was detected in that slot this boot. That is the
normal state of any machine without one, so the node logs one line and exits 0,
and `Restart=on-failure` does not loop.

**`device=serializer`** means no kernel driver owns the backlight and the rootfs
drives it through the serializer. In that case the node still publishes the
sensors, sets `writable=false`, and refuses `set_brightness`.

## Topics

The schema is `schemas/display_backlight.capnp`, and the prefix defaults to
`nodes/backlight/<role>`.

| Key | Schema | |
|---|---|---|
| `<prefix>/status` | `DisplayBacklightStatus` | every `poll_ms` |
| `<prefix>/set_brightness` | `DisplayBrightnessRequest` → `DisplayBrightnessResponse` | service |

```sh
inspect echo nodes/backlight/primary/status -n 1
inspect call nodes/backlight/primary/set_brightness -d '{"unit":"percent","value":20}'
inspect call nodes/backlight/primary/set_brightness -d '{"unit":"raw","value":13107}'
```

A request is clamped to between `min_percent` of `max_brightness` and
`max_brightness`. The response gives the raw value actually applied and, when the
clamp changed it, says so. Clamping up to `min_percent` stops a slider or a remote
from turning the panel off by accident, since a backlight at zero looks exactly
like a dead display.

## Config

`configs/backlight/backlight.yaml`:

```yaml
role: primary                     # names the record file
record_dir: /run/redline/displays # point at a fake tree to run off the target
# topic_prefix: nodes/backlight/primary
poll_ms: 500
min_percent: 1
```

## On the target

The node runs as an instance of the `redline-node@.service` template:

- the binary is `/opt/redline/bin/backlight`;
- its arguments come from `/opt/redline/nodes/backlight.args`, or from
  `/data/nodes/backlight.args`, which takes precedence:

```sh
REDLINE_NODE_ARGS=--config /opt/redline/configs/backlight/backlight.yaml
```

The unit runs as root, which is what writing `brightness` requires.

## Running it without the hardware

`display_backlight_test_record` and `display_backlight_test_sysfs` build a fake
`/sys` tree under a temp directory, matching the board. To run the whole node the
same way, create a directory tree with the attribute files above, write a record
whose paths point into it, and set `record_dir` to the directory holding that
record. `inspect echo` then shows the status, and `set_brightness` rewrites the
fake `brightness` file.

## Not done yet

- **`clear_faults` is not wired.** The driver exposes it as a write-only
  attribute, and what value it expects has not been checked against the
  `lp8863_bl` source.
- **Nothing sets brightness automatically.** See above.
- ~~**The Yocto side** needs to install the node and its args file and enable
  `redline-node@backlight`.~~ Done 2026-09-12: `redline-nodes` ships
  `backlight.args` and a drop-in ordering the instance after
  `redline-display-setup` and `redline-display-backlight`; the lattepanda-mu
  machine enables it (`REDLINE_ENABLED_NODES`).
