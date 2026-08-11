# msel_master_relay — the MSEL solid state battery isolator

The MSEL Master Relay is a solid state battery isolator that reports over CAN.
This node decodes what it says and exposes what it can be told, so its state
shows up on the bus like any other signal and its settings can be changed with
`inspect` — which matters because MSEL ship no configuration tool of their own.

```bash
cmake --build build --target msel_master_relay
./build/nodes/msel_master_relay/msel_master_relay \
    --config configs/msel_master_relay/msel_master_relay.yaml
```

The protocol lives in `libs/msel` and never touches zenoh or a CAN backend, so
all of it is exercised against a stub relay with nothing plugged in:

```bash
ctest --test-dir build -L msel
```

That includes the send-and-wait hand-off (`msel::ResponseWaiter`), which is in
the library rather than the node for the same reason: its interesting cases are
orderings between two threads, and inside a zenoh service callback they can be
reasoned about but not provoked. The one worth knowing is that an answer
arriving after its caller gave up is **dropped**, not saved for the next
command — these acknowledgements are late by nature, so handing one on would
report the wrong command as accepted.

## Read this before changing any setting

**Every configuration command is ignored unless a human is pressing and holding
the external kill switch on the relay while the frame is transmitted.** There is
no software substitute. A command sent without it is not rejected — it is not
answered at all, which looks exactly like a node that is not running.

**A settings call waits for the relay to answer and tells you what it said.**
`ok` means the relay accepted the change, not that bytes were transmitted. So
the intended sequence is simply: hold the switch, make the call, read the reply.

A command that is never answered is therefore an ordinary outcome rather than an
error, and it is reported as one — `answered: false` with a plain explanation.
That is what an unheld switch looks like, and it is also what an unpowered,
re-addressed or wrong-baud-rate relay looks like, which is why the message names
all four.

## What it publishes

With the default `topic_prefix` of `nodes/msel_master_relay`:

| Topic | Schema | Notes |
|---|---|---|
| `…/status` | `MselMasterRelayStatus` | Voltage out, load current, internal temperature, warning bits, state |
| `…/info` | `MselMasterRelayInfo` | Serial number, input voltage, shutdown history |
| `…/config` | `MselMasterRelayConfig` | The relay's stored settings, read back from its own messages |
| `…/switch_state` | `MselMasterRelaySwitchState` | Only on serial 64666 and above; absent on older units, which is normal |

There is no `config_response` topic. The relay's answer to a settings change is
waited for by the node and returned by the call that caused it — see below.

Load current is signed: positive while the battery is discharging, negative
while it is being charged. It is accurate to ±10% or 1 A, whichever is greater —
a whole-system draw indication, not a per-circuit measurement.

## The services

| Key | Request | Effect |
|---|---|---|
| `…/get_settings` | `MselGetSettingsRequest` | Read current settings. Takes no arguments; call with `--data '{}'` |
| `…/set_base_address` | `MselSetBaseAddressRequest` | Move all three messages. Live on acknowledge |
| `…/set_transmit_rate` | `MselSetTransmitRateRequest` | 10 Hz or 100 Hz. Needs a power cycle |
| `…/set_baud_and_shutdown_delay` | `MselSetBaudAndShutdownDelayRequest` | Bus rate and hold-up time. See the warning below |
| `…/set_output_drive` | `MselSetOutputDriveRequest` | PDM/ECU output drive mode. Needs a power cycle |
| `…/set_can_shutdown` | `MselSetCanShutdownRequest` | Enable remote shutdown and set its address. Needs a power cycle |
| `…/can_kill` | `MselCanKillRequest` | Isolates the battery and stops the engine. Gated — see below |

Every write returns an `MselCommandResponse`:

| Field | Means |
|---|---|
| `ok` | **The relay accepted it.** False covers all three failures below |
| `answered` | Whether the relay said anything at all within the wait window |
| `response` / `responseRaw` | What it said. `noAnswer` when it said nothing |
| `error` | Why not, in the words you need: a refusal here, a rejection code, or the timeout and what usually causes it |
| `frameSent` | The exact bytes that went out. Empty when nothing was sent |
| `waitedMs` | How long the node waited before being answered or giving up |
| `requiresPowerCycle`, `holdExternalKillSwitch` | Returned as data rather than logged, because the caller is the one who has to act on them |
| `notAcknowledged` | Only `can_kill` — see below |

