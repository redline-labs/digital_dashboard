# The MoTeC UTC

A MoTeC UTC is a USB-to-CAN dongle: an FTDI FT245BM in front of a classic CAN
controller. `libs/can_motec` drives it as a `can::Channel`, so it appears
alongside `socketcan:` and `pcan:` in `can_bridge` and anything else built on
the channel registry.

```yaml
channels:
  - name: engine
    device: "motec:0"          # the first UTC attached
    bitrate: 1000000           # see "The bit rate" below -- this is NOT applied
```

```
motec:0                    the first UTC attached
motec:56536                that index if it exists, otherwise that serial
motec:serial=56536         that serial, never an index
motec:udp=192.168.1.40     a network gateway speaking the same protocol
motec:udp=[fe80::1%eth0]:29456
```

The index-or-serial fallback is not a convenience. A UTC's serial is a bare
number — the one this was written against is `56536` — so there is no way to
tell an index from a serial by looking at it. A digit string is tried as an
index first and as a serial second, and `serial=` forces the second reading.

## What this cannot do

The protocol is not published, and the parts below were never worked out. All
of them **fail loudly** rather than being approximated, because every one of
them fails silently if guessed at.

| | Why |
|---|---|
| **Set the bit rate** | The `Set` command exists and its argument is shaped like `0x40000000 \| (value << 18)`, but which register selects the bus speed is unknown. `set_bitrate()` returns `Unsupported`. |
| **Read the bit rate back** | Nothing in the protocol reports it. |
| **Listen-only** | No known command. A receive filter is not equivalent — it stops frames being delivered, but the controller still acknowledges them on the bus, which is the entire point of listen-only. An open that asks for it fails. |
| **CAN FD** | The hardware is classic CAN. |
| **Bus state, error counters, bus-off** | Nothing in the protocol carries them. |
| **Remote (RTR) frames** | Four bits of the flags byte are not understood and one of them is presumably RTR. Sending one would put a data frame on the bus where a request was meant. |
| **More than one acceptance filter** | The device accepts exactly one Filter write per session; see below. |
| **Leaving the link idle** | The session times out after ~10 s of client silence; the driver sends a periodic `Version` to prevent it. See "The session has a watchdog". |
| **Closing a session** | No close command is known, and the device latches. See "The `0x21` latch". |

### The acceptance filter, as the hardware actually behaves

Measured against a real UTC, because none of this is in the captures:

- **The index must be 2 or 3.** Index 0 and anything from 4 up are refused with
  status `0x22`. Index 1 is refused with `0x40` — a *different* status, so 1
  looks like a slot that exists and is reserved rather than a value out of
  range. The captured client used 3, and that is the default here.
- **One filter per session, whatever its index.** The second write is refused
  with `0x22` whether it repeats the first index or names the other valid one.
  An `Open` is what clears it. So this is not a bank of filters to populate; it
  is one filter, written once, before the Rx subscribe.
- Pattern and mask are accepted freely — any values, including an all-ones
  mask, which is what makes "accept everything" possible in a single write.

This cost a bring-up: the first attempt used index 0 with an all-ones mask,
which the device refused, and the only diagnostic was `status 0x22`.

### Identifiers are laid out two different ways

The captured frames are **all extended**, so they say nothing about standard
identifiers — and standard identifiers are not carried the same way:

```
bit 31 set   extended: the 29-bit identifier in bits 0..28
bit 30 set   standard: the 11-bit identifier in bits 18..28, LEFT-ALIGNED
```

Left-aligned because bits 18..28 are the top eleven bits of the same 29-bit
arbitration field. Measured by transmitting from a PCAN on the same bus and
reading the raw word back off the UTC:

| bus identifier | wire word |
|---|---|
| `0x001` standard | `0x40040000` |
| `0x100` standard | `0x44000000` |
| `0x200` standard | `0x48000000` |
| `0x7FF` standard | `0x5FFC0000` |
| `0x1ABCDEF` extended | `0x81ABCDEF` |
| `0x1FFFFFFF` extended | `0x9FFFFFFF` |

Both directions were wrong before this was measured. Setting *neither* format
bit — which is what encoding a standard identifier into the low bits does —
made the device transmit it as a **29-bit frame**; a PCAN dongle on the same
bus reported exactly that. Decoding a standard frame from the low bits gave a
large, stable, entirely wrong identifier. Neither failure is visible without a
second device on the bus, which is why the table above is in
`tests/golden/utc_frames.h` as data rather than in a comment.

