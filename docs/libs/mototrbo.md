---
title: mototrbo
parent: Libraries
---

# mototrbo

## Overview

The MOTOTRBO protocols, and nothing else: XNL, the session and authentication
layer a radio speaks over IP; XCMP, the command layer that rides inside an
XNL data message; the control plane built on XCMP (identity, status, the
selected channel, the `0xB4xx` broadcasts); and the NAI data-service codecs
for text, location and registration. Nothing here owns a socket, a thread or
a file, and almost nothing allocates; the two functions that produce a
`std::string` or a datagram say so. No spdlog: it reports through `Result<T>`,
and whether one refused command matters is the node's decision.

The transport half, TCP, the session state machine and the typed queries, is
[xpr](xpr.html), which depends on this. Nothing here depends on that. The
split is the one `gsof` makes from `bd992`, and the reason applies harder here
because this protocol was reverse-engineered rather than published: every way
of getting a frame wrong produces a frame the radio silently drops or a
plausible wrong value, never an error. So the parsers and builders are
`constexpr` and asserted at build time against bytes a real XPR 5550 sent or
accepted. The node is [xpr_bridge](../nodes/xpr_bridge.html); the provenance
and the defects only hardware found are in the
[design notes](../design/mototrbo.html).

## Public headers

| Header | |
| --- | --- |
| `mototrbo/byte_order.h` | Big-endian reads and writes, assembled byte by byte. A second, smaller copy rather than a dependency on `gsof/byte_order.h`, because this library links nothing. |
| `mototrbo/xnl.h` | The length-prefixed XNL frame: `parse_frame`, `serialize_frame`, the opcodes, `conn_request_payload`, `parse_conn_reply`, `parse_auth_reply`, and the TEA cipher (`tea_encrypt_block`, `auth_response`). |
| `mototrbo/xcmp.h` | The XCMP message: a big-endian u16 opcode and a payload. `parse_message`, the opcode enum, result codes. Bit 15 set marks a reply; `0xB4xx` is a broadcast. |
| `mototrbo/control.h` | Reading and steering a running radio: `parse_channel_reply`, `parse_channel_broadcast`, `parse_status`, `parse_broadcast`, and the display decode. `MOTOTRBO_STATUS_ITEM_TABLE` is the one list of status items. |
| `mototrbo/nai.h` | The data services, which are not XNL: TMS on port `4007`, ARS on `4005`, LRRP on `4001`. `encode_text` and the decoders. |
| `mototrbo/error.h` | `mototrbo::Error`: an enum plus two integers, a literal type so parsers can be `constexpr`; `detail` carries the radio's own result code. |

## Using it

Link the CMake target `mototrbo`. The library is builders and parsers over
bytes; a caller owns the socket and feeds it frames. The handshake in outline:

```cpp
auto reply = mototrbo::xnl::parse_frame(bytes);                 // MasterStatusBroadcast, then DeviceAuthReply
auto challenge = mototrbo::xnl::parse_auth_reply(reply->payload);
auto payload = mototrbo::xnl::conn_request_payload(challenge->challenge);   // 12 bytes, TEA response at +4
// ... serialize_frame(DeviceConnRequest, payload) and send; then
auto address = mototrbo::xnl::parse_conn_reply(next->payload);  // assigned address at +2

// Later, an XCMP reply inside an XNL data message:
auto message = mototrbo::xcmp::parse_message(frame.payload);
auto status = mototrbo::control::parse_status(requestedItem, message->payload);
```

`parse_status` takes the item that was requested and checks the echoed item
against it, so a layout that is wrong on hardware reports a mismatch instead
of returning a value read one byte off.

## Behaviour worth knowing

Replies correlate on the transaction id, not the opcode. Several distinct
queries share one opcode: `0x000E` selects the item with a payload byte, so
model, serial and DMR id all reply `0x800E`.

`CONN_REQUEST` is twelve bytes, not ten, with the device type at `+2` and the
authentication response at `+4`. The assigned address is at `CONN_REPLY+2`,
not `+0`. The data-message flags counter must advance, because the radio
dedupes on it. Unacknowledged delivery must be selected in `CONN_REQUEST`'s
flags (bit 3), or every query returns the previous query's answer. Each of
these was found only by putting frames in front of the radio, and each is
pinned by a vector in `tests/golden/hardware_vectors.h`.

The radio rejects a wrong TEA response with an all-zero `CONN_REPLY`. A
challenge/response pair from a live handshake therefore proves the key and
the cipher rather than proving self-consistency.

The data services do not ride the XNL session. They are separate protocols on
separate UDP ports of the same IP link, sharing only the radio's address. The
ports and formats are corroborated across two independent community
implementations (node-dmr-lib and Moto.Net). Only TMS has been exercised
against a radio; ARS is untested and LRRP unanswered.

**Deliberately absent.** The radio also implements FactoryReset (`0x003F`),
RadioReset (`0x000D`), EnterTestMode (`0x000C`), EnterBootMode (`0x0200`),
WriteMemory (`0x0202`), EraseFlash (`0x0203`), the ISH write/delete/reorg
opcodes, and Transmit (`0x0004`). None of them are in `xcmp.h`, and this
library keys no transmitter: it builds no PTT command and no RF tuning
command. That is a scope decision rather than an oversight. Do not sweep the
opcode space to "complete" the enum. The codeplug, its decoding and its field
schema are out of scope for the same reason: this stack reports what the
radio is doing, not how it is configured.

An LRRP request builder is also absent. Nine framings were tried against the
radio and none was answered, most likely because GPS is not enabled in its
codeplug. The receive and decode path is there; the request half is not,
because a builder known not to work makes the next person debug the radio
instead of the request.

## Tests

```bash
ctest --test-dir build -L mototrbo   # mototrbo_test_xnl, mototrbo_test_control, mototrbo_test_nai
```

All three are `unit`. `mototrbo_test_xnl` is mostly `static_assert`s; the
executable exists for the cipher round trip, which needs a decrypt the
library does not provide. `mototrbo_test_control` checks channel control,
radio status and the `0xB4xx` broadcasts against captured traffic; the
display decode is the run-time half because it allocates.
`mototrbo_test_nai` covers the data services, of which only TMS has been
answered by a radio.

Where the tests get their authority: `tests/golden/hardware_vectors.h` holds
bytes the radio actually sent or accepted (serial `511TVMG951`, firmware
`R02.10.00.0001`), and most of the assertions against them are
`static_assert`s. It is the same argument `libs/gsof` makes, and it applies
harder here because this protocol is reverse-engineered: a vector written
from the same reading of the protocol as the parser agrees with the parser
precisely where both are wrong. The authentication pairs are the strongest of
them, for the all-zero-reply reason above.

Two things in the tree are synthetic and labelled as such: the framing of a
`RadioStatus` reply (`<result><item><value>`) comes from the vendor client's
own decoder rather than from a captured reply, so the vectors wrap real values
(this radio's DMR id and model number) in that framing. `parse_status`'s
echoed-item check exists for exactly this reason.
