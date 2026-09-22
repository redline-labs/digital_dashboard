---
title: airplay
parent: Libraries
---

# airplay

## Overview

The CarPlay AirPlay/RTSP session stack, server side: the RTSP receiver the
phone dials on port `7000` of the accessory's NCM link-local address after
`CarPlayStartSession`, the three pairing handshakes and the identity they
establish, the encrypted control and event channels, the HID devices the
accessory presents, the `GET /info` capability declaration, the screen and
audio streams with their Annex-B rewriting and AAC decode, the microphone
uplink, and the NTP-style clock sync without which the phone tears the session
down. It was adapted from LIVI's TypeScript stack and needs OpenSSL for the
crypto and libavcodec for AAC-LC; without OpenSSL the library is skipped with
a warning.

It binds sockets and runs threads, but knows nothing about USB, iAP2 or how
the phone came to have an address; that is [apple_usb](apple_usb.html) and
[iap2](iap2.html). The MFi coprocessor reaches it only as three callbacks in
`ReceiverConfig`, so there is no dependency on
[apple_mfi_ic](apple_mfi_ic.html). The recurring design rule is that anything
that can fail silently, which is almost everything here, is split into a pure
function or a state machine over byte strings so a test can hold it still.
The node that hosts the receiver and publishes its output on zenoh is
[carplay](../nodes/carplay.html); the design is in
[carplay-port](../design/carplay-port.html).

## Public headers

| Header | |
| --- | --- |
| `airplay/receiver.h` | `Receiver`: the RTSP server, session lifecycle, and the input, night mode, Siri, microphone and keyframe entry points. |
| `airplay/config.h` | `ReceiverConfig`, `PrimaryInput`, `kMainDisplayUuid`: what the accessory is, as the phone sees it. |
| `airplay/info_plist.h` | `buildInfoPlist`: the `GET /info` declaration as a pure function of the config. |
| `airplay/rtsp.h` | `rtsp::Message`, `parseRequest`, `serializeResponse`, `makeResponse`, `isResponse`. |
| `airplay/pairing_session.h` | `PairingSession`: `/pair-setup`, `/pair-verify`, `/auth-setup`, the control keys and the verify shared secret. |
| `airplay/pairing_store.h` | `PairingStore`: the accessory's long-term identity and each phone's public key, on disk at `0600`. |
| `airplay/srp.h` | SRP-6a server and client (RFC 5054 3072-bit group, SHA-512). |
| `airplay/crypto.h` | X25519, Ed25519, HKDF-SHA512, SHA-1/256/512, ChaCha20-Poly1305, AES-128-CTR, nonces, random bytes. |
| `airplay/tlv8.h` | HAP TLV8 encode and decode, with fragmentation past 255 bytes. |
| `airplay/channel_crypto.h` | `ChannelCrypto`: the framed, counted ChaCha20-Poly1305 transport under both encrypted channels. |
| `airplay/event_channel.h` | `EventChannel`: the second TCP connection, carrying input out and the phone's commands back. |
| `airplay/event_queue.h` | `EventQueue`: ordering, coalescing, rate and drop policy for outbound events, with no clock of its own. |
| `airplay/hid.h` | The four HID devices (touch, knob, media keys, telephony): descriptors, `/info` entries, reports. |
| `airplay/oem_button.h` | `OemButtonConfig`, `addOemButtonInfo`, `isOemButtonPress`: the manufacturer tile on CarPlay's home screen. |
| `airplay/screen_modes.h` | `buildChangeModesCommand`, `buildRequestUiCommand`, `parseScreenOwner`: handing the main screen between phone and car. Unconfirmed on hardware; the evidence for each constant is in the source. |
| `airplay/media_stream.h` | `VideoPacket`, `AudioPacket`, `runScreenStream`, `runAudioStream`. |
| `airplay/nalu.h` | avcC/hvcC to Annex-B rewriting, codec detection, keyframe detection for H.264 and H.265. |
| `airplay/aac_decoder.h` | `AacDecoder`: raw AAC-LC access units to interleaved S16 PCM via libavcodec. |
| `airplay/mic_uplink.h` | `MicUplink` and the `mic::` packet builders for captured audio going to the phone. |
| `airplay/timing.h` | `ntp::` timestamp arithmetic and `TimingSync`, the mandatory clock sync. |
| `airplay/net.h` | Dual-stack ephemeral TCP listener and UDP socket. |

## Using it