The three ways a write fails are worth telling apart, because they need
completely different things done about them:

```
ok: false, frameSent: ""        -> refused here. Bad value, or not permitted
ok: false, answered: false      -> sent, nothing came back. Hold the switch
ok: false, answered: true       -> the relay rejected it. `response` says why
```

`can_kill` is the exception: the relay acts on the kill frame the moment it
arrives and never acknowledges it, so that call does not wait. It reports
`notAcknowledged: true` so its `answered: false` is not misread as a failure at
the one moment nobody should be in doubt.

**How long it waits** is `command_timeout_ms`, 1000ms by default. Your own query
timeout has to be longer than the node's or you get a transport failure instead
of the answer — `inspect call` gives up after 2000ms unless you pass
`--timeout`. The node refuses a configured value outside 100..10000ms for the
same reason at both ends: too short and an acknowledgement that queued behind
the relay's own 10Hz telemetry reads as silence, too long and the caller gives
up first.

```bash
./build/nodes/inspect/inspect services                      # discover the keys
./build/nodes/inspect/inspect schema MselSetOutputDriveRequest
./build/nodes/inspect/inspect call nodes/msel_master_relay/get_settings --data '{}'
./build/nodes/inspect/inspect call nodes/msel_master_relay/set_output_drive \
    --data '{"drive":"activeLowLowSide"}'
```

`valid: false` in a `get_settings` response means the relay's info message has
not been seen yet — the difference between "the shutdown delay is zero" and "we
have not heard from this relay at all".

## Three things that will catch you out

**Changing the baud rate can make the relay disappear.** The new rate takes
effect at the next power cycle. If the rest of the bus is not moved to match, the
relay comes back speaking a language nothing else on the wire understands, and
the only way back is a CAN interface set to the new rate. Change the bus first,
or be ready to chase it.

**The message identifiers are not constants.** The base address is
user-configurable, and the three messages sit at base, base+1 and base+3 — not
base+2, which is why MSEL's own default kill address of `0x6E6` sits in that gap.
A relay that has been re-addressed is silent on `0x6E4` and looks identical to
one that is unpowered. If the status topic never updates, check `base_address`
before you check the wiring.

Changing the base address through this node retunes its decoder in place, so
telemetry keeps flowing. Nothing else on the bus is told: loggers, dashes and DBC
imports elsewhere will still be looking at the old identifiers.

**It only retunes if the relay actually acknowledged the move**, and reverts if
it did not. That matters because an unacknowledged command is the common case
here: without the fix, a `set_base_address` sent without the switch held left
the node listening at an address the relay had never moved to, and telemetry
stopped with nothing said about why. While the change is in flight the node
listens for the acknowledgement at *both* addresses — the relay re-addresses as
it accepts, so the answer comes back at the new one, but the manual can also be
read as answering before the move. Watching both costs nothing and is right
either way.

**The relay stops transmitting on a quiet bus.** If it sees nothing for more than
two of its own transmit cycles — checked every five seconds — it disables CAN
transmission until the bus is alive again, and shows a flashing green LED. A
relay wired to a bus with nothing else on it will therefore go silent, and that
is the device working as designed rather than a fault.

## When a change takes effect

| Command | Live on acknowledge | Needs a power cycle |
|---|---|---|
| Base address | ✓ | |
| Shutdown delay | ✓ | |
| Transmit rate | | ✓ |
| Baud rate | | ✓ |
| Output drive | | ✓ |
| CAN shutdown enable | | ✓ |

Baud rate and shutdown delay travel in one command because the device packs them
into one frame, so that command is reported as needing a power cycle even though
half of it is immediate. That is the answer that does not mislead: cycle the
relay and you get both changes, do not and you get only the delay.

## Remote shutdown

