# Wired CarPlay Bring-Up & Test Plan

This is the iterative test plan for the native wired CarPlay stack. The code was
written in bulk on a macOS dev box **without** an iPhone, MFi coprocessor, or Linux
host attached. This document is the script for stepping through the hardware
paths, in order.

Most of it assumes a Linux host, which is the target. Since 2026-08-01 **stages
1–4 and 6 also run on macOS**, verified on hardware — see "Running on macOS"
under Building. That is a genuinely different route through the same stack
(macOS supplies both the mux and the NCM link), so read that section before
following Linux-specific advice on a Mac.

**Reference implementation.** This stack is a port of LIVI
(https://github.com/f-io/LIVI, GPL-3.0). Its AirPlay/RTSP layer lives in
`src/main/services/projection/driver/cp/stack/` — `cpStack.ts` (request
dispatch), `getInfo.ts` (the `/info` plist), `timingServer.ts`, `screenStream.ts`,
`hid.ts`. When a handshake step is rejected by the phone and the reason is not
observable, read the corresponding file there rather than permuting: the
`/auth-setup` byte layout and the `/info` display keys were both settled that
way in minutes after an hour of guessing.

**Status: the full pipeline works end to end (2026-07-22) — the CarPlay home
screen renders live in the dashboard widget.** Stages 1–7 all run: USB config
switch, usbmux, lockdown/carkit TLS, iAP2 + MFi auth, the NCM link, and the
AirPlay session through to H.264 decoded and drawn on screen via zenoh. What
remains: audio streams, and confirming touch round-trips.

**Earlier status: stages 1–6 verified (2026-07-21)** — USB config switch,
usbmux, the usbmuxd socket bridge, lockdown/carkit TLS, the iAP2 link,
identification, MFi authentication, and the NCM ↔ TAP bridge. Stage 7 (the
AirPlay session and video) landed the next day.

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
cmake --build build -j4     # -j unbounded OOMs on an 8 GB box; zenoh's Rust build is the hog
./build/libs/airplay/airplay_test_tlv8        # TLV8 encode/decode + fragmentation
./build/libs/airplay/airplay_test_crypto      # HKDF/ChaCha20/X25519/Ed25519/SRP KATs
./build/libs/airplay/airplay_test_nalu        # avcC -> Annex-B rewrite
./build/libs/airplay/airplay_test_aac         # AAC-LC encode/decode round-trip (entertainment audio)
./build/libs/airplay/airplay_test_event_queue # event channel queueing
./build/libs/airplay/airplay_test_oem_button  # manufacturer button: /info keys + press decode
./build/libs/airplay/airplay_test_hid         # HID descriptors vs. the reports sent on them
./build/libs/plist/plist_test_binary          # binary plist round-trip
./build/libs/plist/plist_test_xml             # XML plist round-trip
./build/libs/plist/plist_test_libplist_vectors # differential vectors captured from libplist
./build/libs/iap2/iap2_test_framing           # 0xFF5A link-layer framing round-trip
./build/libs/iap2/iap2_test_nmea              # GPS location NMEA (GGA/RMC) generation + checksum
./build/libs/apple_usb/apple_usb_test_ncm_discovery # NCM descriptor discovery (real-hardware fixture)
./build/libs/apple_usb/apple_usb_test_ncm_frame     # NTB16 framing + EUI-64 link-local derivation
./build/libs/apple_usb/apple_usb_test_pair_record   # pair record mint/parse, X509_verify
./build/libs/apple_usb/apple_usb_test_usbmux_client # usbmux client framing
```

There is no `airplay_test_plist` — the plist tests live in `libs/plist` under the
`plist_test_*` names above. If you have one in a `build/` directory, it is a
stale binary from before the move and will keep passing after its target is
gone; that is a good reason to reconfigure from scratch rather than trust an old
build tree. `ctest` is not wired up, so run them directly as above.

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
sudo apt install libavcodec-dev libssl-dev iproute2
```

**There is no libimobiledevice dependency.** The whole Apple-side stack is in
this tree: property lists (`libs/plist`), the usbmux client and server, the
lockdown handshake, client-certificate TLS, and pairing (`libs/apple_usb`). Do
not install `libimobiledevice-dev` or `libplist-dev` expecting them to be used.

It was a vendored dependency until 2026-07-31 and was removed once our
implementation had replaced every part of it. What that dependency was for, and
what replaced it, is in stage 4 below.

The stock `libimobiledevice` command-line tools remain useful for debugging
because `UsbmuxdServer` speaks the standard usbmux protocol — if you happen to
have them installed, `USBMUXD_SOCKET_ADDRESS=UNIX:<our-socket> ideviceinfo`
still works against it. Nothing in the build needs them.

### Building on macOS

`apple_usb` is no longer Linux-gated. The library splits along a hardware line:

- **Portable** — `muxd.cpp` (the usbmux state machine), `usbmuxd_server.cpp`
  (the socket server), `usbmux_client.cpp`, `lockdown_client.cpp`,
  `tls_stream.cpp`, `pair_record.cpp` and `carkit_channel.cpp`. Plain C++,
  POSIX sockets and OpenSSL, and where the logic worth unit testing lives.
- **Also portable since the libusb port** — `usb_device.cpp` and
  `ncm_discovery.cpp`. libusb runs on macOS, so enumeration, descriptor parsing
  and transfers are real there rather than stubbed. `usb_device_stub.cpp` is
  gone, and so is `APPLE_USB_NO_TRANSPORT`.
- **Portable since the framing extraction (2026-08-01)** — `ncm_frame.cpp`: the
  NTB16 parse/build and the EUI-64 link-local derivation, lifted out of
  `ncm_bridge.cpp`. This is the AV data path, and it now has a unit suite
  (`apple_usb_test_ncm_frame`) that runs everywhere.
- **Linux only** — `ncm_bridge.cpp`, because TUN/TAP is. Off Linux it is
  *substituted*, not omitted: `ncm_bridge_null.cpp` supplies the same four
  out-of-line `NcmBridge` methods, `start()` refuses with an explanation, and
  the build prints `apple_usb: non-Linux host -- USB transport built, NCM
  bridge is the null backend`. `APPLE_USB_NO_NCM` is still defined for anyone
  who would rather skip stage 6 than watch it fail, but nothing gates
  compilation on it.

The practical consequence: **the entire `carplay` node builds and links on
macOS**, `usb_pipeline.cpp` and `iap2_session.cpp` included. There is no
`CARPLAY_HAVE_APPLE_USB` any more. Before this, those two files (~1,500 lines)
were dropped from the macOS build purely because they sat in the same CMake
`if(Linux)` block as the bridge — they had always compiled fine — so a refactor
could break the bring-up path and nobody would find out until the next Linux
build.

`apple_usb_usbprobe` also genuinely enumerates a phone on macOS, which makes
step 1 of stage 1b a check you can run off the Linux box.

Anything from stage 2 onward still needs the phone; stage 6 onward also needs
Linux. On macOS, `--max-stage 5` stops cleanly before the bridge.

### Running on macOS — the full session works

**Status: verified 2026-08-01** against iPhone `00008140…` (`05ac:12a8`), the
same phone as the Linux sessions. **Stages 1–7 all run on macOS**, through iAP2
authentication with the real MFi coprocessor to decoded H.264 — one run streamed
975 frames before it was stopped.

```
[usb]    found 05ac:12a8 at port 1-1 (config 6 of 6)
[usb]    already in configuration 6
[muxd]   using the system usbmuxd at /var/run/usbmuxd for udid=00008140
[lockdown] session 3FA8D8C8-…-09170E3755AC up with TLS
[carkit] com.apple.carkit.service is on port 52113 (ssl=true)
[carkit] carkit TLS channel up (iAP2) udid=00008140
[ncm]    NCM function control if3 data if4 (of 2 function(s))
[ncm]    en9 up, accessory link-local fe80::1ca1:5cd0:be56:35c6
[airplay] RTSP receiver listening on fe80::1ca1:5cd0:be56:35c6%en9:7000
[mfi]    using the shared coprocessor, protocol major 2
[iap2]   link NEGOTIATED (SYN/ACK complete)
[iap2]   identification ACCEPTED
[mfi]    answering RequestAuthenticationCertificate      # 908 bytes
[mfi]    answering RequestAuthenticationChallengeResponse # 128 bytes
[iap2]   <- AuthenticationSucceeded (0xaa05)
[video]  screen stream closed after 975 frames
```

**Port 7000 is already taken on macOS.** The system AirPlay Receiver, inside
`ControlCenter`, holds `*:7000`, so the receiver's wildcard bind fails with
`EADDRINUSE`. Binding the NCM link-local specifically succeeds *alongside* it
given `SO_REUSEADDR`, which the receiver already sets — measured:

```
bind [::]                      reuse=1 -> Address already in use
bind fe80::1ca1:5cd0:be56:35c6 reuse=1 -> OK
```

So `ReceiverConfig::bind_address` is set to the link-local on macOS (it was
declared but unused until now, and is resolved with `getaddrinfo` so the
`%en9` scope comes with it). The phone only ever dials the address we
advertised, so nothing is lost by not holding the wildcard. Turning AirPlay
Receiver off in System Settings would also free the port, but is not required.

**Root is needed exactly once, for the configuration switch.** Configuration 6
is sticky across unplugs, so a single privileged run moves the phone into it and
every run afterwards — stages 3 onwards — works unprivileged:

```bash
sudo ./build/nodes/carplay/carplay --max-stage 2 --verbose   # once
./build/nodes/carplay/carplay --max-stage 6 --verbose        # thereafter
```

**macOS needs less code than Linux, not more**, because it ships both of the
things we hand-rolled. `usb_pipeline.cpp` selects between them with
`CARPLAY_USE_SYSTEM_MUX` and `CARPLAY_USE_SYSTEM_NCM`, which default to the host
and can be overridden so either branch type-checks from either platform:

| Stage | Linux | macOS |
|---|---|---|
| 2 — config switch | usbfs + udev rules | whole-device capture, root |
| 3 — mux | our `MuxHost` drives If1 | the system usbmuxd already does |
| 4 — usbmuxd socket | our `UsbmuxdServer` on a private path | `/var/run/usbmuxd` |
| 4 — pair record | we mint one; phone prompts for trust | already exists; no prompt |
| 6 — NCM link | `NcmBridge` + TAP, ~1,400 lines | `AppleUSBNCM` → `en9` |
| 7–10 | portable | portable |

**Why not fight the system daemon.** Taking If1 from macOS's usbmuxd would mean
capturing the whole device, and capture is all-or-nothing — it would also strip
the NCM interfaces from `AppleUSBNCM`, which is precisely what stage 6 wants to
keep. Stopping the daemon is not an option either: `launchctl bootout
system/com.apple.usbmuxd` is refused (`150: Operation not permitted while System
Integrity Protection is engaged`). Using it is both the cheapest and the only
route that leaves stage 6 intact.

**Which interface is the AV link.** `AppleUSBNCM` binds *both* NCM pairs and
creates an interface for each. Pick by MAC, from `iMACAddress` in the CDC
Ethernet functional descriptor — never by "the interface that just appeared".
The iAP interface (If2) brings up an `AppleUSBEthernetHost` interface too:

```
Apple USB Multiplexor@1  +-o usbmuxd  <AppleUSBHostInterfaceUserClient>
AppleUSBEthernet@2       +-o AppleUSBEthernetHostAQM  +-o en7   <- not this
NCM Control@3 / Data@4   +-o AppleUSBNCMData          +-o en9   <- first pair
NCM Control@5 / Data@6   +-o AppleUSBNCMData          +-o en8   <- second pair
```

`en9`'s MAC `ca:1f:e8:0f:24:b1` shares an allocation with the phone's own
address, which is the same tell the Linux session used to pick the first pair.
Note its link-local is a `secured` (RFC 7217) address, *not* the EUI-64 of the
MAC — same as Linux, and the reason both platforms advertise whatever the kernel
actually assigned rather than a derived address.

**Why root, specifically, for stage 2.** The configuration switch has to take
the phone away from whatever already owns it, and the two platforms do that very
differently:

| | Linux | macOS |
|---|---|---|
| Granularity | one interface at a time | the **whole device** at once |
| Permission | udev rules are enough | root, or `com.apple.vm.device-access` |
| Re-enumerates? | no | no (see below) |
| Who is holding it | `ipheth`, `cdc_ncm`, an earlier client | macOS's own `usbmuxd`, always running |

`com.apple.vm.device-access` is a restricted entitlement Apple issues to
virtualization vendors, so root is the only route open to us. `usbprobe` reports
whether the current process has what it needs, and it answers with no phone
attached:

```
$ ./build/libs/apple_usb/apple_usb_usbprobe
Device capture: UNAVAILABLE -- macOS only lets root take a USB device away from
its own drivers ... Re-run the node with sudo.
```

**Three things about macOS capture that are easy to get wrong**, all verified
against libusb's darwin backend rather than assumed:

1. **It does not re-enumerate.** `kUSBReEnumerateCaptureDeviceMask` seizes the
   device, and libusb follows it with `darwin_restore_state()`, which reopens the
   IOKit objects behind the *same* `libusb_device_handle` and restores the
   previous configuration. So the handle survives, and the "must not
   re-enumerate" property the configuration switch is built around holds on both
   platforms. (An earlier version of this note claimed capture invalidated the
   handle. It does not.)
2. **It is refcounted on libusb's cached device, not on the handle**, and
   `darwin_close()` never releases it. One capture at stage 2 therefore covers
   the whole session — the mux, the lockdown channel and the NCM bridge each open
   their own handle and none needs to capture again.
3. **Never call `libusb_attach_kernel_driver()` or enable
   `libusb_set_auto_detach_kernel_driver()`.** Either drops the refcount
   mid-session, and macOS's `usbmuxd` will take the phone back immediately.

Caveats worth knowing before you blame the code:

- Under `sudo`, `$HOME` may be `/var/root`, which moves the state dir. On macOS
  this matters less than it looks — the pair record comes from the system
  usbmuxd, not from our state dir — but pass `--state-dir` explicitly if you
  want one shared location.
- The vendor request `0x52` triggers a real bus re-enumeration, unlike capture.
  That can drop the capture taken before it; the configuration switch simply
  takes it again, which is why the code captures on both sides of the request.
- **String descriptors are padded.** This phone reports its 24-character UDID in
  a 40-character field, space filled, and libusb returns all 40. That is
  invisible in a terminal and harmless while both ends of the usbmux
  conversation are ours — `UsbmuxdServer` echoes back the same padded string, so
  the comparison matches. Against the system usbmuxd it fails as *"the mux does
  not list udid=…"*, which reads like a mux fault and is a string fault.
  `readSerial()` and `readStringDescriptor()` trim; do not undo that.

**Sanity checks that need no phone-side setup.** `usbprobe` reports whether this
process can capture, and `--drivers` shows which interfaces already have
something bound — the two questions that decide whether stage 2 can run:

```bash
./build/libs/apple_usb/apple_usb_usbprobe --drivers
```

`muxctl` points our own usbmux client at any usbmuxd, ours or Apple's, and a
second argument checks `findDevice()` — the lookup stage 4 depends on, and the
one the UDID padding above breaks:

```bash
./build/libs/apple_usb/apple_usb_muxctl /var/run/usbmuxd 00008140000138EE0184801C
```

**Why there is no macOS NCM backend.** macOS has no TAP device. It *could* get
an ethernet segment from a `feth` pair plus BPF frame injection, so this is not
impossible — it is just several hundred lines of privileged platform code
serving a host that will not normally have a phone attached. If it is ever
written it replaces `ncm_bridge_null.cpp` rather than growing out of it.

**Privileges.** The driver needs raw USB access (usbfs) for the phone, an I²C
adapter node for the MFi coprocessor, and TUN for the NCM bridge. Running as root covers
all three. To run unprivileged instead, install the rules shipped in the repo —
this is a one-time step per machine. (On macOS there is no unprivileged option at
all; see "Running stages 2–5 on macOS" above.)

```bash
sudo cp nodes/carplay/udev/99-carplay.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
```

`udevadm trigger` re-applies the rules to already-plugged devices, so you do not
have to unplug anything. Verify — the phone's node should be group `plugdev`
with an ACL (the trailing `+`):

```bash
ls -l /dev/bus/usb/002/007          # crw-rw----+ 1 usbmux plugdev
```

The devnum changes on every re-enumeration (and the config switch in stage 2
forces one), so that path moves; the rule is keyed on the Apple VID, so it
follows. You must be in `plugdev` (`id -nG | grep plugdev`).

This covers stages 2–5, and stage 6 as well provided the persistent TAP is set
up as described there — **verified unprivileged end to end on 2026-08-01**.

Older revisions of this document said stage 6 needed root. It does not, and the
reason it looked that way is worth knowing: the failure it produces is an
`Operation not permitted` on adding the link-local, which invites `sudo` when
the actual cause is the TAP's MAC being pinned too late. See the
`CARPLAY_TAP_MAC` discussion in stage 6. Address configuration is done in-process
(`SIOCSIFADDR`/`SIOCSIFHWADDR`), not by shelling out; the only remaining
shell-out is a best-effort `nmcli` call to stop NetworkManager touching the
link, and it is fine for that to fail.

**The MFi coprocessor is reached over I²C.** On Linux the in-kernel
`hid_mcp2221` driver binds the MCP2221A and registers it as a standard I²C
adapter, which `i2c-dev` exposes as `/dev/i2c-N`; the driver talks to the
coprocessor through that. macOS has no such driver and drives the bridge over
USB HID from userspace instead. The backend follows the host platform and is not
configurable — there is only one right answer per OS.

Load the two modules (they are not autoloaded by anything here):

```bash
sudo cp nodes/carplay/udev/carplay-i2c.conf /etc/modules-load.d/
sudo modprobe hid_mcp2221 && sudo modprobe i2c-dev
i2cdetect -l          # expect "MCP2221 usb-i2c bridge"
i2cdetect -y 0        # expect a device at 0x11
```

`i2c-tools` is worth installing purely as an independent cross-check of the
driver: two separate implementations probing the same bus is the fastest way to
tell a wiring fault from a software one.

**The userspace MCP2221A driver was broken until 2026-08-01, and it failed in a
way that looked exactly like a wiring fault.** Every scan found zero devices and
every read to 0x11 returned `0x41` forever. That is worth knowing about, because
"a full-bus scan finds nothing" is *not* the hardware tell it appears to be.

Three separate bugs, all in `libs/mcp2221a/mcp2221a.cpp`:

1. **Every transfer went to the general-call address 0x00.** The report builders
   were explicit specialisations of a variadic `make_report<Cmd>()`, and a
   specialisation only matches when the deduced argument types match *exactly*.
   Call sites passed promoted `int`s — `make_report<I2CWriteData>(0, addr << 1)`
   deduces `<int, int>`, not `<uint16_t, uint8_t>` — so the specialisation was
   skipped, the primary template packed the arguments consecutively from byte 1,
   and the address landed in the length-MSB field while the address byte stayed
   zero. Nothing ever answered because nothing was ever addressed. Replaced with
   named builders (`makeI2cWrite` etc.) whose parameter types cannot silently
   change the overload.
2. **The ACK test read the wrong thing.** `ack_status` was the whole of response
   byte 20 compared against zero. DS20005565E table 3-2 says byte 20 carries the
   ACK status in **bit 6 only** — "if ACK was received from client value is 0,
   else 1" — with bit 7 and bits 5-0 explicitly "don't care", and they are not
   zero in practice. Measured: `0x00` when 0x11 answers, `0x40` when nothing
   does. Now masked.
3. **A NACK latches the engine and the scan never unwound it.** An unanswered
   address parks the state machine at `AddressNACKed` (0x25), and while it is
   parked *every* transfer command is refused with 0x01. `clear_i2c_engine()`
   now cancels back to idle before each probe.

Plus a timing detail: the write command being accepted only means the engine
took it, not that the address has been clocked out. Reading the ACK bit
immediately gives a stale answer, so the scan settles 2 ms first.

With those fixed, on the same hardware that had been "failing" all along:

```
$ ./build/libs/mcp2221a/mcp2221a_i2c_scan
Found 1 devices:
 - 0x11

$ ./build/libs/apple_mfi_ic/apple_mfi_demo
Device Version: 0x05   Authentication Protocol Version: 2.0
Subject: /C=US/O=Apple Inc./OU=Apple iPod Accessories/CN=IPA_1212AA…
Valid: Yes
Signature: [6c, 94, 27, 27, …]        # 128-byte challenge response
```

**A single failed transfer at startup is normal, not a fault.** The coprocessor
sleeps after even a short idle period and **NACKs the first access, waking on
it** — so the opening read fails and the retry succeeds a few milliseconds
later. `AppleMFIIC::read_register` retries the write/read pair as a unit (8
attempts, 20 ms apart) precisely for this, and only logs an error once all of
them are gone. The MCP2221A layer therefore reports these at DEBUG: it signals
failure through its return value, and only the caller knows whether a failure
is terminal. If you see

```
[error] I2C read from 0x11 failed ... the client did not acknowledge its address
```

at ERROR level, that is a regression in the logging level, not a bus problem —
check whether `MFi coprocessor ready` follows shortly after.

**Triage order, corrected.** Before suspecting the bus, prove the driver: a scan
that finds *nothing at all* is as likely to be an addressing bug as an
electrical one. Genuine bus faults show up as SCL or SDA stuck low, which the
status response reports directly in bytes 22 and 23 — on a healthy idle bus both
read 1. Only once those look right do pull-ups, power, a swapped SDA/SCL pair
and a held reset line become the likely causes.

Note the MCP2221A backend needs **no privilege** on macOS, unlike the phone.

**Conflicting daemons (Linux).** The system `usbmuxd` will fight us for the phone
(this is the core reason we run our own mux there). It is not merely untidy: it
holds interface 1, the exact vendor-specific interface our mux claims
(`kMuxInterface = 1`). Stop it before testing:

```bash
sudo systemctl stop usbmuxd.socket usbmuxd.service
```

On **macOS this is inverted**: the system usbmuxd is not a conflict, it is the
mux we use. Do not try to stop it — SIP refuses, and stopping it would gain
nothing, since taking If1 requires whole-device capture which would also strip
the NCM interfaces we depend on. See "Running on macOS" above.

Some distros ship only the service unit and no socket unit; drop
`usbmuxd.socket` from the command if systemd reports it does not exist. If the
service comes back on its own, socket activation restarted it — `sudo systemctl
mask usbmuxd.socket usbmuxd.service` (reverse with `unmask`).

**`systemctl stop` alone is not enough, and the reason is not obvious.** The
package ships `/usr/lib/udev/rules.d/39-usbmuxd.rules`, which contains:

```
ACTION=="add", ... ATTR{bConfigurationValue}="0", OWNER="usbmux",
                   ENV{SYSTEMD_WANTS}="usbmuxd.service"
```

The stage 2 config switch *deliberately re-enumerates the phone*, which fires
`add`, which restarts usbmuxd mid-test — it then claims interface 1 and the
next `libusb_set_configuration` fails with `EBUSY`. Observed exactly this on
2026-08-01: stopped at 13:18:59, restarted by udev at 13:21:50, six seconds
after the vendor request. The same rule also sets `bConfigurationValue=0`,
unconfiguring the phone on plug.

So **mask it or remove it**; stopping it will not survive a single stage 2 run.
Removing the package is safe and does not cascade — `libimobiledevice-utils`
stays, and device-node access comes from our own `99-carplay.rules`
(`GROUP="plugdev"`, `TAG+="uaccess"`), not from the rule's `OWNER="usbmux"`.
Beware `apt autoremove` afterwards: it will offer to take `libssl-dev` with it,
which is a stage 1 prerequisite.

**`gvfsd-gphoto2` is the other one, and it is easy to misread as usbmuxd.**
Configuration 6 keeps a PTP/imaging interface at interface 0, so GNOME
auto-mounts the phone as a camera and holds a usbfs claim on it. Any claimed
interface makes `libusb_set_configuration` return `EBUSY`, so this blocks
stage 2 exactly the way usbmuxd does — but `systemctl` shows nothing wrong and
`lsusb -t` reports the interface as `usbfs`, not as a named driver.

Find it by owner rather than by guessing:

```bash
ls -l /proc/*/fd 2>/dev/null | grep /dev/bus/usb    # or: sudo fuser -v /dev/bus/usb/BBB/DDD
kill <the gvfsd-gphoto2 pid>
```

`libusb_detach_kernel_driver` cannot help here: a live usbfs claim by another
*process* is not a kernel driver, and no ioctl takes it away.

**Running in a VM.** USB passthrough works, but the stage 2 config switch
deliberately re-enumerates the phone, and hypervisors commonly hand a
re-enumerating device back to the *host* instead of the guest. If the phone
disappears and never returns within the code's 5 s window, suspect passthrough
before suspecting the driver — re-attach it to the guest and confirm with
`lsusb` that the VID is still visible from inside.

## 1b. Re-verifying the libusb transport port (do this first)

**Status: re-verified on hardware 2026-08-01** against iPhone `00008140…`, the
same phone as the 2026-07-21 session. All five steps below pass, plus a full
`--max-stage 7` session to decoded video. The libusb port introduced no
regression: every failure hit during the re-verification was environmental (see
the conflicting-daemons notes in stage 1) except one genuine bug, in the TAP
link-local derivation, which was in unchanged code and is written up in stage 6.

Stages 2, 3 and 6 had been verified on 2026-07-21 against the *old* usbfs/sysfs
transport; those markers are now good again. Nothing above the transport
changed.

**What changed.** Enumeration, descriptor parsing, configuration selection,
driver detach and every transfer now go through libusb (already vendored for
hidapi, `third_party/libusb.cmake`). Three consequences worth knowing before you
start reading logs:

1. **Devices are tracked by physical port, not by serial.** `DeviceInfo` carries
   a `PortPath` (`bus-port.port`, e.g. `1-4.2`) which is stable across the
   re-enumeration the vendor request triggers, whereas the device address is
   not. The port path prints the same way the kernel names the sysfs directory,
   so `/sys/bus/usb/devices/1-4.2` is still the thing to `cat`.
2. **Enumeration no longer reports a UDID.** Reading it costs a device open, so
   it happens once, in `populateSerial()`, *before* the config switch. A phone
   whose UDID cannot be read is now rejected at detection rather than later.
3. **Interfaces and endpoints come from the configuration descriptor.** The mux
   interface is found by its `255/254/2` class triple and the NCM pair by
   walking CDC descriptors, instead of being hardcoded (`muxd.cpp`) or read out
   of sysfs (`ncm_bridge.cpp`). The 2026-07-21 session recorded that the real
   phone matches both — this port is what makes the code rely on that rather
   than on constants that happened to agree.

### Order to verify in

Each step isolates one assumption, so a failure names its own cause. Do not skip
ahead: step 1 is read-only and will tell you whether steps 2–4 are even worth
attempting.

**Step 1 — descriptors, without touching anything.** `apple_usb_usbprobe` claims
nothing and changes nothing; it only enumerates and dumps.

```bash
./build/libs/apple_usb/apple_usb_usbprobe            # read-only
./build/libs/apple_usb/apple_usb_usbprobe --serial   # + opens each device for its UDID
```

Check, in this order:

| Check | Expected | If wrong |
|---|---|---|
| Device listed at all | `05ac:…` with a port path | udev rules (stage 1) |
| `port=` matches sysfs | same string as the `/sys/bus/usb/devices` dir | port-path formatting bug |
| `nconfigs` | 5 before the vendor request, 6 after | stage 2 triage |
| `--serial` prints a UDID | 24/25 chars | `populateSerial` will reject the phone |
| An interface annotated `<- usbmux` | present in config 6 | `muxd` falls back; see below |
| Two `<- CDC-NCM control` interfaces | present in config 6 | NCM discovery will find fewer |
| Bulk endpoints on data **alt 1** | `ep 0x…  bulk` under `alt 1` | `hasBulkPair()` fails |
| `cdc_subtype=0x0f (Ethernet Networking, iMACAddress=N)` | `N` non-zero | host MAC unreadable |

**Reconciled against real configuration 6 on 2026-08-01 — the fixture was
right.** `carPlayLikeConfig()` matches a real iPhone in configuration 6 exactly:
all 11 interface alt settings, the `0xff/0xfd` iAP interface with its three
altsettings, endpoints `0x87`/`0x88`/`0x06`/`0x89`/`0x07`, `iMACAddress` 18 on
the first NCM function and 16 on the second, an interrupt endpoint on the first
NCM control interface and none on the second, and every functional-descriptor
byte including `wMaxSegmentSize` `0x3e8e` and the NCM functional `06 24 1a 00 01
3b`. Nothing needed correcting. The comment claiming it is "byte-for-byte what
the phone reports" is now verified rather than asserted.

Configuration 5 (what the phone boots into) is the same layout **minus** the iAP
interface, so every interface number and endpoint address below If2 shifts down
by one. Do not mistake a config-5 dump for a config-6 one.

**Then reconcile against the unit tests.** `test_ncm_discovery.cpp` encodes the
descriptor shape this port *assumes* (`carPlayLikeConfig()`). If the probe output
disagrees with it — different interface numbers, endpoints on a different
altsetting, a third NCM pair — correct that fixture first and let the tests fail,
then fix the code. Those tests are the only reason any of this was verifiable
without a phone; keeping them honest is what keeps that true.

**Step 2 — the config switch (stage 2).** This is the step most likely to
regress, because `libusb_set_configuration` replaces a hand-written
`USBDEVFS_SETCONFIGURATION` ioctl. On Linux libusb issues that same ioctl, so
the property the old code was careful about — that selecting a configuration
does **not** re-enumerate, unlike writing sysfs — is preserved. Confirm it:

```bash
./build/nodes/carplay/carplay --max-stage 2 --verbose 2>&1 | grep '\[usb\]'
```

Watch for the port path staying *constant* across the vendor request while the
config goes 4 → 6. A changing port path means the phone was re-plugged or a hub
re-enumerated, and the rediscovery keyed on it will time out.

**Step 3 — the mux (stage 3).** The line to look for is new:

```
[muxd] mux on interface 1 (class ff/fe/02), bulk in 0x85 / out 0x04
```

Those must match the hardcoded values the old code used and the 2026-07-21
session confirmed (If1, `0x85`/`0x04`). **If instead you see** `no ff/fe/02
interface in configuration 6; falling back to interface 1`, the descriptor
lookup is wrong for this phone — the fallback keeps the stack working, but note
it and fix the lookup rather than leaving the fallback as the live path.

**Step 4 — NCM (stage 6).** Two new lines replace the old sysfs walk. These are
the real ones, from hardware on 2026-08-01:

```
[ncm] 2 NCM function pairs present; taking the first (control interface 3). Override with CARPLAY_NCM_CTRL_IF.
[ncm] NCM pair in configuration 6: control iface 3 (status ep 0x87), data iface 4 (bulk in 0x88 / out 0x06), iMACAddress string 18
```

Compare every field against step 1's probe output. Then confirm the two
behaviours the old code got right, because both are easy to lose in a rewrite:

- **The first pair is selected, not the second.** The phone exposes two; the
  kernel's `cdc_ncm` binds the first, and the release now happens through
  `libusb_detach_kernel_driver` before the descriptors are read. If the log
  shows a control interface higher than the first NCM one, the detach silently
  failed. `CARPLAY_NCM_CTRL_IF=<n>` pins it while you investigate.
- **The interrupt endpoint is still drained.** See the warning in stage 6 — this
  is unchanged code, but it is the failure mode that looks like everything is
  fine, so re-confirm the write counts rather than assuming.

**Step 5 — NCM throughput under load.** The one change with a genuine
performance question. The two pumps now issue *synchronous libusb* transfers on
one handle; libusb's sync API serialises internally on an event lock, so one
pump can end up servicing the other's completions. The old usbfs ioctls were
independent. Watch for bulk OUT timeouts appearing *only* under video load, and
compare NTB in/out counts against the 2026-07-21 baseline (**3960 out / 10026 in,
zero errors**). `CARPLAY_NCM_NO_READER=1` disables the read pump to test the
write path in isolation.

**Measured 2026-08-01: no starvation.** A ~100 s `--max-stage 7` session with
video streaming throughout gave **3443 NTBs out / 3467 in, zero errors and zero
bulk timeouts**, with 2294 video frames decoded over the same span. So libusb's
sync API serialising both pumps on one event lock is not a problem at CarPlay's
uplink rate, and the async migration below is not needed. (The in-count is lower
than the 07-21 baseline's 10026 only because that was a longer session; the
ratio of errors to transfers is what matters here, and it is zero.)

If this ever does show starvation, the fix is to move the NCM pumps to libusb's
async API (`libusb_submit_transfer` plus a dedicated event thread) — the mux and
lockdown paths are low-rate and can stay synchronous.

### What this port did *not* change

Nothing above the transport: usbmux framing, plists, lockdown, TLS, pairing,
iAP2, AirPlay, NTB16 framing, the TAP device and all IPv6 setup. If a failure
appears in those layers after this port, suspect the transport underneath rather
than the layer reporting it — with one exception: a phone rejected at detection
for an unreadable UDID never reaches them at all.

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

**Verified on hardware (2026-07-21)**, iPhone `00008140…` — the constants in
`usb_device.h`/`muxd.cpp` are all correct for this generation:

| Expectation | Result |
|---|---|
| `0xC0/0x52` reveals extra configurations | ✓ 5 → **6** configurations |
| `kCarPlayConfiguration = 6` | ✓ config 6 = `PTP + Apple Mobile Device + Apple USB Ethernet + NCM` |
| mux at If1, `kEpOut 0x04` / `kEpIn 0x85` | ✓ If1, vendor-specific 255/254/2 |
| `kNcmDataAltSetting = 1` | ✓ bulk endpoints live on alt 1 |

Two of those rows are no longer constants: since the libusb port the mux
interface is *found* by its `255/254/2` triple and its endpoints read from the
descriptor, rather than assumed to be If1/`0x04`/`0x85`. The hardware row above
is what says that lookup will land on the same place. See stage 1b.

Note the vendor request is *sticky but not idempotent-looking*: before it the
phone advertises 5 configurations (config 5 is `…+ NCM`, which looks tempting
but is **not** the CarPlay config), after it 6. Do not "fix" the constant to 5.

**Applying the configuration needs the kernel drivers out of the way.** The
switch is done with `libusb_set_configuration`, which on Linux issues
`USBDEVFS_SETCONFIGURATION` on the usbfs node — a udev rule can grant that to a
normal user, unlike the root-only sysfs attribute. The kernel returns **`EBUSY`
while any interface is claimed**, so every bound driver is released first with
`libusb_detach_kernel_driver` (also `USBDEVFS_DISCONNECT` underneath); `ipheth`
and an earlier usbfs client both hold interfaces in config 4. Unlike the vendor
request this does **not** re-enumerate the device — config 6 is active in ~100 ms
and, on a VM, the passthrough binding survives. There is **no fallback**: the
root-only sysfs `bConfigurationValue` write inherited from the usbfs
implementation was removed on 2026-08-01, once libusb had been verified against
hardware. It covered a nearly empty case (root can open the usbfs node anyway)
and reached the configuration by re-enumerating — the one thing this step is
careful to avoid.

**Triage.**
- Stuck at 4 configurations → the `0xC0/0x52` vendor request failed; check for
  `EPERM` (run as root) or that the phone is unlocked and trusted.
- `Failed to set configuration …` → libusb could not open the device, or the
  device rejected the request. Install the udev rules from stage 1, or run as
  root.
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

This is the stage that runs the lockdown handshake over *our* mux socket.

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

**Verified on hardware (2026-07-21):** `idevice_id -l` returns the UDID through
our socket and `[carkit] carkit TLS channel up (iAP2)` appears ~113 ms after
start. This exercised the whole chain — mux, the plist framing, the `Connect`
relay, lockdown pairing and TLS — so stages 3 and 4 are no longer speculative.

**The phone must be UNLOCKED, not merely trusted.** These are different things
and only one of them prompts you. With the screen locked, lockdown returns
`Password protected (-17)` and every stage-4 attempt fails while stages 2–3 look
perfect. Tapping "Trust" does not clear it — enter the passcode and keep the
phone awake.

**UDID form matters.** libusbmuxd normalises a modern 24-character serial into
the 25-character `XXXXXXXX-XXXXXXXXXXXXXXXX` form, and `idevice_new_with_options`
matches against *that*. The serial we read from sysfs has no dash, so it is
converted in `openCarkitChannel` before the lookup. Verified differentially:

```
ideviceinfo -u 00008140000138EE0184801C   -> ERROR: Device ... not found!
ideviceinfo -u 00008140-000138EE0184801C  -> reaches lockdownd
```

Without that conversion stage 4 fails at `idevice_new` with a "device not found"
that looks like a mux bug but is a string-format bug.

**Triage.**
- `idevice_id -l` empty → our `UsbmuxdServer` ListDevices reply is wrong; check the
  plist packet header framing (little-endian length/version/message/tag).
- `Password protected (-17)` → the phone's screen is locked. Unlock it.
- Handshake fails with a pairing error → tap "Trust" on the phone; confirm pair
  records are being written under `--state-dir`. Delete the state dir to force a
  fresh pair.
- `could not start com.apple.carkit.service` → the phone did not expose the service.
  Confirm it is genuinely in config 6 (stage 2) — carkit only exists there.
- TLS enable fails → check `[tls]` at `--verbose`; the handshake logs the
  negotiated version and cipher, and the client certificate comes from the pair
  record's root key pair.

### How stage 4 replaced libimobiledevice

Stage 4 is `UsbmuxClient` for the transport, `LockdownClient` for the handshake,
`TlsStream` for both TLS sessions (the lockdown session and the carkit service
connection), and `PairRecord` for the identity — including minting one, so a
device that has never been trusted pairs on our code.

This section is history rather than instructions: libimobiledevice is gone. It is
kept because the two bugs below were found by diffing against its source, and
because both are the kind that will be reintroduced by anyone who assumes the
obvious implementation is correct.

Verified on hardware 2026-07-31:

- **Pairing from nothing.** With the state dir emptied, stage 4 reads the
  device public key, mints a root/host/device certificate set, sends `Pair`, and
  stores the record the device's answer completes. The result is byte-compatible
  with libimobiledevice's: same fields, same sizes (root 948, host 964, device
  1005, keys 1704, escrow bag 32), same extensions, same `sha256WithRSAEncryption`.