Link the CMake target `airplay`; it carries `plist`, `helpers` and OpenSSL
publicly. `startAirPlayReceiver` in `nodes/carplay/usb_pipeline.cpp` is the
reference caller: fill a `ReceiverConfig`, hand it the MFi callbacks, install
handlers, start, and route input to it.

```cpp
using airplay::Bytes;
airplay::ReceiverConfig cfg;
cfg.bind_address = ncm.scopedLinkLocal();   // not the wildcard; see below
cfg.width = 800; cfg.height = 600; cfg.fps = 30;
cfg.state_dir = state_dir;                  // same directory as the pair records
cfg.mfi_certificate = [&] { return signer.certificate().value_or(Bytes{}); };
cfg.mfi_sign = [&](const Bytes& d) { return signer.signChallenge(d).value_or(Bytes{}); };
cfg.mfi_protocol_major = [&] { return signer.protocolMajor(); };

airplay::Receiver receiver(cfg);
receiver.setVideoHandler([&](const airplay::VideoPacket& p) { /* Annex-B out */ });
receiver.setAudioHandler([&](const airplay::AudioPacket& p) { /* S16LE PCM out */ });
receiver.setNightMode(night);               // before start(); pushed at RECORD
receiver.start();
receiver.sendTouch(x, y, airplay::Receiver::TouchPhase::Down);
```

The node binds the AV link's address rather than the wildcard because on
macOS the system AirPlay Receiver already holds `*:7000`; binding the one
link-local on the same port succeeds alongside it, and the phone only ever
dials the address it was given.

## Behaviour worth knowing

Three handshakes run before a session: `/pair-setup` is SRP in transient
mode with the well-known password `3939` (overridable with
`AIRPLAY_SETUP_PASSWORD`), `/pair-verify` an ephemeral X25519 exchange signed
with the long-term Ed25519 keys, `/auth-setup` MFiSAP. With the MFi callbacks
empty, auth-setup answers `501` and the session stops. A protocol error goes
back as a TLV error inside a `200`, because an RTSP error aborts the
connection instead of letting the phone retry. A failed step is also
sticky: the phone closes the connection and waits, so `PairingSession::failed()`
turns true and the receiver reports it once through `setHandshakeFailedHandler`,
for the owner to end the session. Every failure here is silent on
the wire.

On the wired path the phone re-runs pair-setup every session regardless of
what is stored, measured 2026-08-02: it sends `X-Apple-HKP: 0`, transient
pairing, because there is no Bonjour advertisement to recognise us by.
`PairingStore` still earns its place: the accessory identity stops changing on
every restart, and pair-verify's M3 signature is checked against a key on
file rather than one handed over moments earlier, the only form of the check
that means anything. A store that exists and cannot be parsed is left alone.

{: .warning }
The two encrypted channels key their directions differently. On the control
channel the naming follows HAP, so the controller reads with
`Control-Read-Encryption-Key` and that is the accessory's outbound key; on
the event channel the accessory writes with `Events-Write` and reads with
`Events-Read`. Getting either backwards looks like a bad key.

`ChannelCrypto` is stateful because each direction counts its own nonce from
zero. Losing a frame or sealing two out of order desynchronises the counter,
every later frame fails to authenticate, and nothing distinguishes that from
a wrong key, so a failed open is fatal for the channel. Pair-verify M4 is the
last plaintext on the control channel; the event channel is encrypted from
its first byte.

`GET /info` is the most consequential message in the session. A subtly wrong
one produces no error: the phone accepts the session and tears it down
without asking for a stream, indistinguishable from a transport fault. The
main display's UUID must be identical in the display entry, the HID devices
attached to it, and keyframe requests; the touch descriptor carries absolute
coordinates sized to the display, so `EventChannel::Config` must match what
`/info` declared or every touch lands in the wrong place.

The event channel is bidirectional: the phone pushes its own commands and
expects each acknowledged. `rtsp::Message::isResponse` exists because a
status line parses happily as a request, and answering a reply starts a
ping-pong measured at about `1000` messages a second. One thread pumps the
socket and one drains the outbound queue; no producer blocks on it, since
`sendTouch` runs on the zenoh subscriber thread. The channel must be
listening before the SETUP response goes out, because the phone dials it
immediately and a connection left in the backlog reads as a dead channel.