The relay accepts a CAN frame that isolates the battery and stops the engine.
This node can send it, and refuses to unless **both** gates are open:

1. `allow_can_kill: true` in the node's config, which is `false` by default.
2. `confirm` set to exactly `ISOLATE-THE-BATTERY` in the request.

Neither alone is sufficient — a config file left permissive on a bench should not
turn a stray service call into a way to stop a moving vehicle. The relay itself
also has to have remote shutdown enabled in its own settings, which it is not
from the factory, so in practice there are three.

Unlike the configuration commands, the kill frame does **not** need the external
kill switch held. It is acted on the moment it arrives; that is the point of it.

MSEL's own warning is worth repeating: if you drive this from a MoTeC accident
data recorder, check previously logged data to make sure the severe-event
threshold does not trigger on an ordinary curb strike.

## Trying it without hardware

Everything below runs against a virtual CAN channel, with no relay present.

```bash
./build/nodes/msel_master_relay/msel_master_relay \
    --config configs/msel_master_relay/msel_master_relay.yaml &

./build/nodes/inspect/inspect echo nodes/msel_master_relay/status &

# 12.56 V out, 54.5 A, 25.2 C, no warnings, status "normal"
./build/nodes/inspect/inspect publish vehicle/can0/rx --schema CanFrame \
    --data '{"id":1764,"len":8,"data":[4,232,2,33,0,252,0,1]}'

# The relay's settings, from its info message
./build/nodes/inspect/inspect publish vehicle/can0/rx --schema CanFrame \
    --data '{"id":1765,"len":8,"data":[4,232,252,154,86,10,15,135]}'
```

To watch a command go out, subscribe to the transmit topic and make a call:

```bash
./build/nodes/inspect/inspect echo vehicle/can0/tx &
./build/nodes/inspect/inspect call nodes/msel_master_relay/set_output_drive \
    --data '{"drive":"activeLowLowSide"}'
# id 1929 (0x789), data 0A BC 06 06 00 00 0A BC -- the manual's table 20
# and, a second later: ok false, answered false -- nothing is out there to answer
```

**You can play the relay's half too**, which is how the waiting was checked
without hardware. Its answer is the same byte eight times on the base
identifier, so injecting one while a call is in flight is a one-liner. Give the
call a wide window, because `inspect` takes a moment to start:

```bash
# In one shell, with command_timeout_ms raised to 6000 in the config:
./build/nodes/inspect/inspect call nodes/msel_master_relay/set_output_drive \
    --data '{"drive":"activeLowLowSide"}' --timeout 9000

# In another, before that window closes -- 0x00 x8 is "success" on 0x6E4:
./build/nodes/inspect/inspect publish vehicle/can0/rx --schema CanFrame \
    --data '{"id":1764,"len":8,"data":[0,0,0,0,0,0,0,0]}'
# the call returns ok: true, answered: true, waitedMs: <however long you took>

# 0x33 x8 is a rejection, and comes back as response: invalidId
./build/nodes/inspect/inspect publish vehicle/can0/rx --schema CanFrame \
    --data '{"id":1764,"len":8,"data":[51,51,51,51,51,51,51,51]}'
```

Note the ordering trap if you script both halves: publish too early and the
answer is dropped as unsolicited — correctly, since no command was outstanding —
and the call still times out. The node logs the decode, so a log line that comes
*before* the "sent" line is the giveaway.

## Where the protocol description came from

`dbcs/msel/msel_master_relay.dbc` is authored in this repository from the Master
Relay User's Manual Rev 2.0 (September 2025), not shipped by the vendor. MSEL do
publish a `.dbc`, but it is dated 2014-11-19 — eleven years older than that
manual — and describes a device that no longer matches: no `0x6E7` message, no
value tables, and signal ranges that contradict the specification.

The five configuration commands, the kill trigger and the configuration response
are deliberately **not** in the DBC, because a DBC cannot express them. The
commands are constant-magic frames on a fixed identifier carrying each value
twice; the response arrives on the base status identifier, so one identifier
carries two different layouts. All three live in `libs/msel/include/msel/protocol.h`
with tests pinning them to the manual's worked byte sequences.
