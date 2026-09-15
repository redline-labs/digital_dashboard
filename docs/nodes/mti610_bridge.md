---
title: mti610_bridge
parent: Nodes
redirect_from: /mti610.html
---

# mti610_bridge

## Overview

An Xsens MTi-610 inertial measurement unit on a serial port, bridged onto the
zenoh bus. The node is the top of three layers: [xbus](../libs/xbus.html) is
the XBus protocol, [mti610](../libs/mti610.html) is the serial port, the
reader thread and the Config/Measurement handshake, and `nodes/mti610_bridge`
maps MTData2 items onto capnp schemas and reads the YAML. The traps the
protocol sets, the gaps in the documentation and what is still waiting for
hardware are in the [design notes](../design/mti610.html).

The 610 is the IMU member of the 600-series and has no orientation filter.
There is no quaternion, no Euler angle, no rotation matrix and no free
acceleration on any topic here, because the device cannot produce them.
Orientation is a 620 (VRU), 630 (AHRS) or 670 (GNSS/INS). If you have one of
those, this stack will talk to it and decode everything it models; the node
says so at startup and puts the rest on the raw topic, but the outputs it does
not model have never been seen by this code.

## Running it

```bash
./build/nodes/mti610_bridge/mti610_bridge --config configs/mti610/mti610.yaml --probe
./build/nodes/mti610_bridge/mti610_bridge --config configs/mti610/mti610.yaml --check
./build/nodes/mti610_bridge/mti610_bridge --config configs/mti610/mti610.yaml
```

| Option | |
| --- | --- |
| `--config <file>` | The YAML below. |
| `--probe` | Identify the device, print what it is currently configured to output, leave it measuring, and exit. |
| `--check` | The same, exiting non-zero if the device does not match the config. For a health check. |
| `--replay <file>` | Replay a captured byte stream instead of opening a port. |
| `--loop` | Replay the capture repeatedly. |
| `--dump-xbus <file>` | Write every received byte to this file. |
| `--debug` | Verbose logging. |

`device` names the `port` and `baud`. Linux is `/dev/ttyUSB0` for an FTDI
adapter or `/dev/ttyACM0` for CDC-ACM; on macOS use `/dev/cu.usbserial-XXXX`.
`115200` is the factory default in serial mode. `reopen_backoff_ms` is tried
in order and then the last value repeats.

{: .warning }
On macOS open the `cu` device, not `tty`. Opening the `tty` variant blocks
waiting for carrier detect on a device that never asserts it.

The node never changes the device's baud rate. It opens the port at whatever
the YAML says and expects the device to already be there. Use MT Manager to
move a device off `115200`, then update the config.

`configuration` works as it does for the BD992. `mode: enforce` reads the
device's output configuration and writes only what differs from `outputs`;
`report_only` reads and reports and changes nothing. `port_policy: additive`
(the default) leaves outputs not listed alone and reports them; `exclusive`
turns them off. `recheck_interval_s` re-reads on a timer (`0` checks once),
and `reply_timeout_ms` and `retries` bound each exchange.

{: .important }
A re-check, and every service call, stops the data stream for as long as it
takes. The device answers configuration messages only in Config state and
emits data only in Measurement state. That is why `recheck_interval_s`
defaults to a minute and not a second.

Each `outputs` entry is a data name from the XBus data table, a `rate` in Hz
(or `max`) and an optional `precision` of `float32`, `fp1632`, `fp1220` or
`float64`. `fp1632` resolves 2^-32, finer than `float32` anywhere above 1.0,
for two extra bytes per component. The three packet-metadata entries
(`packet_counter`, `sample_time_fine`, `status_word`) take `rate: max`
because the device ignores their rate. Watch the total rate: the device
answers a configuration it cannot carry with error `0x1E`, or silently clamps
a rate and keeps going. The node warns when what comes back differs from what
was asked, and the status message carries the effective rates.
`acceleration_hr` and `rate_of_turn_hr` are deliberately not in the shipped
config: they run at about 2000 Hz and 1600 Hz, are not time-aligned with
anything else, and will not fit down a 115200-baud link alongside the rest.

`publish` sets `topic_prefix` (default `nodes/mti610`), `status_key`,
`status_interval_ms` and `publish_unknown_items`, which puts items this build
does not model on `<prefix>/mtdata2/raw`. On an MTi-610 a busy raw topic means
the device is not a 610.

## Running without hardware

There is no MTi-610 on the bench, so this is the only way to run the node end
to end:

```bash
./build/nodes/mti610_bridge/mti610_bridge --config configs/mti610/replay.yaml --replay /tmp/mti610.bin
inspect echo nodes/mti610/mtdata2/acceleration
```