`EventQueue` holds the policy with no threads or clock. Touch is strictly
ordered; consecutive moves coalesce but down and up never do, so a drag cannot
become a tap somewhere else. Control commands (knob, media and phone keys,
Siri) never coalesce, overtake touch, and are not rate limited. A keyframe
request is a flag that overtakes everything, because it is what recovers a
black screen for a renderer that joined late; zenoh has no retained messages
and a static screen yields only P-frames after the first IDR. Touch is paced
at `8 ms` minimum, the queue drops past `64` entries, and `clear()` on
session end discards an orphaned gesture rather than replaying a phantom
contact.

Night mode is remembered and pushed at RECORD, the earliest the phone accepts
it; sent earlier it is ignored and stalls older iOS for seconds. Siri is a
down/up click, not press-and-hold, which would submit on release. The
manufacturer button press arrives as a `requestUI` with no url. Its icon
must be `prerendered = true`: on iPhone17,1 with AirPlay 950.7.1, `false`
renders an empty tile, verified both ways on 2026-08-02.

Clock sync is not optional: without it the phone tears the session down a few
seconds after RECORD, which reads as an unstable link. The first exchange
cannot be expressed as an offset, because the phone's NTP domain is roughly
`4e9` seconds from a boot-based clock, past the signed 32-bit window, so the
first sample adopts the phone's clock outright.

The phone chooses the codec and picks H.264 in practice; `allow_hevc` is off
by default because turning it on hands the choice to the phone, which will
take it. Both paths are wired because the keyframe rule differs and guessing
wrong is silent. Entertainment audio (stream type `102`) is raw AAC-LC with
no ADTS header, so `AacDecoder` is configured up front from the negotiated
format. The microphone uplink is signalled only by the phone naming a
`dataPort` in a main-audio SETUP; PCM arrives S16LE and must leave big-endian
in whole frames, remainder held back rather than padded, or the audio reaches
the phone as clicks.

`PairingSession` is not thread safe; one connection drives it in order.
`MicUplink::feed` and every `Receiver::send*` are safe from any thread.

## Tests

```bash
ctest --test-dir build -L airplay
```

Fourteen targets, all labelled `airplay` and `unit`, all runnable on macOS
without a phone: the handshakes are driven from a stand-in phone built from
the same primitives, and nothing opens a socket to real hardware.

| Target | What it proves |
| --- | --- |
| `airplay_test_screen_modes` | The `changeModes` and `requestUI` bodies decode to exactly the expected tree, and the owner parser answers "does not say" for every malformed `modesChanged`. The fixtures are synthetic until a captured body replaces them. |
| `airplay_test_tlv8` | Round trips, an empty value, fragmentation at and past 255 bytes. |
| `airplay_test_crypto` | Known answers from RFC 7748, RFC 8032, RFC 8439, SP 800-38A, FIPS 180-4 and RFC 5869; SRP-6a has no published vector, so its values came from an independent Python model of the exchange, plus a second vector whose public key `A` has a zero top byte: the proofs hash `A` and `B` minimal, as the phone does, where LIVI's `srp.ts` pads them. |
| `airplay_test_channel_crypto` | Frame layout, both directions opening, several frames in one pass, and the counter desynchronisation that has no diagnostic on hardware. |
| `airplay_test_pairing_session` | All three handshakes end to end against a real SRP client, X25519 and Ed25519. Cannot prove Apple's phone agrees; pins our reading so it stops moving. |
| `airplay_test_pairing_store` | The identity survives a restart, the file is `0600`, an empty directory disables the store. |
| `airplay_test_rtsp` | Requests split at awkward bytes, two in one read, binary bodies containing the header terminator or a NUL, response detection. |
| `airplay_test_info_plist` | The keys the phone reads are present, spelled as it expects, and consistent. It cannot prove the declaration correct; it stops it changing by accident. |
| `airplay_test_hid` | Each descriptor is structurally valid and its declared report size matches the reports built; a mismatch means the phone silently discards every report. |
| `airplay_test_oem_button` | What `/info` advertises and how the press is told apart from other event traffic. |
| `airplay_test_event_queue` | Coalescing, gesture order, overtaking, rate limiting by waiting not dropping, bounded queues, `clear`. Time is injected, so it is deterministic. |
| `airplay_test_nalu` | avcC/hvcC to Annex-B, codec detection, keyframe detection for both codecs. |
| `airplay_test_mic_uplink` | The packet's wire form: endianness swap, RTP header, sealed body, trailing nonce, frame sizing. |
| `airplay_test_timing` | NTP packing and the offset arithmetic, including a first exchange with an arbitrarily distant phone clock. |
| `airplay_test_aac` | A sine wave through libavcodec's AAC-LC encoder and back through `AacDecoder`; links libavcodec directly for the encoder side. |
