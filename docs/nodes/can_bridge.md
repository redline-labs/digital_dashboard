---
title: can_bridge
parent: Nodes
---

# can_bridge

## Overview

CAN hardware on one side, zenoh topics on the other. `can_bridge` opens every
channel listed in its config, publishes the frames each receives, and puts on
the wire whatever anything publishes to that channel's transmit topic. It is
the one node that owns CAN adapters; every decoder node in this tree
(`megasquirt`, `motec_m1`, `motec_pdm`, `msel_master_relay`, and the rest)
subscribes to the topics this node publishes rather than opening hardware
itself.

Four backends are behind one channel registry, so a channel is named by a
string and swapping hardware changes one line: `pcan:` for PEAK PCAN-USB FD
adapters over libusb, `socketcan:` for the kernel's CAN stack on Linux,
`motec:` for the MoTeC UTC and MoTeC's network gateways
([can_motec](../libs/can_motec.html)), and `trc:` for a recorded PCAN trace
replayed as if it were a bus. A `virtual:` loopback exists for running the
node with no hardware. Any channel can also be tapped into a `.trc` file with
`record_trc`, which is how a bus gets recorded for PCAN-Explorer or for later
replay. The reverse-engineered UTC protocol and its measurements are in the
[MoTeC UTC protocol](../design/motec-utc-protocol.html) design note.

One process opens every channel because a PCAN-USB Pro FD is a single USB
handle serving two CAN channels, and two processes cannot share it. A channel
that fails to open does not take the others down.

## Running it

```bash
./build/nodes/can_bridge/can_bridge --list                                   # what can this machine see?
./build/nodes/can_bridge/can_bridge --config configs/can_bridge/can_bridge.yaml
./build/nodes/can_bridge/can_bridge --config configs/can_bridge/replay.yaml   # a trace, no hardware
```

| Option | |
| --- | --- |
| `--config <file>` | The node configuration YAML. |
| `-l`, `--list` | List the CAN channels this machine can see, in the exact `device:` form the config takes, then exit. `virtual:` and `trc:` are not listed because they exist only once something names one. |
| `--set-bitrate <channel>=<nominal>[:<data>]` | Ask a running bridge to change a channel's bit rate, then exit. |
| `--service-key <key>` | Which bridge to ask, when `--set-bitrate` is used. Default `vehicle/can/set_bitrate`. |
| `-v`, `--verbose` | Debug logging. |

`configs/can_bridge/can_bridge.yaml` ships with a `virtual:bench` channel so
it runs anywhere; edit `device` to point at real hardware.

## Configuration

`channels` is a list, one entry per bus. Each has a `name`, which is what the
topics and the bit-rate service call this bus and is deliberately separate
from the hardware, and a `device`:

```
socketcan:can0        the kernel interface named can0            (Linux only)
pcan:0                the first PCAN-USB FD adapter, channel 0
pcan:0/1              that same adapter, channel 1
pcan:LSN00123/1       the adapter with that serial, channel 1
motec:0               the first MoTeC UTC dongle
motec:serial=56536    the UTC with that serial
motec:udp=host        a MoTeC network gateway, same protocol over UDP
virtual:bench         an in-process loopback bus, no hardware
trc:/logs/run.trc     a recorded PCAN trace, replayed at its own timing
trc:/logs/run.trc/2   bus 2 of a trace that recorded several
```

A UTC's serial is a bare number, so `motec:56536` is tried as an index first
and as a serial second; `serial=` forces the serial reading.