`configs/mti610/replay.yaml` sets `report_only` with no `outputs`: a capture
cannot answer, so the node skips the handshake and reads. `--dump-xbus <file>`
is how a capture gets made once a device exists.

## Topics

All under `topic_prefix`. Every measurement message carries an
`XbusSampleHeader` (packet counter, sample time, status word), which is the
join key: those are properties of the packet, shared by every measurement
beside them, and without them a consumer could not say which acceleration
belongs to which instant except by arrival order. Publishers are created on
first sight; nothing is fused or batched.

| Key | Schema | XDI | Note |
|---|---|---|---|
| `nodes/mti610/mtdata2/acceleration` | `XbusAcceleration` | `0x4020` | m/s², includes gravity |
| `nodes/mti610/mtdata2/rate_of_turn` | `XbusRateOfTurn` | `0x8020` | rad/s |
| `nodes/mti610/mtdata2/magnetic_field` | `XbusMagneticField` | `0xC020` | arbitrary units |
| `nodes/mti610/mtdata2/delta_v` | `XbusDeltaV` | `0x4010` | strapdown increment |
| `nodes/mti610/mtdata2/delta_q` | `XbusDeltaQ` | `0x8030` | strapdown increment |
| `nodes/mti610/mtdata2/acceleration_hr` | `XbusAccelerationHr` | `0x4040` | ~2000 Hz, not time-aligned |
| `nodes/mti610/mtdata2/rate_of_turn_hr` | `XbusRateOfTurnHr` | `0x8040` | ~1600 Hz, not time-aligned |
| `nodes/mti610/mtdata2/baro_pressure` | `XbusBaroPressure` | `0x3010` | whole pascals |
| `nodes/mti610/mtdata2/temperature` | `XbusTemperature` | `0x0810` | the sensor's, not the ambient |
| `nodes/mti610/mtdata2/utc_time` | `XbusUtcTime` | `0x1010` | free-running; no GNSS behind it |
| `nodes/mti610/mtdata2/raw` | `XbusRawItem` | | items this build does not model |
| `nodes/mti610/status` | `Mti610Status` | | |

The five remaining identifiers a 610 emits (`0x1020` PacketCounter, `0x1060`
SampleTimeFine, `0x1070` SampleTimeCoarse, `0xE010` StatusByte, `0xE020`
StatusWord) are the packet header and ride on every message rather than
having topics of their own. `SampleTimeFine` is published raw because where it
wraps on a 600-series is undocumented.

Anything integrating motion should use `delta_v` and `delta_q` rather than
integrating `acceleration`: they are already multiplied by the sample
interval, which makes them immune to the aliasing a sampled acceleration
suffers under vibration. `temperature` reads high by whatever the device is
dissipating, and is the thing to look at when a gyro bias drifts. Do not pair
`acceleration_hr` with `acceleration` by packet counter: that pairs two
different moments.

## Services

| Key | |
| --- | --- |
| `nodes/mti610/get_output_config` | What the device is configured to emit. |
| `nodes/mti610/set_output_config` | Change it. Reads first, writes the whole list with only the difference applied. |
| `nodes/mti610/apply_config` | Re-run the pass from the node's own YAML, now. |
| `nodes/mti610/get_device_info` | What `--probe` prints: the device's identity and firmware. |

Every one of them stops the data stream for as long as it takes, for the
reason above. Services are not offered in replay mode.

## Troubleshooting

**`no goto_config_ack from the device` on every attempt.** The bytes are not
reaching a listener. Almost always the baud rate or the wrong `/dev` node; on
macOS, the `tty` device instead of `cu`.

**The port opens and nothing arrives.** On macOS you opened `/dev/tty.*`,
which blocks on carrier detect. Elsewhere, check that the device is in serial
mode at the configured rate; the node will not change the rate for you.

**Drift on every check, rewritten, and drift again a minute later.** The
device answers `0xFFFF` for the frequency of packet metadata whatever was
asked. The node normalises these before comparing; if you see this on a
non-metadata entry, the rate you asked for is one the device clamped. Lower it
or drop an output.

**Error `0x1E` from the device.** Timer overflow: the requested outputs do not
fit. Lower a rate, use `float32` instead of `fp1632`, or drop an output.

**The outputs are wrong after a power dip.** An unsolicited `WakeUp` means the
device reset, and if it is not acknowledged within 500 ms the device enters
Measurement with its stored configuration. The node answers it and re-runs the
handshake; if the status shows the old configuration, the acknowledgement was
missed.

**A busy raw topic.** The device is emitting identifiers this build does not
model. On a 610 that means the device is not a 610.

**A topic never appears.** Publishers are created on first sight, so an output
that is not in the device's configuration has no topic at all. Read
`get_output_config`.