- **Interop both directions**, while libimobiledevice was still present to
  check against: it ran a full session to `AuthenticationSucceeded` on a record
  we generated, without re-pairing, and we did the same on a record it wrote.
  Records written by either remain readable.
- **Stability.** 25 consecutive `--max-stage 5` runs with no channel loss, plus
  a 150-second single session. Before the fix below it was 6 failures in 13.

The certificates carry **empty subject and issuer names**, which is Apple's
design and what libimobiledevice does too. `openssl verify` therefore reports
"self-signed certificate" for the host and device certificates -- it cannot build
a chain by name. That is expected, identical for libimobiledevice's own records,
and not a defect; `X509_verify` against the root's key is the real check, and
`apple_usb_test_pair_record` does exactly that.

Both of the robustness bugs below were found by diffing against
libimobiledevice's source while it was still vendored. If stage 4 regresses and
the cause is not obvious, that source is still the best reference — the relevant
functions are `idevice_connection_receive_timeout`, `lockdownd_start_session`
and `pair_record_generate_keys_and_certs`.

#### The read-ordering bug, and why the reference's semantics matter

Two fixes took stage 4 from intermittently broken to solid. Both came out of
reading libimobiledevice's source rather than from the symptom.

1. **`recv` must gather, not return the first chunk.** This was the whole
   intermittency: 6 failures in 13 runs before, 0 in 25 after. The phone would
   reset the carkit connection about 0.9 s after the channel came up, on the
   first session of a process.

   `idevice_connection_receive_timeout` loops `SSL_read` until the caller's
   buffer is *full*, returning a partial buffer only once a read times out, and
   the iAP2 link layer was written against those semantics. Returning the first
   available chunk instead hands back as little as a dozen bytes per call with a
   full link-layer poll cycle between calls, so a burst -- the post-authentication
   flurry, or a 60 KB album artwork frame -- leaves the socket backed up. Our own
   `UsbmuxdServer` relay then blocks writing into that socket, which stalls the
   thread pumping the USB mux, which stalls every other stream on it.

   Recorded because it was tested and **rejected**: this is not about ACK volume.
   Runs that died sent 8 ACKs between channel-up and the reset; healthy runs send
   11 over the same span. Fewer, not more.

