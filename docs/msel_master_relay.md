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

## Read this before changing any setting

**Every configuration command is ignored unless a human is pressing and holding
the external kill switch on the relay while the frame is transmitted.** There is
no software substitute. A command sent without it is not rejected — it is not
answered at all, which looks exactly like a node that is not running.

Settings changes are therefore single-shot. A service call builds one frame,
sends it, and returns immediately with the bytes it sent; it does not wait for
the relay, because waiting would turn every mistimed press into a timeout. The
relay's answer, when it comes, is published on `<prefix>/config_response`.

The intended sequence is: hold the switch, make the call, watch
`config_response`.

## What it publishes

With the default `topic_prefix` of `nodes/msel_master_relay`:

| Topic | Schema | Notes |
|---|---|---|
| `…/status` | `MselMasterRelayStatus` | Voltage out, load current, internal temperature, warning bits, state |
| `…/info` | `MselMasterRelayInfo` | Serial number, input voltage, shutdown history |
| `…/config` | `MselMasterRelayConfig` | The relay's stored settings, read back from its own messages |
| `…/switch_state` | `MselMasterRelaySwitchState` | Only on serial 64666 and above; absent on older units, which is normal |
| `…/config_response` | `MselMasterRelayConfigResponse` | The relay's answer to a settings change |

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

Every write returns an `MselCommandResponse` carrying the exact bytes sent, plus
`requiresPowerCycle` and `holdExternalKillSwitch`. Those two are returned as
data rather than logged, because the caller is the one who has to act on them.

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
```

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