| Key | Default | |
| --- | --- | --- |
| `bitrate` | `500000` | Arbitration-phase bit rate. Every node on a bus must agree. Required even for `trc:`, where it is remembered for a recorder's header. |
| `data_bitrate` | `0` | CAN FD data-phase rate. `0` is classic CAN. Setting it asks the adapter for FD, and one that cannot refuses at open. |
| `sample_point_permille`, `data_sample_point_permille` | CiA default | Where in the bit to sample. Override only to match a bus that is not at the default. |
| `listen_only` | `false` | Receive but never transmit: no acknowledgements, no error frames. The safe way to attach to a bus you do not own. Not supported by `motec:`. |
| `rx_key`, `tx_key` | `vehicle/<name>/rx`, `vehicle/<name>/tx` | The topics. |
| `rx_queue_depth` | `8192` | Frames that may back up before the oldest are dropped. Drops are counted on the status topic. Do not shrink it for a `motec:` channel. |
| `publish_rx` | `true` | `false` for a channel that exists only to transmit. |
| `accept_tx` | `true` | `false` is stronger than `listen_only`: nothing can even ask. |
| `record_trc` | unset | Write everything this channel sees, received and transmitted, to a PCAN `.trc` file at this path. Truncated at startup. |
| `record_trc_bus` | `1` | The trace's Bus column, `1` to `16`, so two recorded channels stay apart. |

{: .warning }
A `motec:` channel ignores `bitrate`. The UTC runs at whatever MoTeC's own
tool last configured it for, and the protocol has no command to change or read
it. The node warns at startup, and a mismatch presents as a bus that carries
nothing with no error anywhere.

The trace recorder runs on its own thread, so neither the receive loop nor a
zenoh callback ever waits on the disk. If it cannot keep up it drops frames
and counts them on the status topic under `recordDropped`. Recording works
with `listen_only: true`; nothing about recording puts a frame on the wire.
It is a key on the channel rather than a `trc:` device because a device only
ever sees what this node transmits, and a trace of a bus you are observing
has to contain what you observed.

The node-level keys are `status_key` (`vehicle/can/status`),
`set_bitrate_key` (`vehicle/can/set_bitrate`), `status_interval_ms` (`1000`),
`pcan_detach_kernel_driver` (Linux only; takes a PCAN adapter away from the
in-tree `peak_usb` driver and removes the socketcan interface it created),
and `continue_on_channel_error` (`true`). How every `trc:` channel replays is
node-level too, because one TRC backend serves all of them:
`trc_replay_speed` (`1.0`), `trc_replay_paced` (`true`; off replays as fast
as the file can be read, which is what a test wants and not what a dashboard
wants) and `trc_replay_loop` (`false`; timestamps keep increasing across the
seam).

The node publishes raw frames only. DBC decoding into typed telemetry is the
decoder nodes' job, downstream of `vehicle/<name>/rx`.

## Topics

| Key | Schema | |
| --- | --- | --- |
| `vehicle/<name>/rx` | `CanFrame` | Every frame received on the channel. |
| `vehicle/<name>/tx` | `CanFrame` | Publish here and the frame goes on the wire. Best-effort: a publisher outrunning the subscriber loses samples in zenoh, not in the driver. |
| `vehicle/can/status` | `CanBridgeStatus` | One `CanBridgeChannelStatus` per channel: `open`, `running`, `error`, the bit rates, `state`, `rxFrames`, `txFrames`, `rxDropped`, `txDropped`, error counters where the backend has them, `recordPath`, `recordedFrames`, `recordDropped`. Republished every `status_interval_ms` so a late subscriber does not wait for something to go wrong. |
| `vehicle/can/set_bitrate` | service | Change a channel's bit rate at runtime, by `name`. `--set-bitrate` is the client. |

## Access

On Linux the USB dongles (`pcan:`, `motec:`) need a udev rule; without one,
opening fails with `Access denied`. The rules ship with the node and cover
the MoTeC UTC and the PEAK PCAN family, because a machine doing CAN work
usually has both:

```bash
sudo cp nodes/can_bridge/udev/99-can-usb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
./build/nodes/can_bridge/can_bridge --list
```

`udevadm trigger` re-applies the rules to devices that are already plugged
in. An entry that still says `UNAVAILABLE: Access denied` means the rule did
not match. `plugdev` is the group the rules grant to; `id -nG` will say
whether you are in it, and a group added to your account only takes effect on
your next login. Distributions that use a different group name need the
`GROUP=` in the rules file changed. The PCAN rule grants access but does not
free the device: `peak_usb` claims those adapters at plug-in, so the libusb
path also needs `pcan_detach_kernel_driver: true`. On Linux `socketcan:` is
usually the better path to a PCAN and needs none of this.