2. **`SSL_read` before `poll`, never after.** OpenSSL buffers whole records, so
   once a large message is split across reads the remaining plaintext is already
   decrypted and held while the socket has nothing to report. Polling first waits
   out the entire timeout before returning data it was already holding. The fix
   is `SSL_read` first and `poll` only on `WANT_READ`, which needs a non-blocking
   socket. This alone took the failure rate from 3/4 to 1/4.

Two more differences from the reference, both found the same way and both real:

- **A service dies with the session that started it.** Dropping the
  `LockdownClient` once `StartService` returned killed the carkit channel about a
  second later, every time. `NativeCarkitChannel` owns it for exactly that
  reason, and `LockdownClient`'s destructor sends `StopSession` the way
  `lockdownd_client_free` does.
- **No escrow bag on `StartService`.** libimobiledevice's
  `lockdownd_start_service` passes `send_escrow_bag=0`; ours originally sent one.
  It did not turn out to affect stability, but matching the reference is correct.

**Triage.**
- `the phone is locked` → lockdown returned `PasswordProtected`. Unlock the phone
  and keep it awake. This is reported before the trust prompt can appear, and it
  is *not* cleared by tapping Trust.
- `the phone rejected our pair record` → the record is stale (phone reset, trust
  revoked). `native` re-pairs automatically; no need to delete the state dir.
