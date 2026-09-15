---
title: iap2
parent: Libraries
---

# iap2

## Overview

The iAP2 accessory protocol, transport-agnostic, plus the MFi authentication
signer it needs: the link layer (detection marker, SYN negotiation, sequence
numbers, ACK and EAK, retransmission, fragmentation), the control session
message codec, the catalogue of messages a wired CarPlay accessory exchanges
with the phone (identification, authentication, session start, now playing,
route guidance, calls, power, location), and NMEA sentence generation for the
GPS uplink. It was adapted from LIVI's Python iAP2 stack and carries the same
defaults, down to the first data packet being sequence `100`.

It never sees USB, Bluetooth or TLS. The link layer is written against a
two-method `Iap2Transport`, and `apple_usb::CarkitChannel` has exactly that
shape, but it is deliberately not referenced here so this library stays free
of [apple_usb](apple_usb.html). The one hardware dependency is the MFi
coprocessor, through [apple_mfi_ic](apple_mfi_ic.html) and therefore OpenSSL.
The `MfiSigner` interface it defines is also what [airplay](airplay.html)'s
`/auth-setup` is fed from. The node that drives a session is
[carplay](../nodes/carplay.html); the design is in
[carplay-port](../design/carplay-port.html).

## Public headers

| Header | |
| --- | --- |
| `iap2/link_layer.h` | `Iap2Transport`, `LinkLayer`, `LinkConfig`, `LinkPacketHeader`, `LinkSynchronizationPayload`, the checksum, and the control bits and session ids. |
| `iap2/csm.h` | The control session message wire format: `csm::Param`, `ParamList`, typed `add*` and `get*` helpers, `encodeMessage`, `parseMessage`, `peekLength`. |
| `iap2/messages.h` | `MessageId`, `IdentificationConfig` and its rejection handling, `MfiAuthenticator`, and the encode and decode functions and state folders (`NavGuidance`, `CallTracker`) for every message the accessory speaks. |
| `iap2/mfi_signer.h` | `MfiSigner`: certificate, challenge signing, protocol major version. |
| `iap2/mcp2221a_mfi_signer.h` | `Mcp2221aMfiSigner`, the `MfiSigner` backed by `AppleMFIIC`. |
| `iap2/location_nmea.h` | `LocationFix`, `nmeaGga`, `nmeaRmc`, `appendNmeaChecksum`. |
| `iap2/byte_order.h` | `put_be16` and `get_be16`, shared by all three layers. |

## Using it

Link the CMake target `iap2`; it carries `apple_mfi_ic` publicly. The caller
is `runIap2Session` in `nodes/carplay/iap2_session.cpp`, which adapts the
carkit channel, builds a link, and dispatches control messages by id:

```cpp
class CarkitTransport : public iap2::Iap2Transport { /* forwards to CarkitChannel */ };

CarkitTransport transport(*carkit);
iap2::LinkLayer link(transport, iap2::LinkConfig{});   // wired defaults
iap2::Mcp2221aMfiSigner signer;
signer.init(mfi_i2c_device);
iap2::MfiAuthenticator auth(signer);

link.start();
link.waitNegotiated(5000);
while (link.poll(100)) {
    if (auto frame = link.receiveControlMessage(0)) {
        auto message = iap2::csm::parseMessage(*frame);
        std::vector<uint8_t> reply;
        if (auth.handle(*message, reply) == iap2::MfiAuthenticator::Result::kReply)
            link.sendControlMessage(reply);
    }
}
```

After `AuthenticationSucceeded` the node subscribes with
`encodeStartNowPlayingUpdates`, `encodeStartRouteGuidanceUpdates` and
`encodeStartCallStateUpdates`, answers `CarPlayAvailability` with
`encodeCarPlayStartSession` naming the NCM link-local and port `7000`, and
feeds `nmeaGga` and `nmeaRmc` into `encodeLocationInformation` when the phone
asks.

## Behaviour worth knowing

`LinkLayer` is single threaded and does not own its transport. The caller
drives it with `poll()`, or with the blocking helpers that call `poll`
internally, and every callback fires on the calling thread from inside
`poll()`. `sendControlMessage` before negotiation queues the frame, and a
frame longer than the negotiated maximum is split across packets.

The `LinkConfig` defaults are the wired ("carkit") ones: zero-ack, control
session version 2, and the accessory initiates negotiation rather than waiting
for the phone's detection marker.

{: .note }
`Iap2Transport::recv` must return empty on a timeout with no data, and the
link layer treats that as "no data yet". A transport that has died has to say
so through its own channel, which is why `CarkitChannel` carries `alive()`;
without checking it a dead link is polled forever.

Every length, identifier and parameter tag on the wire is big-endian 16-bit.
A control message is `0x4040 | length | message id` followed by parameters
each framed `length | id | payload`, and parameters are a flat list rather
than a map because the protocol allows the same id more than once and the
order is sometimes the only thing distinguishing them.

A zero-length boolean parameter reads as absent, not true. The iAP2 spec
allows presence to mean true; this follows LIVI in not doing so. If a phone
ever sends one, `CarPlayAvailability` reads falsy and the session never
starts, which is why the behaviour is pinned in `iap2_test_csm` rather than
left to be rediscovered. An integer read at the wrong width also reads as
absent rather than taking the wrong bytes.

`IdentificationConfig` defaults describe the accessory LIVI advertises: one
`USBHostTransportComponent` with CarPlay interface `3` and advanced power
providing. The phone may reject identification over any optional component;
`applyIdentificationRejection` clears the flagged one so the caller can
re-send, and returns false when the rejection names nothing droppable.
`advertise_wireless_carplay` is off because this accessory cannot hand over
to wireless, and advertising it makes the phone ask for Wi-Fi credentials on
every session.

`MfiSigner::protocolMajor` decides the challenge size: version 2 signs a
20-byte SHA-1 digest, version 3 a 32-byte SHA-256. `Mcp2221aMfiSigner::init`
reads it from the coprocessor's device info; despite the name it works over
whatever bus `AppleMFIIC` opens, including a native controller.

Of the four NMEA families the phone can request in
`StartLocationInformation`, only `$GPGGA` and `$GPRMC` are generated;
`$GPGSV` and `$GPVTG` are decoded as requests and not answered.

## Tests

```bash
ctest --test-dir build -L iap2    # iap2_test_framing, iap2_test_nmea, iap2_test_csm
```

All three are labelled `iap2` and `unit`, and all run on macOS: the link
layer is driven over a fake transport that buffers bytes in both directions,
and the MFi handshake over a fake signer, so no coprocessor is needed.

`iap2_test_framing` covers checksums, header and SYN payload round trips,
start and negotiation, the control session data path, reassembly of a
message split across packets, fragmented sends, out-of-sequence delivery, EAK
retransmission, sends queued before negotiation, CSM framing, identification
and rejection handling, the CarPlay session messages, route guidance and the
merged `NavGuidance` view, calls and power, the subscription messages, MFi
authentication against a fake signer, the whole handshake carried over the
link, transport failure, and the external accessory and file transfer
sessions.

`iap2_test_csm` goes at the codec's edges, where the input is a byte string
the phone chose and the wrong reading is silent: truncated and lying
parameter lengths, a parameter shorter than its own header (which would spin
forever), header-only parameters, the zero-length boolean rule, and integer
width mismatches.

`iap2_test_nmea` checks the sentences against a known fix: field layout,
`ddmm.mmmm` coordinate encoding, hemispheres, and the XOR checksum.