### The bit rate

The device runs at whatever MoTeC's own tool last configured it for. The
`bitrate:` in the config is recorded and reported but **does not reach the
hardware**, and the backend says so at every open:

```
[warning] [motec] motec:0 runs at whatever bit rate MoTeC's tool last
          configured; the requested 1 Mbit/s is NOT applied and cannot be read back
```

`bitrate()` therefore returns what was asked for, which is exactly the
"reports intent rather than reality" trap the SocketCAN backend was fixed for.
The difference is that there the truth was available over netlink and simply
never asked for; here there is no way to ask. If the `Set` register map is ever
worked out, `set_bitrate()` is the one function that has to change.

Because the rate cannot be checked, a mismatch presents as a bus that carries
nothing, with no error anywhere. That is worth remembering before suspecting
the cable.

## Access

On Linux the dongle needs a udev rule; without one, opening it fails with
`Access denied`. The rules ship with the node — they cover the MoTeC UTC and
the PEAK PCAN family, because a machine doing CAN work usually has both:

```bash
sudo cp nodes/can_bridge/udev/99-can-usb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

`udevadm trigger` re-applies the rules to devices that are already plugged in,
so there is no need to unplug anything. Check it took:

```bash
./build/nodes/can_bridge/can_bridge --list
```

An entry that still says `UNAVAILABLE: Access denied` means the rule did not
match. `plugdev` is the group the rules grant to — `id -nG` will say whether
you are in it, and a group added to your account only takes effect on your next
login. Distributions that use a different group name need the `GROUP=` in the
rules file changed to match.

Nothing in mainline Linux binds a driver to `0403:dcd8` — `ftdi_sio` ignores
MoTeC's product id unless told about it with a `new_id` write — so the dongle is
normally free for libusb to claim. If something has bound to it,
`detachKernelDriver` takes it away.

The FT245BM is a fixed 64-byte FIFO part with no baud rate, no latency timer and
no flow control. This driver deliberately issues **none** of those control
requests; doing so is the first thing a driver written against the far more
common FT232 would do, and this device does not answer them.

### The other half: SocketCAN

The udev rules do nothing for `socketcan:can0`. That path goes through the
kernel's CAN stack, where the interface is a network device and the permission
that matters is `CAP_NET_ADMIN` — there is no device node to grant. A PCAN
adapter is claimed by the in-tree `peak_usb` driver at plug-in, which is what
creates `can0`, so this is the usual way to reach one on Linux.

Bringing the interface up is a privileged operation whichever way you do it:

```bash
# once per boot, per interface
sudo ip link set can0 up type can bitrate 500000
```

Or give the binary the capability, so it can configure the link itself and
`bitrate:` in the config actually takes effect:

```bash
sudo setcap cap_net_admin+ep ./build/nodes/can_bridge/can_bridge
```

`can_bridge` reads the interface's real state either way and refuses to start
on one that is down and cannot be brought up, rather than running and carrying
nothing.

## The `0x21` latch, and how to clear it

The device can end up in a state where **every** command on the normal tag --
every opcode, every bus handle, every payload, `Open` included -- is answered
with `status 0x21`. It is not dead: the replies are well formed, the CRC is
right and the request id is echoed. It simply refuses all service, and because
`Open` is refused too there is no way back in by the front door.

**The way out is `tag`.** The command byte is `(tag << 5) | code`, and the tag
selects an endpoint. Tag 0 is a separate one that keeps answering while the
others are latched -- it replies with its own per-command statuses instead of a
blanket `0x21`. An `Open` addressed there clears the latch, after which the
normal tag works again:

```
tag 1..7  Open / Version / Poll   -> 0x21   (everything refused)
tag 0     Version                 -> 0x22
tag 0     Poll                    -> 0x04
tag 0     Open, no payload        -> 0x23
tag 0     Open, version payload   -> 0x00   <- clears it
```

The payload matters: an `Open` on tag 0 with no payload is answered `0x23` and
the device stays latched.

`can_motec` does this automatically. `handshake()` recognises `0x21`, sends the
unlock, and reopens; the log says so when it happens. A node that came up
against a latched dongle used to need a human with physical access, and now
does not.

### What does NOT clear it

All tried against a genuinely latched device, and all recorded here so nobody
repeats them:

- reopening the USB device and re-claiming the interface;
- `libusb_clear_halt` on both bulk endpoints;
- a full `USBDEVFS_RESET` -- it re-enumerates in half a second and comes back
  still latched, because the FT245BM in front is only a FIFO bridge and
  resetting USB does not reset the microcontroller behind it;
- writing `usb1-portN/disable`, which maps to `CLEAR_FEATURE(PORT_POWER)`. On
  an Intel Cannon Lake root hub the request is accepted and the device
  re-enumerates, but **Vbus is not actually switched** -- a logical disconnect
  only, and the latch survives. A powered hub with real per-port power
  switching would be a different matter;
- **a full host reboot** -- the board keeps Vbus up across a warm boot, so the
  microcontroller never loses power;
- waiting, for tens of minutes;
- bringing another node onto the bus so pending transmissions can drain.

Unplugging the dongle clears it, which is what pointed at power rather than
protocol in the first place -- but the tag-0 unlock makes that unnecessary.

### What causes it is not known

It happened twice, both times after a client was killed or exited without
closing its session. But it could not be reproduced deliberately: twelve
open-without-close cycles, overfilling the transmit buffer until the device
refused 1 994 of 2 000 frames, and killing a subscriber three times mid-stream
all left the device perfectly healthy. The trigger is still open, which is
exactly why the recovery is automatic rather than documented as a manual
procedure.

## The protocol

### Provenance

None of this is published. It comes from
[motec-gw-sim](https://github.com/ryandavid/motec-gw-sim), reverse-engineered
from CAN Inspector v1.19 and from packet captures of a genuine UTC.

`libs/can_motec/tests/golden/utc_frames.h` holds **eleven frames captured from
real hardware**, and every one has to decode and re-encode byte-identically.
That is the only part of this library that can be called confirmed, and it is
what the tests are built around — a vector written from the same reading of the
protocol as the parser agrees with the parser even where both are wrong. Those
captures are what established that the extended-identifier bit lives in bit 31
of the identifier rather than in the flags byte, that the data block carries no
CRC of its own, and that a Tx acknowledgement's byte count is not a block
length.

### The envelope

```
[0..2]     80 81 86            preamble, NOT covered by the CRC
[3]        N                   count of CRC-covered bytes, starting here
[4]        CMD                 (tag << 5) | code; bit 0x10 set on replies
[5]        field5              bus handle from Open
[6]        REQID               rolling request id
[7..]      payload             N-4 bytes
[3+N]      CRC-16-CCITT        big-endian, over the N bytes from [3]
[3+N+2..]  data block          Tx/Rx/RegRead only, and NOT checksummed
```

A frame is `3 + N + 2 + dataLength` bytes. The CRC is polynomial `0x1021`,
initial value `0xFFFF`, non-reflected, no final XOR.

| Code | Command | |
|---|---|---|
| `0x00` | Open | Returns the bus handle every later request must echo in `field5` |
| `0x01` | Poll | Answered with a bare status |
| `0x02` | Ack | One-way; must **not** be answered |
| `0x04` | RegRead | Register read; answers on the data path |
| `0x06` | Filter | Acceptance filter write; mask bits are *don't care* |
| `0x08` | Tx | Transmit; records ride in the data block |
| `0x09` | Rx | Subscribe — sent **once**, then the device free-runs |
| `0x0A` | Set | Config write; register map unknown |
| `0x0F` | Version | A real UTC answers 7.2 |

The code is the low **four** bits of `CMD`, not five: the reply bit `0x10` sits
immediately above it, so a 5-bit read folds every response into a different
command.

### The data block length

This is the most delicate thing in the codec, and it is why `FrameReader`
exists rather than a loop over `decode_frame`. A stream transport cannot fall
back on "whatever is left in the datagram" the way the UDP one can, so the
block's length has to be derived from the command:

| | Payload | Block length |
|---|---|---|
| Tx **request** | `[BE16 byte count][BE16 record count]` | payload `[0..1]` |
| Rx **response** | `[status][BE16 byte count]` | payload `[1..2]` |
| RegRead **response** | `[status][BE16 byte count]` | payload `[1..2]` |
| everything else | | none |

The trap is the **Tx response**. It carries a BE16 in the same position an Rx
response carries its block length — the count of bytes the device accepted —
with nothing behind it. Reading it as a length makes the reader wait for
seventeen bytes that are never sent, and then find them at the front of the
next frame, consuming it. `test_tx_ack_does_not_swallow_the_next_frame` is
there for exactly this.

### CAN records (17 bytes)

```
[0..3]   CAN id      big-endian; bit 31 = extended (29-bit) identifier
[4]      flags/DLC   low nibble is the DLC; the upper four bits are unknown
[5..12]  data        always 8 bytes on the wire
[13..16] timestamp   big-endian free-running microseconds
```

Two things that bite:

- **Bytes past the DLC are junk, not padding.** Real hardware leaves stale
  buffer content there. `to_can_frame()` drops them.
- **The extended bit is in the identifier, not the flags.** A decoder looking
  in the flags byte reads a 29-bit `0x1CF81BEC` as an 11-bit identifier and
  hands over a frame that is wrong in the one way nothing downstream detects.

The timestamp runs at roughly 1,001,800 ticks/s, so it drifts against
wall-clock by about 0.18%.

### The session

```
Open    -> bus handle
Version -> 7.2
Filter  -> accept everything
Rx      -> subscribe ONCE; the device then free-runs
```

After the subscribe the device pushes a data frame roughly every 255 ms on its
**own** request-id counter, empty ones included. The idle ones are what
`statistics()` uses to tell a live link from a dead one: nothing arriving for
`rxStallTimeoutMs` reports `BusState::Unknown` rather than a flattering
`ErrorActive`.

### The session has a watchdog, and it is not optional

The device stops sending **about ten seconds after the subscribe** unless the
client keeps talking to it. Data frames stop, the idle keep-alives stop, and a
fresh subscribe is answered with `status 0x04`. Nothing warns before it
happens, so the symptom is a bridge that goes deaf ten seconds after it starts
— which reads exactly like a bus that went quiet.

The command that keeps it open is **`Version`** (`0x0F`), sent periodically;
the reference client calls this its "MinReq keep-alive". This driver sends one
every `keepAliveIntervalMs` (2 s) from the receive thread.

Measured, with a PCAN transmitting on the same bus:

| | records received |
|---|---|
| no keep-alive | 600 in the first burst, then **nothing** — a burst at t+11 s delivered zero |
| `Version` every 2 s | 600 + 600 + 600 across 45 s and three bursts, **zero dropped** |

**Do not use `Ack` for this.** `Ack` (`0x02`) is documented as one-way, and
sending one stops the stream *immediately* — three idle keep-alives and then
silence, worse than sending nothing at all.

### USB framing

Outbound bytes go verbatim. **Inbound, every 64-byte packet begins with two
FTDI modem-status bytes that are not part of the stream** (`31 60` observed).
`strip_ftdi_status()` removes them before the reader sees anything; feeding a
raw transfer straight in puts `31 60` in the middle of a record.

Frames straddle packet boundaries in both directions, and several small frames
arrive in one packet, so the reader has to reassemble rather than assume one
read is one frame.

## Testing without hardware

The same envelope runs over UDP, which is how MoTeC's network gateways speak.
It is also the fastest way to work on the session logic without a dongle
attached, and it is how this backend was first brought up -- the USB half has
since been verified against real hardware as well, see below.

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
handling, transmit and the statistics — everything except the FTDI framing and
the stream reassembly, which the unit tests cover directly.

## What has been verified, and how

The USB transport has now run against a real UTC (serial 56536) with a
PCAN-USB Pro FD on the same 1 Mbit/s bus, each acting as the other's oracle.
Confirmed on hardware:

- the FTDI status stripping and the stream reassembly — the device answers
  `Version 7.2`, which cannot be read without both being right;
- the session sequence: Open, Version, Filter, Rx subscribe;
- receive, including the keep-alive cadence that the stall detector uses;
- transmit, in both identifier formats, checked by a second dongle on the wire;
- the identifier layouts and filter rules documented above, which is where the
  bugs were.

Frames verified crossing the bus in both directions: `0x201` and `0x7FF`
standard, `0x1ABCDEF`, `0x1FEDCBA` and `0x1FFFFFFF` extended, at DLC 0, 2, 3
and 8.

### Sustained load

Measured PCAN to UTC, DLC 8 standard frames at 1 Mbit/s, sequence-numbered so
gaps, duplicates and reordering are all detectable:

| offered | delivered | in order | repeats |
|---|---|---|---|
| 1 000/s × 10 000 | 100% | yes | 0 |
| 3 000/s × 15 000 | 100% | yes | 0 |
| 5 000/s × 20 000 | 100% | yes | 0 |
| 7 000/s × 21 000 | 100% | yes | 0 |
| 6 000/s for 20 s (120 000 frames) | 100% | yes | 0 |

7 000/s is essentially the ceiling: a DLC 8 standard frame at 1 Mbit/s is about
135 bits with stuffing, so the bus tops out near 7 400/s, and the highest rate
actually observed arriving was 7 920/s. The pipeline is lossless right up to
it, and there is no degradation over a 20-second soak.

**Do not offer frames faster than the bus can carry them.** Handing the adapter
15 000/s when the bus does 7 400/s loses roughly half of them: USB accepts every
transfer, `send()` returns success, and the adapter discards silently — there is
no transmit-overflow report in the protocol (`PCAN_UFD_MSG_OVERRUN` is
specifically a *receive* overflow). `Statistics::txFrames` therefore counts
frames handed to the adapter, not frames that reached the wire.

### The receive queue, and how deep it has to be

`rx_queue_depth` is not a tuning knob here in the way it is for the other
backends: **this device delivers records in batches**, around 60 in one data
frame on a busy bus, and they are all pushed onto the queue under one lock
before the reader can drain any. A queue shallower than the largest batch the
device sends therefore drops frames even on a quiet bus.

The PCAN backend does not behave this way -- it delivers a record at a time, and
a queue one entry deep still did not overflow at 2 000 frames/s -- so a depth
that is fine there is not necessarily fine here. The 8 192 default holds about
two seconds of a saturated bus and is the right choice; anything under about a
hundred is asking for drops.

Forced overflow, 2 000 frames at 1 000/s into a 16-deep queue:

- `rxFrames 2040`, `rxDropped 1456`, and **exactly** 584 frames published --
  `rxFrames - rxDropped` matches what came out, so the accounting is right;
- the oldest are discarded and the survivors stay in order, spread across the
  whole run rather than truncated at one end;
- the channel recovers completely: five frames sent afterwards all arrived.

Still unverified: the PCAN backend's own overflow path (nothing available on
this bench produces frames faster than the bridge drains them from it), and
anything to do with CAN error conditions on the UTC -- the protocol reports
none, so there is nothing to check.

### The transmit path

Lossless, and an earlier claim here that it was not was wrong: the missing
frames were dropped by zenoh before the bridge ever saw them, which shows up
only if you compare against the bridge's own `txFrames` rather than against
what you published. Measured at all three points:

| offered to zenoh | reached the bridge | reached the wire |
|---|---|---|
| 500/s × 2 000 | 2 000 | **2 000** |
| 1 000/s × 3 000 | 3 000 | **3 000** |
| 2 000/s × 3 000 | 2 811 (189 lost in zenoh) | **2 811** |

Every frame the bridge was actually handed reached the bus. `vehicle/<name>/tx`
is a best-effort zenoh topic, so a publisher outrunning the subscriber loses
samples there, not in this driver.

### `status 0x20`: the transmit buffer is full

A Tx command is acknowledged with a status and a byte count. `0x20` means the
device did not take the frame. It happens transiently under load -- 7 refusals
in 2 818 frames at 2 000/s, all correctly counted -- and **permanently when
nothing on the bus acknowledges**:

| | acknowledgements |
|---|---|
| UTC alone on the bus | 4 accepted, then **994 of 1 000 refused with `0x20`** |
| PCAN live on the same bus | **1 000 of 1 000 accepted**, and 2 000 of 2 000 at a higher rate |

So a lone UTC accepts about four frames and then refuses everything: the
controller is still retrying the first, unacknowledged, and the buffer never
drains. This is normal CAN behaviour rather than a fault, but on a bench with
only the dongle connected it presents as "transmit is broken".

`send()` counts the frame optimistically because it cannot wait for a USB round
trip; the acknowledgement arrives on the receive thread and corrects
`txFrames`/`txDropped` there. Verified exactly: with 7 refusals the partner
received precisely `txFrames` frames.

Still unknown, and worth looking for in any future capture: the meaning of the
four-byte Rx subscribe payload (`FF FF FF 01` and `FF FF 64 01` have both been
seen), the RTR bit, the upper four bits of the flags byte, the `Set` register
map, the third dword of the Filter command, and what distinguishes filter
index 2 from index 3.