- Anything else at stage 4 → run with `--verbose` and read the `[carkit]`,
  `[lockdown]` and `[tls]` lines in order; each stage of the handshake logs where
  it got to. `apple_usb_muxctl` isolates the transport underneath it.

## 5. iAP2 link layer, identification, MFi auth

```bash
sudo ./build/nodes/carplay/carplay --verbose 2>&1 | grep -E '\[iap2\]|\[mfi\]'
```

**Expect:** link SYN/ACK established, identification accepted, MFi certificate read
and a challenge signed, then the phone reporting CarPlay availability.

**Verified on hardware (2026-07-21)** up to the MFi handshake — the link layer
and the wired identification encoding are correct:

```
carkit > SYN     seq=99  ack=0   len=29
carkit < SYN|ACK seq=101 ack=99  len=29
carkit > ACK     seq=99  ack=101
[iap2] link negotiated (state -> normal)
[iap2] IdentificationInformation encoded: 344 bytes, 19 params
[iap2] <- StartIdentification (0x1d00)
[iap2] <- IdentificationAccepted (0x1d02)     <- accepted first try, no rejection
[iap2] <- RequestAuthenticationCertificate (0xaa00)
```

The phone advertises `max_outgoing=4 max_len=65535 rto=0ms ack_timeout=0ms
max_retransmissions=0 max_ack=0` and three sessions (10 control v2, 11 external
accessory v1, 12 file transfer v2). Two of the caveats below are settled by those
numbers: `max_len` really is 65535 so the fragmentation off-by-ten is invisible,
and the phone advertises **zero** retransmissions/acks, confirming the wired path
never exercises the retransmission/EAK timers.

**The phone requests the MFi certificate immediately after accepting
identification**, before sending anything else. So `CarPlayAvailability` — and
with it the zero-length-boolean question below — **cannot be reached until the
coprocessor works**. `--iap2-allow-missing-mfi` runs everything up to that point
anyway, which is the right way to exercise the link while the board is out.

**Isolating the MFi board (do this before blaming iAP2).** The bridge and the
coprocessor are separate failure domains, and the MCP2221A tells you which one
is at fault if you read its status. `apple_mfi_demo` narrates both:

| Symptom | Layer | Meaning |
|---|---|---|
| `MCP2221A device not found` | host | no `/dev/hidraw` node — `hid_mcp2221` is still bound (see stage 1) |
| `I2C engine is in state 0x62, not idle; resetting` | bridge | expected once after an unclean exit; the driver self-heals |
| `I2C speed set to 100000 Hz` | bridge | **bridge is fully healthy from here on** |
| `state: 0x25` (`AddressNACKed`) | board | nothing is answering at that address |

`mcp2221a_i2c_scan` finding **no** devices while the bridge reports
`SCL=1 SDA=1` means the I²C lines are pulled up and free but the coprocessor is
not acknowledging — i.e. a board problem (power, wiring, or the MFi RESET pin
held asserted), not a software one. Note the library has no GPIO support, so if
your breakout wires MFi RESET to one of the bridge's GP0–GP3 pins, nothing
releases it and every address will NACK.

**The coprocessor sleeps, and the first access after it wakes is NACKed.**
This is the single most misleading behaviour on this board. A bus scan that
probes each address once walks straight past it: the wake-up NACK at `0x11` is
read as "nothing here" and the scan moves on to `0x12`, never coming back. Two
consecutive `i2cdetect` runs show it clearly — the first finds nothing, the
second finds `0x11`. It also re-sleeps quickly: a **0.7 s** gap between opening
the bus and the next access was enough. Every transaction in `AppleMFIIC` is
therefore retried (8 attempts, 20 ms apart) rather than only the first one after
connect. Do not "simplify" that away.

**Two MCP2221A behaviours worth knowing.** These bit us on the userspace hidapi
path used on macOS; the kernel driver handles both itself, so they are invisible
on Linux:

- *A cancel issued while the I²C engine is Idle wedges it.* The engine drives a
  STOP that never completes and latches `StopTimeout` (0x62), which refuses
  every later parameter change and survives process exit. `MCP2221A::cancel()`
  therefore returns early when already idle — do not "helpfully" remove that
  guard.
- *Only a device Reset clears a latched 0x62.* Cancel does not; five consecutive
  cancels were acknowledged and left the state unchanged. Reset costs a full USB
  re-enumeration (measured ~6 s through VMware USB passthrough, and the hidraw
  node path is recycled, so a handle opened too early lands on the dying node),
  so `open()` resets **only** when it finds the engine non-idle.

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
`nullopt`), but the spec arguably intends presence-as-value here.

**Not observed on the phone tested during bring-up** — it sends a proper
one-byte boolean (`wired=true available=1`), so this did not fire. It is not
worth a runtime switch, but it is worth recognising: `runIap2Session` logs a
loud warning naming the zero-length case specifically, because the resulting
failure is otherwise completely silent — availability simply decodes as absent
and no session is ever requested. If that warning appears, changing
`csm::getBool()` to treat a zero-length boolean as `true` is the one-line fix.

**Other iAP2 caveats to keep in mind:**
- Outbound fragmentation chunks at `max_len - 10` (header+checksum overhead),
  where LIVI chunks at `max_len`. Invisible on the wired path (65535, small
  messages) but it matters if a phone advertises a small `max_len`.
- Only the *wired* carkit identification is implemented. Bluetooth/wireless
  transport components are deliberately not encoded.
- Retransmission/EAK timers are a structural port that has never run against a
  phone in either codebase — only the zero-ack wired path is exercised in
  practice. Suspect them if the link is unstable under load rather than at setup.
- ~~The link layer treats an empty `recv()` as "no data yet", never as EOF.~~
  **Fixed.** This was real: `LibimobiledeviceCarkitChannel::recv()` returned an
  empty vector for both a timeout and a hard error, and a failed `send()` was
  discarded, so a dead link would have spun forever. `CarkitChannel` now exposes
  `alive()`, which the stage 5 transport adapter checks every poll.

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

