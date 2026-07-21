# Wired CarPlay Bring-Up & Test Plan

This is the iterative test plan for the native wired CarPlay stack. The code was
written in bulk on a macOS dev box **without** an iPhone, MFi coprocessor, or Linux
host attached, so **none of the hardware paths have been executed yet**. This
document is the script for stepping through them on a Linux host, in order.

Work the stages in sequence — each one depends on the previous. Every stage lists
what to run, what you should observe, and how to triage the common failures.

## 0. Conventions that make failures observable

**Log prefixes.** Every layer tags its messages so `grep` isolates a stage:

| Prefix | Layer | Source |
|---|---|---|
| `[usb]` | device detect, config-6 switch | `libs/apple_usb/usb_device.cpp` |
| `[muxd]` | usbmux TCP-over-USB | `libs/apple_usb/muxd.cpp` |
| `[usbmuxd]` | usbmuxd socket bridge | `libs/apple_usb/usbmuxd_server.cpp` |
| `[carkit]` | lockdown / TLS / carkit service | `libs/apple_usb/lockdown.cpp` |
| `[iap2]` | iAP2 link layer + control messages | `libs/iap2/` |
| `[mfi]` | MFi coprocessor auth | `libs/iap2/mcp2221a_mfi_signer.cpp` |
| `[ncm]` | NCM ↔ TAP bridge | `libs/apple_usb/ncm_bridge.cpp` |
| `[airplay]` | RTSP/AirPlay session | `libs/airplay/` |
| `[video]` / `[audio]` | media streams | `libs/airplay/` |
| `[node]` | zenoh publishing / orchestration | `nodes/carplay/` |

Run the driver with `--verbose` for `SPDLOG_DEBUG` output. Filter noise with e.g.
`./nodes/carplay/carplay --verbose 2>&1 | grep -E '\[muxd\]|\[carkit\]'`.

**Hardware-free unit tests.** These must pass before touching hardware — they are
the regression net for the pure-logic layers:

```bash
cmake --build build -j
./build/libs/airplay/airplay_test_tlv8        # TLV8 encode/decode + fragmentation
./build/libs/airplay/airplay_test_crypto      # HKDF/ChaCha20/X25519/Ed25519/SRP KATs
./build/libs/airplay/airplay_test_plist       # binary plist round-trip
./build/libs/airplay/airplay_test_nalu        # avcC -> Annex-B rewrite
./build/libs/iap2/iap2_test_framing           # 0xFF5A link-layer framing round-trip
```

A failure here is a logic bug, not a hardware problem — fix before proceeding.

**Simulation mode — the dashboard side needs no hardware at all.** The driver
node can publish a synthetic session (encoded H.264 test pattern, a 440 Hz PCM
tone, and rotating now-playing/nav metadata) on the real zenoh topics:

```bash
./build/nodes/carplay/carplay --simulate --verbose      # terminal 1
./build/dashboard/dashboard -c configs/dashboard/carplay_demo.yaml   # terminal 2
```

You should see the moving test pattern with a sweeping white box, hear the tone,
and watch the now-playing widget cycle tracks. Touching the video area logs
input events in terminal 1. `--sim-width/--sim-height/--sim-fps` adjust the
stream. Use this to isolate *any* dashboard-side problem from the phone: if
something is broken in simulation, it is not a CarPlay bug.

**Already verified on macOS with simulation** (2026-07-20): video decode +
render, audio sink startup, metadata → widget flow, widget instantiation from
YAML, and the publish rates below. This means stages 8–10 are exercising only
the *phone-side* half of those paths.

```
inspect hz -k nodes/carplay/video       ->  30 msgs/s   (matches --sim-fps)
inspect hz -k nodes/carplay/audio       ->  50 msgs/s   (20 ms PCM chunks)
inspect hz -k nodes/carplay/nowplaying  ->   1 msgs/s
```

This also retires the plan's "zenoh video throughput" risk: a 4 Mbit/s 30 fps
H.264 stream rides zenoh peer-to-peer on localhost without backpressure, so the
shared-memory fallback is not needed.

## 1. Host prerequisites (Linux)

```bash
sudo apt install libimobiledevice-dev libplist-dev libusbmuxd-dev \
                 libavcodec-dev libssl-dev iproute2
```

Then reconfigure and confirm the lockdown path is actually compiled in — if this
warns, every later stage will fail at stage 4:

```bash
cmake -S . -B build && cmake --build build -j 2>&1 | grep -i apple_usb
# WANT: "apple_usb: libimobiledevice found; wired CarPlay lockdown enabled"
# BAD:  "libimobiledevice/libplist not found; wired CarPlay lockdown disabled"
```

**Privileges.** The driver needs raw USB and TUN access. For bring-up just run it
as root; for deployment add a udev rule granting the Apple VID to a group and
grant `CAP_NET_ADMIN` for the TAP device.

**Conflicting daemons.** The system `usbmuxd` will fight us for the phone (this is
the core reason we run our own mux). Stop it before testing:

```bash
sudo systemctl stop usbmuxd.socket usbmuxd.service
```

## 2. USB detection and the config-6 switch

Plug in an unlocked, trusted iPhone.

```bash
sudo ./build/nodes/carplay/carplay --verbose 2>&1 | grep '\[usb\]'
```

**Expect:** the phone enumerated at VID `05ac` with its UDID, then a transition to
`bConfigurationValue` 6. Verify independently:

```bash
cat /sys/bus/usb/devices/<dev>/bNumConfigurations   # want >= 6 after the vendor request
cat /sys/bus/usb/devices/<dev>/bConfigurationValue  # want 6
```

**Triage.**
- Stuck at 4 configurations → the `0xC0/0x52` vendor request failed; check for
  `EPERM` (run as root) or that the phone is unlocked and trusted.
- Config reverts to 4 → something re-enumerated it, usually the system usbmuxd
  (stage 1) or `usb_storage`/`ipheth` grabbing the device.
- Device vanishes after the switch → expected briefly; the code waits up to 5s for
  re-enumeration. If it never returns, try a different cable/port (some cables are
  charge-only).

## 3. usbmux TCP-over-USB

```bash
sudo ./build/nodes/carplay/carplay --verbose 2>&1 | grep -E '\[muxd\]|\[usbmuxd\]'
```

**Expect:** interface 1 claimed, the version/setup handshake completing, then
`[usbmuxd] serving <udid8> on /tmp/...sock`, and the socket present on disk.

**Triage.**
- `could not claim mux interface` → another driver holds it; check `lsusb -t` and
  unbind the kernel driver, or confirm stage 1's usbmuxd stop.
- `usb reader ended` immediately → wrong endpoints for this device generation;
  confirm `EP_IN 0x85` / `EP_OUT 0x04` against `lsusb -v` for config 6.
- Connect attempts time out (`mux connect ... failed`) → the SYN/ACK handshake
  isn't completing; enable debug and check `[muxd]` RST logs. An immediate RST
  usually means the phone rejected the port (wrong lockdown port) rather than a
  framing bug.

## 4. Lockdown pairing + carkit TLS channel

This is the stage that depends on libimobiledevice talking to *our* socket.

```bash
sudo ./build/nodes/carplay/carplay --verbose 2>&1 | grep '\[carkit\]'
```

**Expect:** `[carkit] carkit TLS channel up (iAP2) udid=xxxxxxxx`.

Sanity-check our socket independently with the stock tools — this isolates
"our mux is broken" from "our lockdown call is broken":

```bash
USBMUXD_SOCKET_ADDRESS=UNIX:/tmp/<our-socket> idevice_id -l   # should list the UDID
USBMUXD_SOCKET_ADDRESS=UNIX:/tmp/<our-socket> ideviceinfo     # should dump device info
```

**Triage.**
- `idevice_id -l` empty → our `UsbmuxdServer` ListDevices reply is wrong; check the
  plist packet header framing (little-endian length/version/message/tag).
- Handshake fails with a pairing error → tap "Trust" on the phone; confirm pair
  records are being written under `--state-dir`. Delete the state dir to force a
  fresh pair.
- `could not start com.apple.carkit.service` → the phone did not expose the service.
  Confirm it is genuinely in config 6 (stage 2) — carkit only exists there.
- TLS enable fails → version skew in libimobiledevice; try a newer release.

## 5. iAP2 link layer, identification, MFi auth

```bash
sudo ./build/nodes/carplay/carplay --verbose 2>&1 | grep -E '\[iap2\]|\[mfi\]'
```

**Expect:** link SYN/ACK established, identification accepted, MFi certificate read
and a challenge signed, then the phone reporting CarPlay availability.