The udev rules do nothing for `socketcan:can0`. That path goes through the
kernel's CAN stack, where the interface is a network device and the
permission that matters is `CAP_NET_ADMIN`. Bringing the interface up is
privileged whichever way you do it:

```bash
# once per boot, per interface
sudo ip link set can0 up type can bitrate 500000

# or give the binary the capability, so bitrate: in the config takes effect
sudo setcap cap_net_admin+ep ./build/nodes/can_bridge/can_bridge
```

`can_bridge` reads the interface's real state either way and refuses to start
on one that is down and cannot be brought up, rather than running and
carrying nothing.

## Testing without hardware

`configs/can_bridge/replay.yaml` replays a checked-in trace onto
`vehicle/can0/rx`, where every decoder node already looks. Two ship in the
repository: `mock_data/data/pdm32_log.trc` (a MoTeC PDM32, 24156 frames) and
`mock_data/data/racegrade_tc8.trc` (a Racegrade TC8, 6600 frames). A
`virtual:` channel is a loopback with nothing behind it, for exercising the
node itself.

The MoTeC envelope also runs over UDP, which is how MoTeC's network gateways
speak and how that backend was first brought up:

```bash
# build the reference gateway from motec-gw-sim (its own main pulls in avahi;
# a three-line main that just runs GatewayServer does not)
./gwsim &

cat > /tmp/utc.yaml <<'EOF'
channels:
  - name: utc
    device: "motec:udp=[::1]"
    bitrate: 1000000
status_key: "vehicle/can/status"
set_bitrate_key: "vehicle/can/set_bitrate"
EOF
./build/nodes/can_bridge/can_bridge --config /tmp/utc.yaml
```

That exercises the envelope, the CRC, the session sequence, the record
handling, transmit and the statistics, everything except the FTDI framing and
the stream reassembly, which the unit tests cover directly.

## Troubleshooting

**A channel carries nothing and reports no errors.** Suspect the timing
before the cable. Every node on a bus must agree on the bit rate and sample
point, and a mismatch on a `motec:` channel is invisible because the UTC's
rate cannot be read back and the protocol has no error counters. A second
adapter on the same bus that does see traffic is the fastest way to tell
"wrong timing" from "quiet bus".

**A `motec:` channel goes deaf about ten seconds after it starts.** The UTC's
session watchdog. The backend sends a `Version` keep-alive every two seconds
to prevent it; if you are seeing this, the keep-alive is not going out, and
the design note has the measurements.

**A `motec:` channel answers `status 0x21` to everything, `Open` included.**
The dongle has latched. The backend recognises this, unlocks it through tag 0
and reopens, and logs `the gateway was latched and has been cleared`. No
unplugging is needed; a host reboot would not clear it anyway.

**Transmit on a lone UTC accepts four frames and then refuses the rest with
`status 0x20`.** Nothing on the bus is acknowledging, so the controller is
retrying the first frame forever. Normal CAN behaviour; put a second node on
the bus.

**`UNAVAILABLE: Access denied` in `--list`.** The udev rule did not match, or
you are not in `plugdev` yet, or you have not logged in again since being
added.

**`socketcan:` refuses to start.** The interface is down and the process
cannot bring it up. Use `ip link set ... up` once per boot, or `setcap`.

**`rxDropped` climbing on a `motec:` channel.** `rx_queue_depth` is below the
batch size the device delivers. Leave it at the default.

**Frames published to `vehicle/<name>/tx` do not all reach the wire.** Compare
against the bridge's own `txFrames` before suspecting the driver: a publisher
outrunning the subscriber loses samples in zenoh. Offering frames faster than
the bus can carry them (about 7 400/s for DLC 8 at 1 Mbit/s) loses them in
the adapter, silently, because the protocol has no transmit-overflow report.