**⚠ The single most important thing on this stage: drain the control
interface's interrupt endpoint.** CDC devices announce link state there
(`NETWORK_CONNECTION`, `CONNECTION_SPEED_CHANGE`) and the kernel's `cdc_ncm`
always keeps a URB queued on it. If the host never reads it, **the phone refuses
to service the bulk OUT endpoint entirely** — every write times out while reads
on the same interface keep working perfectly. It is a maddening signature
because nothing looks wrong: the pair is claimed, the altsetting is right, the
endpoints match the descriptors, and downlink is flowing. Draining the endpoint
took this from 11 failed writes out of 12 to **3960 writes with zero errors**.

**Verified on hardware (2026-07-21):** `cpusb0` up with the phone-dictated MAC,
3960 NTBs out / 10026 in with no errors, and the phone opening TCP to
`[fe80::…]:7000` and sending `POST /pair-setup RTSP/1.0`
(`User-Agent: AirPlay/950.7.1`). Stage 6 is done; that request is stage 7's
first message.

**Running unprivileged.** Creating a TAP needs `CAP_NET_ADMIN`, but *attaching*
to a persistent one you already own does not — and `setcap` on the binary is
lost on every rebuild, since it lives on the inode. Create the device once
instead:

```bash
sudo cp nodes/carplay/udev/carplay-tap.service /etc/systemd/system/
sudo systemctl enable --now carplay-tap.service
```

That also sets `addrgenmode eui64`, so the kernel derives exactly the `fe80::`
the bridge advertises, and `accept_dad=0`. The MAC is still set at runtime (the
phone dictates it through `iMACAddress` and will ignore us otherwise) via
`SIOCSIFHWADDR` **on the tun fd**, which the tun driver allows for a device you
own. No capability is needed anywhere.

**The advertised `fe80::` does not have to encode our MAC, and relying on that
is what keeps stage 6 unprivileged.** This cost an hour on 2026-08-01 by looking
like a permissions problem, so the mechanism is worth stating exactly.

The kernel derives a link-local **when the interface is brought up**, from
whatever MAC is set at that instant, and it will not revise that address later:

- IPv6 addresses survive carrier loss — only `NETDEV_DOWN` (admin down) flushes
  them.
- addrconf does **not** regenerate on `NETDEV_CHANGEADDR`.
- It will not add a second link-local when one already exists.

On a persistent TAP nothing has carrier until the bridge attaches, so generation
is triggered by our own `TUNSETIFF` — and the bridge sets the phone-dictated MAC
microseconds afterwards. **That is a race**, and it goes both ways in practice:
both outcomes were observed on 2026-08-01 on the same machine. Whichever address
lands then sticks for the life of the device. So this is intermittent across
boots, not a deterministic failure — do not conclude from one good run that it
is fixed.

The bridge therefore **advertises whichever link-local the interface actually
has** rather than insisting on the EUI-64 of the MAC. That address is reachable
by definition, so nothing has to be added and the one step that wanted
`CAP_NET_ADMIN` disappears.

**Verified on hardware that the mismatch is harmless** (2026-08-01). With
`cpusb0` deliberately left holding `fe80::dc70:7eff:fe94:a6ee` while its MAC was
`ca:1f:e8:0f:24:b1` — forced by attaching, waiting out addrconf, then setting
the MAC — the phone dialled the advertised address and the session ran to 2061
decoded frames. The EUI-64 match in LIVI is incidental: it creates the TAP with
the phone's MAC, so the kernel derives from it anyway. NDP resolves the address
to whatever MAC we present, which is the part that actually matters.

```
[ncm]  advertising kernel link-local fe80::dc70:7eff:fe94:a6ee (EUI-64 of this MAC would be fe80::c81f:e8ff:fe0f:24b1)
[iap2] sending CarPlayStartSession -> [fe80::dc70:7eff:fe94:a6ee]:7000 id=ca:1f:e8:0f:24:b1
```

`CARPLAY_TAP_MAC` in `carplay-tap.service` is consequently **optional**. Setting
it to the phone's host MAC before first bring-up makes the derived address
predictable and removes the race's visible effect, but nothing depends on it,
and leaving it empty is correct for a head unit that may see more than one phone.

**Recorded because it was tried and does not work:** `TUNSETCARRIER` on our own
tun fd, to bounce carrier and make addrconf redo the derivation. It is the
obvious unprivileged lever and it fails for the first reason above — the stale
address is still present when carrier returns, so generation is skipped.
Verified directly: with the bridge attached, `ip -6 addr` still showed the old
address after the bounce. Do not re-attempt it.

**Triage.**
- No `cpusb0` → `/dev/net/tun` missing or no `CAP_NET_ADMIN`.
- `no NCM function pair in configuration 6` → descriptor discovery found nothing;
  run `apple_usb_usbprobe` and compare against stage 1b's table.
- `refusing to claim: a kernel driver still holds interface N` → the
  `libusb_detach_kernel_driver` pass did not take. Confirm with
  `lsusb -t` that `cdc_ncm` is gone.
- Interface up but no ping → the kernel `cdc_ncm` driver may have claimed the
  interface; it must be unbound so we can drive it from userspace.
- Ping works but no inbound TCP → check that `CarPlayStartSession` was sent with
  the correct accessory fe80 address and port 7000.
- **Every bulk OUT times out while reads work** → the interrupt endpoint is not
  being drained; see above. Confirm with usbmon (below): the OUT URBs will show
  as submitted and then `ENOENT`/unlinked, meaning the device never serviced
  them at all, while OUT on the usbmux endpoint `0x04` completes normally.
- `"bulk endpoints not found"` → endpoint discovery now reads the configuration
  descriptor through libusb, so it no longer depends on having selected the
  altsetting first and the old sysfs `ep_*` race is gone entirely. Altsetting 1
  is still where the data interface's bulk pair lives (as in LIVI), confirmed
  on hardware 2026-08-01 for both NCM pairs.
- Nothing at all on either endpoint → the phone only powers up its NCM data
  path once a CarPlay session is actually running. Before `CarPlayStartSession`
  both directions time out, which is expected, not a fault.

**Debugging USB with usbmon.** When a transfer fails and the cause is not
visible from the driver's own logs, look at the bus:

```bash
sudo modprobe usbmon
sudo setcap cap_net_raw,cap_net_admin+eip $(which tcpdump)
sudo chgrp plugdev /dev/usbmon* && sudo chmod g+r /dev/usbmon*   # not persistent
tcpdump -i usbmon2 -w /tmp/usb.pcap -s 256      # bus 2; match your phone's bus
```

Decode with the summary script pattern: the URB status is what matters.
`ENOENT`/`ECONNRESET` on completion means *we* cancelled it (our timeout fired
and the device never responded); `EPIPE` means the device stalled; `OK` on a
sibling endpoint proves the device is servicing the bus generally, which is what
localised the fault above.

**Two NCM pairs.** The CarPlay configuration exposes two, and `cdc_ncm` claims
the first as soon as the configuration is applied. The bridge releases it
(`detachKernelNcmDrivers`) and uses that first pair, which is correct: its host
MAC shares an allocation with the phone's own address (`ca:1f:e8:0f:…` here)
while the second pair's is unrelated. `CARPLAY_NCM_CTRL_IF` pins the pair if you
need to re-test that.
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

That differential run was a one-off. Since 2026-08-01 the framing lives in
`ncm_frame.cpp` and `apple_usb_test_ncm_frame` pins it permanently and in-tree
(60 assertions, runs on any host). It covers what the differential run did not:
malformed input. Backwards NDP chains, out-of-range datagram pointers,
truncated blocks and short NDPs are all exercised, because the differential
cases were all things Python had produced and therefore well-formed.

**One latent bug found by that suite** (2026-08-01, fixed): `deriveEui64LinkLocal`
accepted a malformed MAC. `std::from_chars` reports success on a *partial*
parse, so `"0g:…"` came back as `0` with the pointer left on the `g`, and the
end pointer was never checked. In practice the MAC comes from
`/sys/class/net/<if>/address` and is always well-formed, so this could not fire
on the current call path — but the failure mode if it ever did is the bad kind:
a plausible-looking wrong link-local goes into `CarPlayStartSession`, the phone
dials an address we are not listening on, and the session dies with no error
anywhere. The fix requires `from_chars` to have consumed both hex digits.

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

**Verified on hardware (2026-07-21):** the complete handshake runs and the phone
streams H.264. In order: `/pair-setup` M1→M6, `/pair-verify` M1→M4, the encrypted
control channel, `/auth-setup`, session `SETUP`, `GET /info`, `RECORD`,
`POST /command`, stream `SETUP` (type 110), then a video data connection
carrying the avcC config and encrypted frames:

```
[airplay] stream type 110 -> dataPort 35141 (connectionID 7411721103110128217)
[video]   screen stream connected
[video]   codec config: H.264 (32 bytes Annex-B)
[video]   FIRST FRAME decoded: 116 bytes Annex-B
```

**`viewAreas` is what unblocked the stream.** Before it, the phone accepted
everything through `RECORD` and then sent `TEARDOWN` ~1 ms later without ever
requesting a stream. The display entry must carry `viewAreas` (with a nested
`safeArea`) and `initialViewArea` — we were advertising `viewAreas` in the
session SETUP `enabledFeatures` while supplying none, which is worse than not
claiming it at all.

**The screen stream format:** a 128-byte header followed by a body whose length
is the header's leading little-endian `uint32`. `header[4]` is the opcode: 1 is
the codec config (an avcC atom, in the clear), 0 is a frame, ChaCha20-Poly1305
sealed with the entire 128-byte header as AAD and a counter nonce that advances
**only on frames**.

**The first message on the stream is an empty config, and that is normal.**
The phone opens with a well-formed opcode-1 header carrying a zero-length body:

```
header 00 00 00 00 op=01 | 00 56 01 c5      <- length 0, opcode 1
```

The real 33-byte avcC follows ~60 ms later. This is not a framing desync and not
a preamble being misread — the length field really is zero, and the stream stays
aligned either way, which is why it is easy to misdiagnose. `configToAnnexB()`
rejects anything below 9 bytes up front (nothing shorter can be avcC or hvcC)
and logs it at debug. Before that guard existed it fell through to a speculative
bare-`hvcC` parse, which logged `nalu: hvcC atom too short (0 bytes)` at **error**
in a session that only ever advertises H.264 — an H.265 complaint about a codec
nobody sent, once per connection. If you see that line again, the guard has been
removed. The key is
`HKDF-SHA512(pair-verify shared, "DataStream-Salt<streamConnectionID>",
"DataStream-Output-Encryption-Key", 32)`.

**The event channel** is encrypted from the first byte with keys derived from
the pair-verify shared secret: `HKDF-SHA512(shared, "Events-Salt",
"Events-Write-Encryption-Key"|"Events-Read-Encryption-Key")`. Unlike the control
channel these are **not** swapped — the accessory writes with Events-Write and
reads with Events-Read. HID input (touch) is pushed over it as a
`POST /command` with an `hidSendReport` plist: `{type, uuid, hidReport}`, where
`hidReport` is the multitouch report matching the descriptor in `/info` (six
bytes per contact: `[index, down, x-lo, x-hi, y-lo, y-hi]`, pixel coordinates).

**⚠ `streamConnectionID` is unsigned.** It goes into that salt as a decimal
string, and roughly half of all sessions produce a value above `INT64_MAX`,
which a signed plist decode renders negative — a different salt, a different
key, and every frame failing to decrypt. Verified on hardware:
`4663436911794014275` worked, `-3498692594036096197` (really
`14948051479673455419`) did not. Format it as `uint64_t`.

Details worth not rediscovering:

- **pair-setup is transient SRP with password `3939`.** Username is
  `Pair-Setup`, as the triage note below says. M5/M6 exchange long-term Ed25519
  identities under `Pair-Setup-Encrypt-Salt`/`-Info` with nonces `PS-Msg05`/`06`.
- **`A` is occasionally 383 bytes, not 384.** Roughly one run in 256 the phone
  strips a leading zero from its SRP public key. `srp::Server::verify()` re-pads
  from the BIGNUM so this is handled, but a 456-byte M3 body instead of 457 is
  the tell if a proof is ever rejected for no apparent reason.