**Triage.**
- No `[mfi]` certificate → coprocessor not reachable. Test it standalone first with
  the existing demo: `./build/libs/apple_mfi_ic/apple_mfi_demo` (verifies the
  MCP2221A bridge and I²C address 0x11 independently of CarPlay).
- Identification rejected → the phone lists which components it refused; the code
  logs them. Usually a required message is missing from the sent/received lists.
- Challenge signature rejected → check the protocol major version (2 ⇒ SHA-1/20B,
  3 ⇒ SHA-256/32B); signing the wrong digest length fails silently-ish.
- Link resets repeatedly → checksum or sequence handling; `iap2_test_framing`
  should have caught pure framing bugs, so suspect retransmission/EAK logic.

**⚠ If the phone reports CarPlay availability but the session never starts,
check this first.** LIVI decodes a zero-length `bool` iAP2 parameter as `None`,
which makes `CarPlayAvailability.wired_available` falsy and silently skips
sending `CarPlayStartSession`. That behaviour was ported faithfully (returns
`nullopt`) but the spec arguably intends presence-as-value here. It is logged at
DEBUG — run with `--verbose` and grep for the availability decode. Flipping
zero-length bools to `true` is the one-line experiment.

**Other iAP2 caveats to keep in mind:**
- Outbound fragmentation chunks at `max_len - 10` (header+checksum overhead),
  where LIVI chunks at `max_len`. Invisible on the wired path (65535, small
  messages) but it matters if a phone advertises a small `max_len`.
- Only the *wired* carkit identification is implemented. Bluetooth/wireless
  transport components are deliberately not encoded.
- Retransmission/EAK timers are a structural port that has never run against a
  phone in either codebase — only the zero-ack wired path is exercised in
  practice. Suspect them if the link is unstable under load rather than at setup.
- The link layer treats an empty `recv()` as "no data yet", never as EOF. The
  transport must signal death some other way or a dead link will spin; watch for
  this when `CarkitChannel` is wired to `Iap2Transport`.

**Verified without hardware:** `iap2_test_framing` covers 20 groups / ~150
assertions — byte-exact checksums, header round-trips, the full start sequence,
SYN|ACK negotiation, RST, corrupted-payload drop-and-retransmit, inbound
reassembly (1 CSM over 3 packets, 2 CSMs in 1 packet), outbound fragmentation
against a 64-byte device, out-of-sequence hold/release, EAK emission and
EAK-driven retransmission, the CSM parameter codec incl. nested groups,
identification encode + rejection handling, route-guidance merge in both arrival
orders, call/power/cellular decode, the MFi authenticator on both protocol
majors, and an end-to-end identification+auth handshake over the link layer.

## 6. NCM/TAP link

```bash
sudo ./build/nodes/carplay/carplay --verbose 2>&1 | grep '\[ncm\]'
ip -6 addr show cpusb0     # expect an fe80::/64 link-local address
ping6 -c3 fe80::<phone>%cpusb0
```

**Expect:** the NCM interface pair claimed, `cpusb0` created and up, and the phone
answering on its link-local address.

**Triage.**
- No `cpusb0` → `/dev/net/tun` missing or no `CAP_NET_ADMIN`.
- Interface up but no ping → the kernel `cdc_ncm` driver may have claimed the
  interface; it must be unbound so we can drive it from userspace.
- Ping works but no inbound TCP → check that `CarPlayStartSession` was sent with
  the correct accessory fe80 address and port 7000.
- `"bulk endpoints not found"` → endpoint discovery reads sysfs `ep_*` right
  after `USBDEVFS_SETINTERFACE`, assuming sysfs repopulates synchronously. If
  this proves racy it needs a retry loop — this is a known guess. Also note
  altsetting 1 is hardcoded for the data interface (as in LIVI).
- `stop()` appears to hang for ~2s → by design; threads are joined rather than
  having their fds yanked. Longer than that means a usbfs bulk IN ignored its
  timeout, and there is no `USBDEVFS_DISCARDURB` escape hatch.

