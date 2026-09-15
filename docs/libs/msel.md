---
title: msel
parent: Libraries
---

# msel

## Overview

The MSEL Master Relay protocol: decoding what the isolator reports, and
building the commands that reconfigure it. Deliberately free of zenoh and of
any CAN backend. It takes frames and returns frames, which is what lets the
whole protocol, including the parts that only happen while a human holds a
switch on the device, be tested against a stub with nothing plugged in.
Wiring it to the bus is [msel_master_relay](../nodes/msel_master_relay.html).

The generated DBC code (`dbc_msel_master_relay`, from
[dbc_parser](dbc_parser.html)) is linked private: the decoder converts into
this library's own types, so nothing downstream includes a generated header or
cares that the messages came from a DBC. The protocol was authored from the
Master Relay User's Manual Rev 2.0 (September 2025), and the comments cite its
table numbers.

## Public headers

| Header | |
| --- | --- |
| `msel/protocol.h` | The enums and their `*FromRaw` conversions, `Addresses`, `Config`, the decoded frame structs, the `makeSet*Frame` command builders, `decodeConfigResponse`, `effectOf`. Pure functions over bytes. |
| `msel/decoder.h` | `Decoder`: the stateful half, the only part that knows the relay has been re-addressed; callbacks and a `Snapshot`. |
| `msel/response_waiter.h` | `ResponseWaiter`: the rendezvous between the thread that sends a command and the thread that decodes the answer. |
| `msel/stub_relay.h` | `StubRelay`: a simulated relay that packs its frames by hand from the manual's byte tables. |
| `msel/error.h` | `msel::Error` with two kinds, `InvalidArgument` and `Unsupported`, and deliberately no `Io`. |

## Using it

Link the CMake target `msel`. The node constructs a decoder at the configured
base address, registers callbacks before frames arrive, and arms a waiter
before each command goes out:

```cpp
msel::Decoder decoder(msel::Addresses { .base = config.baseAddress });
msel::ResponseWaiter waiter;
decoder.onStatus([&](const msel::StatusFrame& f) { publish(f); });
decoder.onConfigResponse([&](msel::ConfigResponse r) { waiter.deliver(r); });

// on the receive thread
decoder.onFrame(frame);

// on the service thread
waiter.arm();
auto cmd = msel::makeSetTransmitRateFrame(msel::TransmitRate::Hz100);
send(*cmd);
auto answer = waiter.wait(commandTimeout);   // nullopt: the switch was not held
```

## Behaviour worth knowing

The base CAN address is user-configurable, so the three periodic messages do
not sit at fixed identifiers, but the DBC describes them at one fixed set.
The decoder matches an observed identifier against the three this relay is
configured to use and only then translates, rather than handing `id - offset`
to the generated parser: subtracting an offset from every frame on the bus
makes unrelated traffic alias onto the relay's messages, and with a base of
`0x6F4` an arbitrary `0x704` would decode as a status frame and publish
invented voltages. `setAddresses` re-addresses in place and keeps both the
callbacks and the snapshot, because assigning a fresh `Decoder` silently
throws the callbacks away and frames then decode correctly and reach nobody.

{: .warning }
Every configuration command only takes effect while a human is pressing and
holding the external kill switch on the relay. There is no software
substitute, an ignored command is not answered at all, and
`requiresHeldExternalKillSwitch()` is a function so call sites read as a
statement about the protocol.

The five commands go to one fixed identifier, `0x789`, which does not move
with the base address; they are told apart by a magic word at both ends of the
payload with each value carried twice. The response arrives on the base status
identifier, which already carries a periodic message at 10 Hz. The two are
told apart by shape: a response is eight identical bytes that are all one of
`0x00`, `0x11`, `0x22`, `0x33`, and a status frame cannot look like that because
its byte 7 is a `Status` in 1 to 10. That non-overlap is the whole reason
`decodeConfigResponse` is safe and must be rechecked if either set grows.

A base-address change is the one command whose acknowledgement does not come
back where the caller was listening: the relay moves as it accepts, so the
answer arrives at the new base, or at the old one if a given firmware answers
before it moves. Both are plausible readings of the manual and no test can
settle it, so `watchConfigResponseAt` accepts a configuration response, and
only a response, at one extra identifier while the node waits.

Only the shutdown delay applies immediately; everything else is stored and
read at boot, and `effectOf` returns that so the node can hand it back to
whoever asked. A delay that is not a whole multiple of 100 ms is rejected, not
rounded, since the field is in tenths of a second and truncating 150 ms to
100 ms would leave the caller believing it had set something it had not.
`validateBaseAddress` refuses a base whose `+3` falls off the 11-bit space or
whose span would swallow `0x789` and make the device unreachable. Enum fields
in `Config` and the frame structs are optional with the raw byte beside them: a
value the manual does not assign is reported as absent, never coerced onto a
neighbouring meaning. The switch-state message is only transmitted by units at
serial number 64666 or above, so its absence from a snapshot is ordinary.

`ResponseWaiter` must be armed before the frame goes out; on a fast bus the
answer can be decoded before the sender returns from its publish. A response
delivered while nothing is armed is dropped, because these answers arrive
seconds late and only when a human timed the press right, so an answer turning
up after its caller gave up is what happens whenever someone tries again, and
handing it to the next command would report the wrong command as accepted.
Never call `wait()` holding a lock the receive thread needs.

## Tests

```bash
ctest --test-dir build -L msel    # msel_test_protocol, msel_test_decode, msel_test_response_waiter
```

All three are labelled `msel` and `unit`. `msel_test_protocol` pins every
command builder to the worked byte sequence printed in the manual, the only
independent statement of what the bytes should be, and checks the refusals:
the 150 ms delay, the base address whose third message falls off the end, the
command effects, response recognition, the config-word round trip and the enum
conversions. `msel_test_decode` drives the decoder at a shifted base and feeds
it traffic chosen to alias if an offset had been subtracted, checks both
directions of the response/status collision, and round-trips against
`StubRelay`; the stub packs its frames by hand from the manual's tables rather
than through the DBC, so decoding them is a real check that the DBC's layout
matches the hardware, which encoding and decoding through the same generated
code would not be. It links the generated DBC directly to cross-check the
DBC's view of the configuration word against the hand-written one.
`msel_test_response_waiter` uses real threads because the cases worth pinning
are orderings: an answer that beats its waiter, silence timing out, a late
answer not given to the next command, an unsolicited answer dropped, and
consecutive commands each getting their own.