- **After pair-verify M4 the control channel is encrypted** and stays that way:
  2-byte little-endian length, ciphertext, 16-byte Poly1305 tag, the length
  doubling as AAD, separate counter nonces per direction starting at zero. The
  accessory *sends* with `Control-Read-Encryption-Key` and *receives* with
  `Control-Write-Encryption-Key` — the naming is from the controller's point of
  view. Get this wrong and the phone simply goes quiet, because our parser sits
  waiting for an RTSP header that never comes.
- **`/auth-setup` layout**, which is not guessable and cost the most time:
  request is `<1 mode><32 device X25519 pk>`; response is
  `<32 our pk><4 cert length BE><cert><4 signature length BE><signature>`. The
  signature is over `SHA-1(our_pk | their_pk)` signed by the coprocessor, then
  **encrypted with AES-128-CTR** where the key is `SHA-1("AES-KEY" | shared)[0:16]`
  and the IV is `SHA-1("AES-IV" | shared)[0:16]`. Note SHA-**1**, not SHA-512 —
  that single mistake looks identical to every other failure mode from outside.
- **The clock sync is mandatory.** The session SETUP body carries the phone's
  `timingPort`; we must bind our own UDP port, advertise it, and drive RTCP-style
  type-210 requests at it (see `libs/airplay/timing.cpp`). LIVI's comment is
  explicit that the phone tears the session down without it.

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
| `cannot convert decoded frame to RGB` | decoded, but swscale could not build a converter for that pixel format / geometry |
| `first video frame decoded and rendered (WxH)` | **the picture is live**; if the screen is still black, suspect widget geometry/layout, not video |

**Colours look wrong?** Channel order is swscale's problem now, not ours, so a
red/blue swap is no longer a failure mode. What *is* worth checking is colour
**range**: `renderFrameToBackBuffer()` normalises the deprecated `YUVJ*` formats
to their plain equivalents and drives the range via `sws_setColorspaceDetails`,
full range for `YUVJ420P` or `color_range == AVCOL_RANGE_JPEG` and limited
otherwise. Real CarPlay is full-range; the `--simulate` x264 stream is
limited-range. Getting this backwards shows up as washed-out or over-contrasty
video, not wrong hues. `CARPLAY_DUMP_RENDER=/path.png` on the dashboard grabs
the exact `QImage` the widget blits, to check pixel values without a screenshot
tool.

Historical note: the original hand-rolled converter wrote into a
`Format_RGB888` buffer and swapped red and blue for a while. Greens were
unaffected (the middle byte is always G) and the test pattern was white-on-grey,
so it survived until a real CarPlay frame — a blue Maps dot rendering red was
the giveaway. swscale replaced that loop.

### How video reaches the screen