**Note on the fe80 address.** `linkLocalAddress()` returns *our* (the head
unit's) address, derived EUI-64 from the TAP's MAC after it is set — that is the
address that goes into `CarPlayStartSession` and that the phone dials at :7000.
It is not the phone's address. The bridge re-reads
`/sys/class/net/<if>/address` after setting the MAC so a failed `ip link set
address` cannot desync what we advertise from what is on the wire.

**Verified by differential testing against the Python** (no hardware): NTB16
block construction is byte-identical to LIVI's `_build_ntb` across 30 cases
(frame sizes 60–9000, sequence counts including the 512-byte padding boundary
and 16-bit wrap); NTB parsing matches `_parse_ntb` on 10 malformed/chained
blocks; EUI-64 derivation matches on 9 MACs. So framing bugs are unlikely —
suspect enumeration, altsetting, or privileges first.

**Known perf limitation:** TX sends one ethernet frame per NTB block (no
aggregation), i.e. one bulk transfer per frame. If uplink throughput is a
problem under video-heavy load, this is the thing to fix.

## 7. AirPlay handshake through RECORD

```bash
sudo ./build/nodes/carplay/carplay --verbose 2>&1 | grep '\[airplay\]'
```

**Expect, in order:** inbound TCP on `[fe80::...]:7000`, `/pair-setup` (SRP)
completing, `/pair-verify` completing, `/auth-setup` (MFiSAP) completing,
`GET /info` answered, `SETUP` for stream 110 (main screen), `RECORD`, then a
`VideoConfig` (avcC) arriving.

**The crypto primitives are proven; suspect labels and framing, not math.**
`airplay_test_crypto` is 90 assertions against published vectors: SHA-1/256/512,
HKDF-SHA512 (RFC 5869 TC1, plus multi-block expansion and the real
`Pair-Setup-Encrypt` / `Control-Salt` labels), X25519 (RFC 7748 §5.2 and §6.1
both sides, plus low-order-key rejection), Ed25519 (RFC 8032 §7.1 key/sign/verify
plus mangled-signature/message/key rejection), ChaCha20-Poly1305 (RFC 8439
§2.8.2 plus AAD/tag/nonce tamper rejection), AES-128-CTR (NIST SP 800-38A
F.5.1), `nonce64`/`nonceLabel` byte layout, and a **full SRP-6a KAT** — verifier
`v`, server `B`, client `A`, session key `K`, and both proofs `M1`/`M2` — with
negative tests for `A = 0`, `A = N`, wrong password, wrong username, and mangled
proofs.

So if pair-setup or pair-verify fails on hardware, the arithmetic is almost
certainly fine. Look at message framing, TLV ordering, and which bytes get fed
to each hash — not the primitives.

**Triage.**
- pair-setup fails → check the TLV8 sequence and that the SRP username is
  exactly `Pair-Setup`; the SRP math itself is KAT-verified.
- pair-verify fails → X25519/Ed25519 key handling or the HKDF labels
  (`Pair-Verify-*`, `Control-Salt`, `Events-Salt`).
- auth-setup fails → MFi signature over the wrong bytes; stage 5 must pass first.
- `/info` accepted but no SETUP → the phone rejected our advertised capabilities;
  we advertise **H.264 only** by design (no `hevcInfo`). Log the raw `/info` we sent
  and compare against a known-good capture.
- Everything up to RECORD but no video → check the event channel keying.

## 8. Video + touch (usable CarPlay)

Terminal 1: `sudo ./build/nodes/carplay/carplay --verbose`
Terminal 2: `./build/dashboard/dashboard -c configs/dashboard/carplay_demo.yaml`

Independently confirm the zenoh contract before blaming the widget:

```bash
./build/nodes/inspect/inspect hz   nodes/carplay/video    # expect ~30-60 Hz
./build/nodes/inspect/inspect dump nodes/carplay/session
./build/nodes/inspect/inspect dump nodes/carplay/input    # then touch the widget
```

**Expect:** the CarPlay UI renders and responds to touch. Kill and restart the
dashboard — video must recover (the driver keeps the phone session; the widget
waits for the next config+keyframe).

**Triage a black video area by reading the widget's log** — it narrates every
stage of the video path, so you can tell exactly where it stops:

| Log line | Meaning |
|---|---|
| (nothing) | no `CarPlayVideo` messages arriving — check `inspect hz`, keys, zenoh |
| `video decoder ready (H.264)` | messages arrive, decoder opened |
| `dropped N frame(s) waiting for a keyframe/config` | arriving but no sync point yet — **the driver must publish config or a keyframe periodically, not once** |
| `video synced on parameter sets/keyframe` | sync achieved |
| `decoder rejected N packet(s)` | bitstream problem — bad Annex-B rewrite, or parameter sets fed as a standalone access unit |
| `cannot convert decoded frame to RGB` | decoded, but not YUV420P — the converter only handles that format |
| `first video frame decoded and rendered (WxH)` | **the picture is live**; if the screen is still black, suspect widget geometry/layout, not video |

**Design requirement this exposed:** zenoh has no retained/latched messages, so
a one-shot `VideoConfig` leaves any subscriber that starts later — or restarts —
permanently black. The driver **must republish the parameter sets before every
keyframe** (the simulator does this; the real AirPlay path must too), and the
widget syncs on either config *or* a keyframe since Annex-B keyframes carry
SPS/PPS in band. Verified: a dashboard started 8 s into a running session syncs
within one GOP (~2 s) and renders.

Also note the widget caches a config message and prepends it to the next access
unit rather than feeding it to the decoder alone — parameter sets by themselves
are not a decodable access unit and produce `AVERROR_INVALIDDATA`.
- Frames stall after a while → zenoh backpressure on large keyframes; measure
  before switching to shared memory.
- Touch does nothing → verify `nodes/carplay/input` carries events (`inspect dump`),
  then check the 0..10000 → 0..1 rescale and HID report.

## 9. Audio downlink

**Expect:** music and navigation prompts play through the widget's `QAudioSink`;
`inspect hz nodes/carplay/audio` shows a steady rate matching the sample rate.

**Triage.** Choppy audio is almost always pacing, not decode: confirm the jitter
ring is sized to the negotiated latency. Verify decode independently by dumping
PCM to a file and playing it with `aplay`.

## 10. Metadata, mic, and supplemental widgets

```bash
./build/nodes/inspect/inspect dump nodes/carplay/nowplaying   # play music
./build/nodes/inspect/inspect dump nodes/carplay/nav          # start navigation
./build/nodes/inspect/inspect dump nodes/carplay/call         # place a call
```

Then Siri/mic (two-way call audio), and finally the supplemental now-playing
widget rendering the same topic the `inspect` dump showed.

## What exists today (read before starting)

Not all stages below are implemented yet. Current state:

| Layer | State |
|---|---|
| USB detect, config-6 switch, usbmux, usbmuxd socket, lockdown (libimobiledevice) | written, never run |
| iAP2 link layer, identification, MFi auth, metadata decode | written, unit-tested |
| NCM ↔ TAP bridge | written, differentially verified vs LIVI |
| AirPlay crypto/SRP/plist/NALU foundation | written, KAT-verified |
| **AirPlay RTSP session** (pair-setup/verify, auth-setup, /info, SETUP, RECORD, HID, screen + audio streams) | **NOT YET WRITTEN** |
| **Node orchestration** wiring apple_usb → iap2 → airplay | **NOT YET WRITTEN** — `nodes/carplay/main.cpp` still runs a placeholder loop |
| zenoh bridge, widgets, audio, metadata topics | done, verified via `--simulate` |

So stages 2–6 can be attempted now; **stages 7–10 need the AirPlay session layer
and the node orchestration first**. Until then the driver publishes only idle
session state, and `--simulate` is the way to exercise the dashboard.

## Known-unverified list

Nothing in the USB → iAP2 → AirPlay path has executed against a phone. Ranked by
remaining uncertainty (highest first):

1. **usbmuxd socket bridge** (`usbmuxd_server.cpp`) — plist packet framing and the
   `Connect` relay have *no test coverage at all*. Stage 4's `idevice_id -l` check
   is the first real exercise of it. This is now the weakest link.
2. **Lockdown/carkit glue** (`lockdown.cpp`) — never compiled on this machine
   (libimobiledevice absent on macOS); stage 1's build check is its first
   compile. Watch for API drift in `idevice_new_with_options` / SSL enable.
3. **Zero-length iAP2 bools** → `CarPlayStartSession` silently never sent
   (stage 5). Cheap to check, high impact.
4. **Audio pacing** — timing-dependent, cannot be desk-checked.
5. **NCM enumeration** (not framing) — sysfs endpoint discovery after
   `SETINTERFACE`, and the hardcoded altsetting 1.
6. **iAP2 retransmission/EAK timers** — structural port, never exercised in
   either codebase on the wired zero-ack path.

Deliberately *lower* risk than they look, because they are verified:
NTB16 framing (byte-identical to LIVI across 30 cases), the crypto primitives
and SRP (90 KAT assertions), iAP2 framing/reassembly/fragmentation (~150
assertions), and the entire dashboard-side pipeline (`--simulate`).