The decode thread converts each `AVFrame` with libswscale straight into one of
two reused `QImage`s (`Format_RGB32`, Qt's native raster format), then takes
`_frame_mutex` only long enough to flip a front/back index — no pixels are
copied to publish a frame. `paintEvent` holds that same lock across its
`drawImage`, which is what stops the decoder from overwriting a buffer mid-draw.

**swscale converts *and* scales in one pass, to the widget's size, not the
stream's.** The widget publishes its geometry into an atomic on resize and the
decode thread scales to it. This is deliberate: swscale has to walk every pixel
for the colour conversion regardless, so folding the resize in is close to free,
whereas leaving it to `drawImage(rect(), img)` costs a *second* full transform
pass — on the GUI thread, on **every repaint**, not once per decoded frame. With
overlays composited above the video that repaint independently of the frame
rate, that difference compounds. The steady-state paint is now always a straight
blit. Verified with `--sim-width 640 --sim-height 480` against the 800x600
widget: the log reads `video scaler ready: 640x480 yuv420p -> 800x600 RGB32` and
the dumped frame is 800x600.

`SWS_POINT` is used when the sizes match (swscale's optimised unscaled
converter) and `SWS_BILINEAR` when a real resize is needed. The scaler context
is rebuilt only when source geometry, target geometry, pixel format or colour
range actually changes.

Because rendering goes through `QPainter` into the normal widget backing store,
**ordinary Qt Z-ordering applies** — sibling widgets can be `raise()`d over the
video and will be visible. (A `QVideoWidget` was tried here and reverted for
exactly this reason: its surface composited on top of any overlapping sibling,
even a raised one, so nothing could be layered above the video.)

**Three bugs stood between "frames arriving" and "picture on screen"**, all
found running the real dashboard against a live phone (2026-07-22):

1. **The phone sends exactly one keyframe.** A static CarPlay screen produces one
   IDR at session start and then only P-frames (verified: 1 × NAL type 5, 100 ×
   type 1 in a capture). A dashboard that subscribes late never sees it. Fix: the
   driver asks the phone for a fresh keyframe periodically via a `forceKeyFrame`
   command on the encrypted event channel (`Receiver::requestKeyframe`, every
   1 s). The phone then re-sends parameter sets + an IDR, and any late subscriber
   syncs within a second.
2. **CarPlay decodes to `YUVJ420P` (pix_fmt 12), not `YUV420P` (0).** The widget's
   converter rejected anything but format 0 and dropped every frame with
   `cannot convert decoded frame to RGB`. The two formats share layout and the
   converter's coefficients were already full-range, so the fix was simply to
   accept format 12 as well.
3. **The driver published `VideoConfig` once and cached it silently.** It must be
   published as its own message *and* re-published before every keyframe, since
   zenoh has no retained messages.

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

### How touch reaches the phone, and why it is rate limited

Each touch report costs far more than it looks. `Receiver::sendTouch()` builds a
plist, `plist::encode()`s it, wraps it in an RTSP POST, runs it through
`encryptFrames()`, and writes it to the event channel socket. That channel is
**shared with `requestKeyframe()`** — the thing that recovers a black screen for
a late-joining renderer. Unthrottled touch on a 500–1000 Hz mouse can therefore
delay the keyframe request, which is a much worse failure than a slightly
coarser drag.

Two independent limits, at different altitudes:

1. **The widget paces itself to 60 Hz** (`kTouchPublishHz`). Leading edge, so a
   drag starts responding immediately, with coalescing and a trailing flush for
   everything inside the interval.
2. **The node enforces a 125 Hz ceiling** in `eventSendLoop()`. This is a
   guardrail, not a second throttle — it sits well above the widget's rate so it
   never engages in normal operation, and exists only to bound a publisher that
   ignores its own limit.

**60, not 30.** The phone derives scroll momentum from the last few samples of a
gesture; at 30 Hz a quick flick only lands two or three, so fling velocity comes
out noisy. The symptom is taps and slow drags feeling fine while flicks feel
inconsistent — easy to misread as a phone-side problem. We also advertise
high-fidelity touch in `/info`, so 30 would undersell what we claim.

The two limits share only the spacing decision, as `helpers::RateGate`
(`libs/helpers/include/helpers/rate_gate.h`) — a header-only "may I send at
`now`, and if not how long until I may". They are otherwise different animals
and are deliberately not unified: `airplay::EventQueue` is a bounded
multi-producer queue drained by a writer thread that limits *every* report
including down and up, while `TouchThrottle`
(`dashboard/widgets/carplay/include/carplay/touch_throttle.h`) is single
threaded, holds at most one deferred position, and never delays a down or an up.
Folding the widget onto `EventQueue` would also mean the dashboard linking the
AirPlay stack to get a rate limiter.

Three invariants that a naive throttle breaks, all covered by
`carplay_test_touch_throttle` (exact, time injected) and again by
`carplay_test_touch_rate` (through a real widget, real wall clock):

- **Down and up are never rate limited.** They are state transitions, not
  samples.
- **A drag that stops moving still reports where it came to rest.** Without the
  trailing flush the last move is swallowed and no further events arrive, so the
  phone's idea of the finger stays an interval behind indefinitely. This is the
  one that bites.
- **Motion never coalesces across a down or an up.** Collapsing a down into a
  following move relocates the press and turns a drag into a tap somewhere else.
  This is why `sendTouch()` takes a `TouchPhase` rather than the old bare `down`
  bool — the receiver could not otherwise tell a down from a move, since both
  set the same bit on the wire.

**Nothing blocks on the socket.** A single writer thread (`eventSendLoop()`)
owns the event channel; `sendTouch()` and `requestKeyframe()` enqueue and
return, so the zenoh subscriber thread and the keyframe thread can no longer
stall on a congested `send()` or on each other. Keyframe requests are held as an
idempotent flag rather than queued, and jump ahead of pending touch — they carry
no ordering relationship to a gesture, so there is nothing to gain by making
them wait behind a drag.

The queue is what bounds a misbehaving publisher: consecutive moves coalesce
onto the tail, so flooding costs a memory write rather than an unbounded queue,
and what the phone eventually sees is where the finger actually is. Only
down/up can accumulate; past 64 queued reports they are dropped with a
rate-limited warning (`event channel backed up`), which only happens if the link
itself has stalled. The queue is also cleared when the event channel closes, so
a gesture orphaned by a disconnect cannot inject a phantom contact into the next
session.

All of those rules live in `airplay::EventQueue` (`libs/airplay/event_queue.h`),
which deliberately holds no mutex, no clock and no socket — `Receiver` supplies
all three. `take(now)` is a pure decision given the queue state and an injected
time, so `airplay_test_event_queue` covers ordering, coalescing, keyframe
priority, the rate limit and the drop path with no threads and no hardware.
`eventSendLoop()` is left with only threading and I/O.

One thing that extraction turned up: the "last touch sent" timestamp used to be
left at its default, which is the clock epoch — making *never sent*
indistinguishable from *sent at time zero*, so the first report of a session was
rate limited against it. Real `steady_clock` values are far enough past the
epoch that this never showed up in practice. It now lives in `RateGate` as an
explicit flag, fixed once for both users, and both test suites assert at the
epoch precisely because that is the value that breaks.

### Testing the touch path without hardware

Three levels, none of which need a phone, the driver node, or the dashboard:

| Test | Scope | Cost |
|---|---|---|
| `carplay_test_touch_throttle` | widget throttle policy, time injected — interval boundaries to the nanosecond, deferral state machine, gesture transitions | instant |
| `airplay_test_event_queue` | node queue policy, time injected — ordering, coalescing, keyframe priority, drop path | instant |
| `carplay_test_touch_rate` | a real `CarPlayWidget` and a real zenoh subscriber driven with synthetic mouse events, headless | ~2.3 s of wall clock |

The first two are where behaviour is pinned; the third is what proves the policy
is actually wired to the timer and the publisher, which a pure unit test cannot
see. Keep it, but do not add cases to it that the unit tests could hold
exactly — its rate assertion has to use loose bounds because it measures real
elapsed time on a possibly-loaded machine.

### The non-touch input devices (knob, media keys, telephony, Siri)

Touch is not the only input CarPlay takes. Since 2026-08-02 the accessory
advertises **four** HID devices in `/info` rather than one — a touchscreen, a
rotary controller (select/home/back, a pointer, a detent wheel), consumer media
keys, and a telephony keypad — all in `libs/airplay/hid.cpp`. Siri is not HID;
it is a `requestSiri` command on the same event channel.

This is what the `knob`, `mediaKey`, `telephony` and `siri` kinds on
`nodes/carplay/input` have always claimed to be for. They previously fell
through a `break` in `usb_pipeline.cpp` and went nowhere.

`schemas/carplay_input.capnp` documents what `code` and `value` mean per kind.
The media and telephony codes **are the HID usage indices** in the descriptors
we advertise, so they cannot be renumbered independently of `hid.h`.

Nothing publishes these events yet — no widget has a knob or hard keys wired to
it — so on hardware this is exercised by publishing to the topic directly.

Two things make an input device fail silently, and both are what
`airplay_test_hid` checks:

- **A descriptor that disagrees with the report.** The phone accepts the device,
  then discards every report whose length does not match what the descriptor
  declared, with no diagnostic anywhere. The test parses each descriptor's item
  stream, sums its Input item bits, and compares against the report the code
  actually builds.
- **A uuid that does not match.** The device's `uuid` in `/info` and the `uuid`
  on the report are matched as strings, so `2a2a2a2b` and `0x2A2A2A2B` are two
  different devices, one of which does not exist.

Momentary presses are sent as press-then-release pairs, because the phone acts
on the transition: a media key that is never released is a key the phone stops
believing in. The knob's wheel and pointer are *relative*, so a turn is one
report and needs no release — but `sendKnob()` sends the all-clear anyway, since
the same report carries the button levels.

## 9. Audio downlink

**Expect:** music and navigation prompts play through the widget's `QAudioSink`;
`inspect hz nodes/carplay/audio` shows a steady rate matching the sample rate.

**Verified on hardware (2026-07-22): LPCM audio works.** Playing music opened a
type-100 `media` stream at 44.1 kHz stereo, ~134 packets/s decrypted with zero
failures, published on zenoh and played through the sink:

```
[airplay] audio stream type 100 'media' -> 44100 Hz 2 ch, dataPort ...
[audio]   first packet on type 100 'media' (44100 Hz, 2 ch)
[carplay] audio sink started: 44100 Hz / 2 ch
```

**How audio differs from video:**
- **Streams are on-demand.** The phone opens an audio stream only when there is
  something to play. An idle CarPlay screen requests no audio stream at all —
  play music or start navigation to trigger one. Do not expect audio at RECORD.
- **Transport is UDP, not TCP.** Each audio SETUP asks for a `dataPort` *and* a
  `controlPort` (both UDP); the response must echo `streamConnectionID` or the
  phone tears the stream down.
- **Packet layout** is `[12B RTP header][ciphertext][16B tag][8B nonce LE]`,
  ChaCha20-Poly1305 with AAD = the RTP header's timestamp+SSRC (bytes 4..12) and
  nonce = four zero bytes + the 8-byte tail. Same per-stream
  `DataStream-Salt<id>` / `DataStream-Output-Encryption-Key` derivation as video.
- **PCM is 16-bit big-endian on the wire** and must be byte-swapped to S16LE for
  the sink.

**Only LPCM is decoded.** `/info` advertises PCM formats for stream types 100 and
101 (nav prompts, Siri, calls, alerts, and PCM music), so those work with no
codec dependency. **Type 102 (buffered entertainment/music) is AAC-LC only** in
CarPlay and is not decoded yet — a type-102 SETUP is answered so the session
stays healthy, but produces no sound. Decoding it needs an AAC-LC decoder
(libavcodec has one); see LIVI `rtpAudioDecoder.ts` for the RTP jitter-buffer
pacing. The mic uplink (`DataStream-Input-Encryption-Key`, OPUS/PCM encode for
Siri and calls) is also not implemented.

**Playback architecture.** The widget plays through `QAudioSink` in **pull
mode**: the network thread pushes decrypted PCM into a thread-safe ring
(`dashboard/widgets/carplay/audio_ring.*`) and the sink's own audio thread pulls
at the sample-clock rate, with a short priming cushion and silence-fill on
shortfall. This decouples the bursty network delivery from steady playback and,
unlike the earlier push-mode path, never silently drops samples on a short
write. `AIRPLAY_DUMP_AUDIO=/path.pcm` on the driver writes the raw S16LE for
`aplay -f S16_LE -r <rate> -c <ch>` — the definitive way to isolate playback
from data.

**⚠ Choppy audio is usually the host, not this code.** Verified 2026-07-22 on a
VMware guest: the LPCM data was clean (0 decrypt failures, 0 source-side gaps,
delivered at exactly 1.0× real time), yet playback stuttered — and so did a
YouTube video and a raw `aplay` of the dumped PCM. The tell in the ring stats is
**zero underruns but steadily growing overruns**: the audio device is draining
*slower than real time*, so the ring fills and drops the oldest samples. That is
a host problem — an emulated audio device (VMware HD Audio) under CPU contention
(load ~3.2 on 4 vCPUs) cannot sustain real-time playback. No amount of buffering
fixes a device that will not drain at 1×. Remedy at the VM/host level (more
vCPUs, a lighter load, host audio backend, larger PipeWire quantum), not here.
Genuinely choppy *data* would instead show `[audio] inter-packet gap` warnings
from the driver.

## 10. Metadata, mic, and supplemental widgets

```bash
./build/nodes/inspect/inspect dump nodes/carplay/nowplaying   # play music
./build/nodes/inspect/inspect dump nodes/carplay/nav          # start navigation
./build/nodes/inspect/inspect dump nodes/carplay/call         # place a call
```

**Now-playing is wired and verified (2026-07-22).** Metadata comes over the
**iAP2 carkit channel** (stage 5), *not* AirPlay: after MFi auth succeeds the
session sends `StartNowPlayingUpdates`, then decodes each `NowPlayingUpdate`
(0x5001) and publishes to `nodes/carplay/nowplaying`. Two things to know:

- **Updates are partial.** A track change carries title/artist/album/duration;
  a tick may carry only `elapsed`. `usb_pipeline.cpp` merges each update into a
  persistent state before publishing, so absent fields are not cleared.
- **They are re-published every 2 s.** zenoh has no retained messages, so a
  dashboard that connects while a track is *paused* (no fresh updates) would
  otherwise show nothing. The republish keeps late joiners fed.

Verified on hardware: a paused Music track published
`American Dream / Alabama Shakes / I Must Be Dreaming`, merged from separate
partial updates, with duration and elapsed for the progress bar.

**Album artwork is wired and verified (2026-07-22).** After a track change the
phone pushes the cover image over the iAP2 **file-transfer session** (id 12),
automatically — no per-track request. The receiver in `iap2_session.cpp` handles
the datagram protocol (`SETUP`→ack `START`, accumulate `FIRST/DATA/LAST`,
complete→ack `SUCCESS`) and hands the assembled JPEG to the artwork handler,
which folds it into the now-playing state and bumps `album_art_seq`. The widget
caches by that sequence and only re-decodes on change, so the 2 s metadata
republish does not thrash it. Verified: a 99,563-byte JPEG arrived intact and
matched the track (Alabama Shakes cover).

**Microphone uplink is implemented; control path verified on hardware
(2026-07-22).** When the phone wants mic audio (Siri, a call) its main-audio
(type 100) SETUP carries a `dataPort` of *its own* — that is the signal to send
captured audio there. The receiver then:

1. derives the **input** key (same `DataStream-Salt<id>`, `-Input-Encryption-Key`
   rather than `-Output-`),
2. fires `MicStatusHandler` → session `mic_active` → the widget starts its
   `QAudioSource` and publishes captured PCM on `nodes/carplay/mic`,
3. `feedMic()` frames that PCM (framesPerPacket, else 20 ms) and RTP+encrypts
   each frame to the phone — an exact mirror of the working downlink (BE PCM,
   AAD = RTP timestamp+SSRC, `nonce64` counter, `[hdr][ct+tag][nonce8]`).

Verified on hardware up to the audio: triggering Siri opened the uplink
(`[audio] mic uplink up: [fe80::…]:62672 44100 Hz 1 ch, 882 samples/frame`), the
widget started capture, and the framing/keying matched the downlink. **End-to-end
voice was not confirmed on the VM** — its microphone is near-silent (peak ~194)
on the same emulated audio stack that stutters playback, and the capture happens
in the *dashboard*, which runs on the Mac. Like audio playback (which the VM
mangled but the Mac plays cleanly), confirm Siri/calls on real host audio.

**Navigation and call metadata are wired and verified on hardware (2026-07-23).**
Both follow the now-playing pattern: after auth the session subscribes
(`StartRouteGuidanceUpdates`, `StartCallStateUpdates`) and routes decoded updates
to `nodes/carplay/nav` and `nodes/carplay/call`, merged and re-published every 2 s.

Verified with a live route and a real call:

```
[iap2] navigation: state=1 road '...' -> 'Wagyu Factory'
[node] nav publish: active=true dest='Wagyu Factory' toManeuver=19m remain=30337m eta_in=1860s
[iap2] call: active ('(714) 338-2330' / '7143382330')
[iap2] call: ended ('' / '')
```

Two field-mapping details that hardware settled:

- **`nav.active` derives from the route-guidance `state`.** Observed values:
  `0` = not routing, `1` = actively guiding (destination present), `3` =
  transient (calculating). `active = state != 0` is correct — during navigation
  the phone holds `state=1`.
- **`nav` distances are named the opposite of intuition in the iAP2 struct.**
  `distance_remaining_m` is total-to-destination, `distance_to_maneuver_m` is to
  the next turn (`usb_pipeline.cpp` maps them correctly).
- **`current_road_name` is only sent when the phone knows the current road** —
  i.e. when it can place the car on a road from GPS/movement. On a stationary
  bench phone it is often empty; it populated as "Canyon Rd" when the
  route start resolved. Not a bug: the field decodes correctly, the phone just
  omits it.

To re-check, run the driver (`--max-stage 5`+), start turn-by-turn in Maps and
place/receive a call, then `inspect dump -k nodes/carplay/nav` /
`.../call`, and watch the `[iap2] navigation:` / `[iap2] call:` log lines.

**AAC-LC entertainment audio (type 102) is implemented; not yet hardware-tested
(2026-07-23).** The buffered music stream is AAC-LC, not PCM. `/info` now
advertises AAC-LC (0x400000) for type 102, and `libs/airplay/aac_decoder.cpp`
decodes each raw access unit to S16 PCM with libavcodec (no GStreamer/external
process — libavcodec is already linked for video). The decrypted RTP payload is
a *raw* AAC-LC access unit (no ADTS), so the decoder is configured with a
2-byte AudioSpecificConfig built from the negotiated rate/channels.

The decode path is **unit-tested without hardware** by `airplay_test_aac`, which
encodes a 440 Hz tone to AAC-LC and round-trips it through the decoder at 44.1k
and 48k — proving the ASC/extradata and the float→S16 conversion.

**⚠ Hardware finding (2026-07-23): this wired iPhone never uses the AAC stream.**
It routes all music through **type 100 as PCM**, even when `/info` advertises
AAC-LC for type 102. This was probed by temporarily withdrawing the PCM `media`
option from type 100, leaving only type-102 AAC: the phone did *not* switch to
AAC — it declined to route audio to CarPlay at all and fell back to playing
through its own speaker. So AAC-LC (type 102) is a **wireless-path codec** in
practice; the wired path we drive uses PCM and it works cleanly. The decoder
stays as a verified-correct fallback for any phone that does send type 102, but
it could not be exercised end-to-end here (withdrawing PCM just breaks wired
audio, so type 102 stays advertised only as an addition, never a replacement).

**GPS location uplink is implemented; not yet hardware-tested (2026-07-23).**
CarPlay lets the head unit feed the phone the car's own GPS so the phone can
dead-reckon where its signal is weak (tunnels, garages). The phone requests it
with `StartLocationInformation` (0xFFFA), naming which NMEA families it wants
(GGA/RMC/GSV/VTG as presence flags); the session answers with
`LocationInformation` messages carrying NMEA sentences at ~1 Hz until
`StopLocationInformation`.

- **Sentence generation** lives in `libs/iap2/location_nmea.cpp` (GGA + RMC,
  which cover what the phone needs; GSV/VTG are not generated). Unit-tested by
  `iap2_test_nmea` — coordinate `ddmm.mmmm` encoding, hemispheres, all fields,
  and the XOR checksum, against a known fix.
- **The fix source is a zenoh topic**, `nodes/carplay/location`
  (`CarPlayLocation` schema): any GPS source publishes fixes, the driver caches
  the latest and uplinks it. This mirrors how mic/input come from the dashboard
  side.

*To test on hardware without a GPS device*, feed a static fix and start
navigation (which is what makes the phone ask for location):

```bash
./build/nodes/carplay/carplay --location "37.3349,-122.00902,5,12.3,87.6" --verbose
# lat,lon[,altitude_m,speed_knots,course_deg]
```

Start turn-by-turn in Maps, then watch the driver log for
`[iap2] location requested: GGA=... RMC=...` followed by the ~1 Hz uplink. A
real GPS source instead publishes `CarPlayLocation` on `nodes/carplay/location`.

**Nav/maps rendering — what the phone actually provides.** Over iAP2 the phone
sends turn-by-turn *metadata* (road name, next-maneuver type, turn angle,
distance-to-turn, distance/time remaining, ETA) — already decoded and published
on `nodes/carplay/nav`. It does **not** send map imagery over iAP2; the live map
is inside the CarPlay video stream we already render. So a richer nav experience
is a *dashboard widget* concern (a cluster-style turn-by-turn card rendering the
`nav` topic), not more protocol. There is no such widget yet.

**Still not done:** the OPUS codec (wireless-only; the wired path we drive uses
PCM/AAC), a dedicated turn-by-turn nav widget, and GSV/VTG NMEA sentences (the
phone works with GGA+RMC).

## 11. The manufacturer button

CarPlay draws one tile on its own home screen for the vehicle manufacturer. The
user presses it to hand the screen back to the head unit's native UI. Both
halves are implemented (2026-08-01), neither is hardware-verified.

**What we advertise.** `GET /info` carries `oemIconVisible`, `oemIconLabel` and
an `oemIcons` array (one entry per rendition: `imageData`, `widthPixels`,
`heightPixels`, `prerendered`). Built by `addOemButtonInfo()` in
`libs/airplay/oem_button.cpp` and unit-tested by `airplay_test_oem_button`.

It is advertised **once**, at `/info` time. There is no way to show or hide the
button mid-session, so a config change needs a new session to take effect.

**How the press comes back.** The phone posts `requestUI` on the encrypted event
channel. The *same* command carries an app asking the head unit to open a
specific url, so the two are told apart by whether `params.url` is present and
non-empty — no url means the button. `isOemButtonPress()` is the predicate;
`Receiver::handleEventCommand()` routes it to the `OemButtonHandler`, which
today only logs:

```
[airplay] manufacturer button pressed -- phone is asking for the vehicle's own UI
[node] manufacturer button pressed -- returning to the vehicle's UI is not wired up yet
```

Nothing is hooked to it yet. The action belongs in the node's handler in
`usb_pipeline.cpp` — for this dashboard, telling the widget stack to leave the
CarPlay page.

**Configuring it.**

```bash
./build/nodes/carplay/carplay --config configs/carplay/carplay.yaml --verbose
./build/nodes/carplay/carplay --oem-button=false      # advertise no button
./build/nodes/carplay/carplay --oem-label "Mercedes"  # override the caption
```

`configs/carplay/carplay.yaml` documents the fields. The artwork it points at is
generated by `configs/carplay/make_oem_icon.py` (a steering wheel, at 60/120/180
px) — re-run that only if the icons change; the PNGs are committed. Icon
dimensions are read from each file's PNG header, so a config only names paths.
Without `--config` the button is still advertised, with the default label and no
artwork; the node warns, because CarPlay then draws its own placeholder, which
looks enough like a working button to hide the mistake.

**What to check on hardware.** In order: the tile appears on CarPlay's home
screen (advertisement accepted); it shows our steering wheel rather than a
placeholder (`oemIcons` accepted); the caption reads right and is not truncated;
pressing it logs the two lines above.

If the tile appears but the artwork does not, the icon *entry* keys are the
suspect, not the top-level ones — try a single 120 px entry, and try
`prerendered: true`, before touching anything else. Turning on `--verbose`
prints every unrouted event command with its full body, which is where a
differently-named press would show up.

## What exists today (read before starting)

Not all stages below are implemented yet. Current state:

| Layer | State |
|---|---|
| **USB transport (libusb)** | **rewritten 2026-08-01, NOT re-verified on hardware** — see stage 1b. Descriptor discovery unit-tested (`apple_usb_test_ncm_discovery`); stages 2, 3 and 6 need re-running |
| USB detect, config-6 switch, usbmux, usbmuxd socket | verified on hardware 2026-07-21 (stages 2–3), **against the pre-libusb transport** |
| Property lists (binary + XML), ours | **replaces libplist** in the usbmuxd server; differential-tested against libplist, verified on hardware |
| usbmux client, ours | **replaces libusbmuxd**'s role; mock-tested and verified on hardware via `apple_usb_muxctl` |
| lockdown client + TLS + pair record + pairing, ours | **the only implementation** — libimobiledevice removed 2026-07-31. Pairs from scratch; 25 consecutive clean runs |
| iAP2 link layer, identification, MFi auth | **verified on hardware 2026-07-21** (stage 5 complete) |
| iAP2 metadata decode | written, unit-tested |
| NCM ↔ TAP bridge | verified on hardware 2026-07-21 (stage 6); **discovery half rewritten on libusb 2026-08-01**, TAP/IP half untouched |
| NTB16 framing (`ncm_frame.cpp`) | extracted from the bridge 2026-08-01 and unit-tested for the first time (`apple_usb_test_ncm_frame`, 60 assertions). Framing logic itself unchanged and hardware-proven; the extraction did surface one latent bug in the EUI-64 derivation (stage 6) |
| **macOS stages 1–7** | **verified on hardware 2026-08-01** — config switch under root, then system usbmuxd + lockdown TLS + carkit + `AppleUSBNCM` + iAP2/MFi auth + 975 decoded video frames, all unprivileged after the one-time switch. See "Running on macOS" |
| MCP2221A userspace driver | **three bugs fixed 2026-08-01** — every transfer was addressed to 0x00, the ACK bit was unmasked, and a NACK latched the engine. Presented as a wiring fault for a long time; it was not |
| AirPlay crypto/SRP/plist/NALU foundation | written, KAT-verified |
| **AirPlay RTSP session**: framing, pair-setup, pair-verify, encrypted channel, auth-setup, /info, SETUP, RECORD, clock sync | **written; handshake verified on hardware** |
| **AirPlay screen stream** (H.264 decode to Annex-B, published on zenoh) | **working, verified on hardware** |
| **Late-joining renderer sync** | working — periodic `forceKeyFrame` over the event channel |
| **Widget render (YUVJ420P)** | working — full CarPlay home screen renders |
| **Event channel + touch HID** | **verified on hardware** (single-touch + drag) |
| **Knob / media key / telephony HID + Siri** | written 2026-08-02, unit-tested (`airplay_test_hid`), **not hardware-verified**. Advertised in `/info` and wired to the `input` topic; nothing publishes them yet |
| **Event channel inbound commands** | written 2026-08-01 — the phone's own commands are now parsed and acknowledged (they were previously read and discarded); only `requestUI` is routed, the rest are logged with their body |
| **Manufacturer button** (`/info` advertisement + press decode) | written 2026-08-01, unit-tested (`airplay_test_oem_button`), **not hardware-verified**; the press is logged and goes nowhere — see stage 11 |
| **AirPlay audio downlink (PCM)** | **verified on hardware** (types 100/101) |
| **AirPlay audio downlink (AAC-LC, type 102)** | decode unit-tested (`airplay_test_aac`); the wired iPhone never routes music as AAC (uses PCM), so end-to-end unexercised — see stage 9 |
| **Microphone uplink** | written; control path verified on hardware; end-to-end voice pending real host audio |
| **Now-playing metadata + album art** | **verified on hardware** |
| **Navigation + call metadata** | **verified on hardware 2026-07-23** |
| **GPS location uplink** (car → phone, NMEA) | written; NMEA unit-tested (`iap2_test_nmea`); not yet hardware-tested |
| **Node orchestration**, stages 2–7 + metadata | done — `usb_pipeline.cpp` + `iap2_session.cpp`, driven by `--max-stage` |
| **Node orchestration** wiring NCM → airplay | **NOT YET WRITTEN** |
| zenoh bridge, widgets, audio, metadata topics | done, verified via `--simulate` |

Stages 2–4 run today via `--max-stage`, which stops the pipeline at a chosen
stage so a failure at one layer is not buried under the next layer failing as a
consequence:

```bash
./build/nodes/carplay/carplay --max-stage 2 --verbose   # detect + config switch
./build/nodes/carplay/carplay --max-stage 3 --verbose   # + mux + usbmuxd socket
./build/nodes/carplay/carplay --max-stage 4 --verbose   # + lockdown/carkit TLS
./build/nodes/carplay/carplay --max-stage 5 --verbose   # + iAP2 link, identification, MFi

# While the MFi board is out, run everything up to the certificate request:
./build/nodes/carplay/carplay --max-stage 5 --iap2-allow-missing-mfi --verbose
```

No `sudo` is needed for these once the stage 1 udev rules are installed.
**Stages 5–10 still need code**: the carkit channel is not yet wired to
`Iap2Transport`, and the AirPlay session layer does not exist. Until then the
driver publishes only idle session state, and `--simulate` exercises the
dashboard.

## Known-unverified list

Everything from USB up to the carkit TLS channel has run against a phone, but
**the USB transport underneath was replaced on 2026-08-01 and has not**. That is
now the top unverified item — see stage 1b for the ordered re-verification, and
note that its three highest risks are: the mux interface lookup landing somewhere
other than If1, the NCM detach-then-select ordering picking the second pair, and
NCM throughput under libusb's synchronous API.

Ranked by remaining uncertainty (highest first):

**The end-to-end path is proven: the CarPlay home screen renders in the
dashboard widget.** Driver → USB → AirPlay → zenoh → widget → screen. Remaining:

1. **Audio streams.** The video path is complete; audio (stream types 100–102,
   PCM/OPUS/AAC-LC) is not implemented. `audioFormats` is already advertised in
   `/info`, so the phone may request an audio stream SETUP — handle it in
   `handleSetup` alongside type 110. See LIVI `audioStream.ts` / `rtpAudioDecoder.ts`.
2. **Touch round-trip.** The encrypted event channel is up and `sendTouch()`
   pushes `hidSendReport` multitouch reports over it, wired to the dashboard's
   input topic. Confirm on screen that taps register.

**The "one frame then stops" earlier symptom was `viewAreas`, now fixed** — see
above. With it in place the phone streams continuously (100+ frames observed).
2. **Audio pacing** — timing-dependent, cannot be desk-checked.

**Retired by the 2026-07-21 hardware session:**
- ~~usbmuxd socket bridge~~ — `idevice_id -l` and `ideviceinfo` both work through
  our socket, exercising the plist framing and the `Connect` relay.
- ~~Lockdown/carkit glue~~ — compiles and reaches
  `[carkit] carkit TLS channel up (iAP2)`. One real bug found and fixed: the
  sysfs UDID needs the libusbmuxd dash normalisation (stage 4).
- ~~iAP2 link layer and wired identification~~ — negotiates against a real phone
  and identification is **accepted first try**, no rejection round needed.
- ~~iAP2 retransmission/EAK timers~~ — not a risk on this path: the phone
  advertises `max_retransmissions=0 max_ack=0`, so they never run.
- ~~Outbound fragmentation off-by-ten~~ — `max_len` is 65535 as assumed.
- ~~NCM enumeration / altsetting / pair selection~~ — all correct on hardware.
  The real defect was the undrained interrupt endpoint, which no amount of
  framing verification would have caught.
- ~~NTB16 framing on the wire~~ — 3960 blocks accepted by the phone with no
  errors, confirming the differential testing against LIVI.
- ~~MFi authentication~~ — certificate (908 B) accepted and a 20-byte SHA-1
  challenge signed; the phone answers `AuthenticationSucceeded`. Protocol major
  is **2** on this CP2.0C part, so the SHA-1/20-byte branch is the live one.
- ~~Zero-length iAP2 bools~~ — **did not occur.** The phone sends a proper
  1-byte boolean, so `wired_available` decodes as `true` and
  `CarPlayStartSession` is not suppressed. See the note under stage 5: the
  behaviour is still worth knowing, because it fails silently if a different
  phone does send one.

Deliberately *lower* risk than they look, because they are verified:
NTB16 framing (byte-identical to LIVI across 30 cases), the crypto primitives
and SRP (90 KAT assertions), iAP2 framing/reassembly/fragmentation (~150
assertions), and the entire dashboard-side pipeline (`--simulate`).
