---
title: CarPlay port
parent: Design notes
---

# CarPlay port

## Overview

These are the working notes from porting a wired CarPlay stack to C++ and
bringing it up against a phone: what each layer turned out to do on hardware,
what was tried and reverted, and why the code is shaped the way it is. They are
dated where it matters and are not maintained as a manual. How to run the node
is in [carplay](../nodes/carplay.html); this page is for the developer who wants
to know why something is the way it is before changing it.

The code was written in bulk on a macOS dev box without an iPhone, MFi
coprocessor or Linux host attached, then stepped through on hardware one layer
at a time between 2026-07-21 and 2026-08-02, with one later measurement of the
coprocessor's timing on 2026-09-13. The stack is a port of LIVI
(https://github.com/f-io/LIVI, GPL-3.0). Its AirPlay/RTSP layer lives in
`src/main/services/projection/driver/cp/stack/`: `cpStack.ts` (request
dispatch), `getInfo.ts` (the `/info` plist), `timingServer.ts`,
`screenStream.ts`, `hid.ts`. When a handshake step is rejected by the phone and
the reason is not observable, read the corresponding file there rather than
permuting: the `/auth-setup` byte layout and the `/info` display keys were both
settled that way in minutes after an hour of guessing.

The section order follows the bring-up stages the node's `--max-stage` counts
through: USB transport, usbmux and lockdown, iAP2 and MFi, the NCM link, the
AirPlay session, then video, input, audio and metadata, and finally the
comparison with LIVI and the dated status tables.

## Status

The full pipeline works end to end (2026-07-22): the CarPlay home screen renders
live in the dashboard widget. USB configuration switch, usbmux, lockdown/carkit
TLS, iAP2 and MFi authentication, the NCM link, and the AirPlay session through
to H.264 decoded and drawn on screen via zenoh. Stages 1 through 6 had been
verified the day before (2026-07-21); stage 7 landed the next day. Since
2026-08-01 stages 1 through 7 also run on macOS, verified on hardware.

### What exists today

The table below was last revised across the 2026-08-01 and 2026-08-02 sessions.
Rows are dated individually.

| Layer | State |
|---|---|
| USB transport (libusb) | rewritten 2026-08-01 and re-verified on hardware the same day (see The USB transport). Descriptor discovery unit-tested (`apple_usb_test_ncm_discovery`) |
| USB detect, config-6 switch, usbmux, usbmuxd socket | verified on hardware 2026-07-21 against the pre-libusb transport, and again 2026-08-01 on libusb |
| Property lists (binary + XML), ours | replaces libplist in the usbmuxd server; differential-tested against libplist, verified on hardware |
| usbmux client, ours | replaces libusbmuxd's role; mock-tested and verified on hardware via `apple_usb_muxctl` |
| lockdown client + TLS + pair record + pairing, ours | the only implementation; libimobiledevice removed 2026-07-31. Pairs from scratch; 25 consecutive clean runs |
| iAP2 link layer, identification, MFi auth | verified on hardware 2026-07-21 |
| iAP2 metadata decode | written, unit-tested, verified on hardware 2026-07-22 and 2026-07-23 |
| NCM link | rebuilt on the system NCM driver 2026-08-02; the userspace bridge and its TAP are deleted; verified on hardware, 4713 frames over three minutes with no errors, and measured against the old path |
| NTB16 framing | deleted 2026-08-02; the kernel NCM driver frames the AV path now, so this code and its unit suite are gone along with the bridge |
| macOS stages 1 through 7 | verified on hardware 2026-08-01: config switch under root, then system usbmuxd + lockdown TLS + carkit + `AppleUSBNCM` + iAP2/MFi auth + 975 decoded video frames, all unprivileged after the one-time switch |
| MCP2221A userspace driver | three bugs fixed 2026-08-01: every transfer was addressed to 0x00, the ACK bit was unmasked, and a NACK latched the engine. Presented as a wiring fault for a long time; it was not |
| AirPlay crypto/SRP/plist/NALU foundation | written, KAT-verified |
| AirPlay RTSP session: framing, pair-setup, pair-verify, encrypted channel, auth-setup, /info, SETUP, RECORD, clock sync | handshake verified on hardware |
| AirPlay screen stream (H.264 decode to Annex-B, published on zenoh) | working, verified on hardware |
| Late-joining renderer sync | working; `forceKeyFrame` over the event channel, gated on subscriber matching since 2026-08-02 |
| Widget render (YUVJ420P) | working; full CarPlay home screen renders |
| Event channel + touch HID | verified on hardware (single touch and drag) |
| Knob / media key / telephony HID + Siri | written 2026-08-02, unit-tested (`airplay_test_hid`), not hardware-verified. Advertised in `/info` and wired to the `input` topic; nothing publishes them yet |
| Event channel inbound commands | written 2026-08-01; the phone's own commands are parsed and acknowledged (they were previously read and discarded). `requestUI`, `modesChanged`, `duckAudio`/`unduckAudio`, `suggestUI` and `disableBluetooth` routed 2026-08-02 |
| Session lifecycle (TEARDOWN) | written 2026-08-02; TEARDOWN was acknowledged and otherwise ignored, so the dashboard never learned a session had ended. Not hardware-verified |
| `POST /feedback` media clock | written 2026-08-02; names the open audio streams instead of answering empty. No playback anchor |
| Night mode | written 2026-08-02, set by `night_mode:` in the config and reflected in `CarPlaySessionState.nightMode`. No light sensor drives it; not hardware-verified |
| Keepalive port | written 2026-08-02; `/info` advertised `keepAliveLowPower` with no port behind it |
| Cluster (alt) display, 48 kHz entertainment audio | deliberately not done; see What LIVI has that we do not |
| Manufacturer button (`/info` advertisement + press decode) | fully verified on hardware 2026-08-02: tile, label, artwork (needs `prerendered: true`) and the press. Nothing is hooked to the handler yet |
| AirPlay audio downlink (PCM) | verified on hardware 2026-07-22 (types 100/101) |
| HEVC (H.265) | verified on hardware 2026-08-02; off by default, `display.allow_hevc` turns it on |
| AirPlay audio downlink (AAC-LC, type 102) | decode unit-tested (`airplay_test_aac`); the wired iPhone never routes music as AAC (uses PCM), so end-to-end unexercised |
| Microphone uplink | written; control path verified on hardware 2026-07-22; end-to-end voice pending real host audio |
| Now-playing metadata + album art | verified on hardware 2026-07-22 |
| Navigation + call metadata | verified on hardware 2026-07-23 |
| GPS location uplink (car to phone, NMEA) | written; NMEA unit-tested (`iap2_test_nmea`); not yet hardware-tested |
| Node orchestration, stages 2 through 7 + metadata | done: `usb_pipeline.cpp` + `iap2_session.cpp`, driven by `--max-stage` |
| zenoh bridge, widgets, audio, metadata topics | done, verified via `--simulate` |

An earlier revision of this table, before stages 5 through 7 were written, said
"stages 5 through 10 still need code: the carkit channel is not yet wired to
`Iap2Transport`, and the AirPlay session layer does not exist. Until then the
driver publishes only idle session state, and `--simulate` exercises the
dashboard." That is no longer true and is recorded only so the phrase is not
mistaken for current state if it turns up in an old commit.

### Known-unverified list

Ranked by remaining uncertainty, highest first, as of 2026-08-02:

1. End-to-end voice through the microphone uplink on real host audio (the VM's
   microphone was near-silent).
2. Audio pacing under a buffered (type 102) stream, which no wired phone has
   opened; timing-dependent, cannot be desk-checked.
3. Everything written on 2026-08-02 without a phone afterwards: TEARDOWN
   handling, the `/feedback` answer, night mode, the keepalive port and the
   non-touch HID devices.
4. The GPS location uplink against a phone actually asking for it.
5. The iAP2 retransmission/EAK timers and the fragmentation off-by-ten, which
   the wired path never exercises (the phone advertises zero retransmissions and
   a 65535-byte `max_len`).

Two items that led this list at earlier dates are closed. The libusb transport,
rewritten 2026-08-01, was re-verified the same day; its three risks (the mux
interface lookup landing somewhere other than If1, the NCM detach-then-select
ordering picking the second pair, NCM throughput under libusb's synchronous API)
all measured fine, and the third became moot when the kernel took over the AV
path on 2026-08-02. Audio streams and the touch round-trip, listed as remaining
on 2026-07-22, were verified the same and following day.

The "one frame then stops" symptom from the first AirPlay session was
`viewAreas`, now fixed; with it in place the phone streams continuously.

### Retired by the 2026-07-21 hardware session

The usbmuxd socket bridge: `idevice_id -l` and `ideviceinfo` both work through
our socket, exercising the plist framing and the `Connect` relay. The
lockdown/carkit glue reaches `[carkit] carkit TLS channel up (iAP2)`; one real
bug found and fixed, the UDID dash normalisation. The iAP2 link layer and wired
identification negotiate against a real phone and identification is accepted
first try, no rejection round needed. The iAP2 retransmission/EAK timers are not
a risk on this path: the phone advertises `max_retransmissions=0 max_ack=0`, so
they never run. The outbound fragmentation off-by-ten is invisible because
`max_len` is 65535 as assumed. NCM enumeration, altsetting and pair selection
were all correct on hardware; the real defect was the undrained interrupt
endpoint, which no amount of framing verification would have caught. NTB16
framing was accepted by the phone for 3960 blocks with no errors, confirming the
differential testing against LIVI (both are gone now). MFi authentication: the
certificate (908 B) was accepted and a 20-byte SHA-1 challenge signed; the phone
answers `AuthenticationSucceeded`. Protocol major is 2 on this CP2.0C part, so
the SHA-1/20-byte branch is the live one. Zero-length iAP2 booleans did not
occur: the phone sends a proper 1-byte boolean, so `wired_available` decodes as
`true` and `CarPlayStartSession` is not suppressed.

Deliberately lower risk than they look, because they are verified: the crypto
primitives and SRP (90 KAT assertions), iAP2 framing/reassembly/fragmentation
(about 150 assertions), and the entire dashboard-side pipeline (`--simulate`).

## Tests

The hardware-free tests are the regression net for the pure-logic layers, and
they must pass before anyone touches hardware; a failure there is a logic bug.

```bash
cmake --build build -j4     # -j unbounded OOMs on an 8 GB box; zenoh's Rust build is the hog
ctest --test-dir build --output-on-failure
```

That runs every test in the repository in about seven seconds. Every test
carries its component as a label plus at least one of `unit` (pure logic, no
sockets, clock, hardware or display), `net` (opens a zenoh session), `gui`
(constructs Qt widgets, forced offscreen) and `slow`. The registration helper is
`cmake/ProjectTest.cmake`; a new test is one `add_project_test()` line next to
its `add_executable`.

```bash
ctest --test-dir build -L unit      # the deterministic ones, well under a second
ctest --test-dir build -L airplay   # only the AirPlay layer
ctest --test-dir build -LE slow     # skip anything that measures real elapsed time
ctest --test-dir build -j8          # they are independent; this is safe
ctest --test-dir build -R pairing   # by name
```

| Test | Covers |
|---|---|
| `airplay_test_tlv8` | TLV8 encode/decode + fragmentation |
| `airplay_test_crypto` | HKDF/ChaCha20/X25519/Ed25519/SRP known-answer vectors |
| `airplay_test_channel_crypto` | encrypted-channel framing, and the nonce lockstep both ends depend on |
| `airplay_test_pairing_session` | pair-setup/verify/auth-setup, driven from the phone's side |
| `airplay_test_pairing_store` | the persisted identity and pairings |
| `airplay_test_info_plist` | the GET /info capability declaration |
| `airplay_test_hid` | HID descriptors against the reports sent on them |
| `airplay_test_oem_button` | manufacturer button: /info keys + press decode |
| `airplay_test_event_queue` | event-channel ordering, coalescing, priority, drops |
| `airplay_test_rtsp` | message framing, which every byte from the phone passes through |
| `airplay_test_timing` | the NTP clock offset, whose absence tears the session down after RECORD |
| `airplay_test_mic_uplink` | the microphone packet's wire format and framing |
| `airplay_test_nalu` | avcC to Annex-B rewrite |
| `airplay_test_aac` | AAC-LC encode/decode round-trip (entertainment audio) |

Underneath: `plist_test_{binary,xml,libplist_vectors}`,
`iap2_test_{framing,csm,nmea}`,
`apple_usb_test_{ncm_discovery,pair_record,usbmux_client}`, plus
`carplay_test_node_config` for the config file and its PNG reader, and the touch
tests listed under Touch and the other inputs.

`iap2_test_csm` is worth knowing about for one thing in particular: it pins the
zero-length boolean (see iAP2 and the MFi coprocessor) as a hardware suspect. We
read one as absent, following LIVI, where the spec allows presence to mean true.
If a phone ever sends one, `CarPlayAvailability` reads falsy and the session
silently never starts.

There is no `airplay_test_plist`; the plist tests live in `libs/plist` under the
`plist_test_*` names. If you have one in a `build/` directory, it is a stale
binary from before the move and will keep passing after its target is gone,
which is a good reason to reconfigure from scratch rather than trust an old
build tree.

Simulation was verified on macOS on 2026-07-20 (video decode and render, audio
sink startup, metadata to widget, widget instantiation from YAML, and the publish
rates), so hardware stages 8 through 10 exercise only the phone-side half of
those paths. It also retired the plan's "zenoh video throughput" risk: a
4 Mbit/s 30 fps H.264 stream rides zenoh peer-to-peer on localhost without
backpressure, so the shared-memory fallback is not needed (measured again under
Video, below).

## The USB transport

### The libusb port (2026-08-01)

Enumeration, descriptor parsing, configuration selection, driver detach and
every transfer go through libusb (already vendored for hidapi,
`third_party/libusb.cmake`) since 2026-08-01. Re-verified on hardware the same
day against iPhone `00008140…`, the same phone as the 2026-07-21 session: all
five steps below pass, plus a full `--max-stage 7` session to decoded video. The
port introduced no regression; every failure hit during the re-verification was
environmental (see Conflicting daemons) except one genuine bug, in the TAP
link-local derivation, which was in unchanged code and is written up under The
NCM link.

Three consequences worth knowing before reading logs. Devices are tracked by
physical port, not by serial: `DeviceInfo` carries a `PortPath` (`bus-port.port`,
e.g. `1-4.2`) which is stable across the re-enumeration the vendor request
triggers, whereas the device address is not. The port path prints the same way
the kernel names the sysfs directory, so `/sys/bus/usb/devices/1-4.2` is still
the thing to `cat`. Enumeration no longer reports a UDID: reading it costs a
device open, so it happens once, in `populateSerial()`, before the config
switch, and a phone whose UDID cannot be read is rejected at detection rather
than later. And interfaces and endpoints come from the configuration descriptor:
the mux interface is found by its `255/254/2` class triple and the NCM pair by
walking CDC descriptors, instead of being hardcoded (`muxd.cpp`) or read out of
sysfs. The 2026-07-21 session recorded that the real phone matches both; this
port is what makes the code rely on that rather than on constants that happened
to agree.

### Verifying the transport in order

Each step isolates one assumption, so a failure names its own cause. Step 1 is
read-only and says whether steps 2 through 4 are worth attempting.

Step 1 is descriptors, without touching anything. `apple_usb_usbprobe` claims
nothing and changes nothing; it only enumerates and dumps (`--serial` also opens
each device for its UDID).

| Check | Expected | If wrong |
|---|---|---|
| Device listed at all | `05ac:…` with a port path | udev rules |
| `port=` matches sysfs | same string as the `/sys/bus/usb/devices` dir | port-path formatting bug |
| `nconfigs` | 5 before the vendor request, 6 after | configuration switch triage |
| `--serial` prints a UDID | 24/25 chars | `populateSerial` will reject the phone |
| An interface annotated `<- usbmux` | present in config 6 | `muxd` falls back; see below |
| Two `<- CDC-NCM control` interfaces | present in config 6 | NCM discovery will find fewer |
| Bulk endpoints on data alt 1 | `ep 0x…  bulk` under `alt 1` | `hasBulkPair()` fails |
| `cdc_subtype=0x0f (Ethernet Networking, iMACAddress=N)` | `N` non-zero | host MAC unreadable |

Reconciled against real configuration 6 on 2026-08-01, the fixture was right:
`carPlayLikeConfig()` in `test_ncm_discovery.cpp` matches a real iPhone in
configuration 6 exactly. All 11 interface alt settings, the `0xff/0xfd` iAP
interface with its three altsettings, endpoints `0x87`/`0x88`/`0x06`/`0x89`/
`0x07`, `iMACAddress` 18 on the first NCM function and 16 on the second, an
interrupt endpoint on the first NCM control interface and none on the second,
and every functional-descriptor byte including `wMaxSegmentSize` `0x3e8e` and
the NCM functional `06 24 1a 00 01 3b`. The comment claiming it is
"byte-for-byte what the phone reports" is now verified rather than asserted. If
the probe output ever disagrees with that fixture (different interface numbers,
endpoints on a different altsetting, a third NCM pair), correct the fixture
first and let the tests fail, then fix the code. Those tests are the only reason
any of this was verifiable without a phone.

Configuration 5 (what the phone boots into) is the same layout minus the iAP
interface, so every interface number and endpoint address below If2 shifts down
by one. Do not mistake a config-5 dump for a config-6 one.

Step 2 is the configuration switch, the step most likely to regress, because
`libusb_set_configuration` replaces a hand-written `USBDEVFS_SETCONFIGURATION`
ioctl. On Linux libusb issues that same ioctl, so the property the old code was
careful about, that selecting a configuration does not re-enumerate, unlike
writing sysfs, is preserved. Watch for the port path staying constant across the
vendor request while the config goes 4 to 6. A changing port path means the
phone was re-plugged or a hub re-enumerated, and the rediscovery keyed on it
will time out.

Step 3 is the mux. The line to look for is
`[muxd] mux on interface 1 (class ff/fe/02), bulk in 0x85 / out 0x04`, which
must match the hardcoded values the old code used and the 2026-07-21 session
confirmed. If instead you see `no ff/fe/02 interface in configuration 6; falling
back to interface 1`, the descriptor lookup is wrong for this phone; the
fallback keeps the stack working, but fix the lookup rather than leaving the
fallback as the live path.

Step 4 is NCM. Two lines replace the old sysfs walk, these from hardware on
2026-08-01:

```
[ncm] 2 NCM function pairs present; taking the first (control interface 3). Override with CARPLAY_NCM_CTRL_IF.
[ncm] NCM pair in configuration 6: control iface 3 (status ep 0x87), data iface 4 (bulk in 0x88 / out 0x06), iMACAddress string 18
```

Compare every field against the probe output, and confirm the first pair is
selected, not the second. The phone exposes two and the system NCM driver binds
the first, which is the one we want. If the log shows a control interface higher
than the first NCM one, descriptor discovery picked wrong;
`CARPLAY_NCM_CTRL_IF=<n>` pins it while you investigate.

Step 5 was throughput under load, the one change with a genuine performance
question: the AV data path used to run as two synchronous libusb pumps on one
handle, and libusb serialises its sync API on an event lock, so one pump could
service the other's completions where the old usbfs ioctls were independent.
Measured 2026-08-01 and found not to starve (3443 NTBs out / 3467 in, zero
errors, 2294 frames over about 100 s). The question is moot since 2026-08-02:
the AV data path is the kernel's now, so the only libusb traffic left is the mux
and lockdown, both low-rate. The async migration this step used to recommend is
not needed.

### What this port did not change

Nothing above the transport: usbmux framing, plists, lockdown, TLS, pairing,
iAP2 and AirPlay. If a failure appears in those layers after this port, suspect
the transport underneath rather than the layer reporting it, with one exception:
a phone rejected at detection for an unreadable UDID never reaches them at all.

### USB detection and the configuration switch

Verified on hardware 2026-07-21 against iPhone `00008140…`; the constants in
`usb_device.h`/`muxd.cpp` are all correct for this generation.

| Expectation | Result |
|---|---|
| `0xC0/0x52` reveals extra configurations | 5 became 6 configurations |
| `kCarPlayConfiguration = 6` | config 6 = `PTP + Apple Mobile Device + Apple USB Ethernet + NCM` |
| mux at If1, `kEpOut 0x04` / `kEpIn 0x85` | If1, vendor-specific 255/254/2 |
| `kNcmDataAltSetting = 1` | bulk endpoints live on alt 1 |

Two of those rows are no longer constants: since the libusb port the mux
interface is found by its `255/254/2` triple and its endpoints read from the
descriptor. The hardware row is what says that lookup lands on the same place.

The vendor request is sticky but not idempotent-looking: before it the phone
advertises 5 configurations (config 5 is `…+ NCM`, which looks tempting but is
not the CarPlay config), after it 6. Do not "fix" the constant to 5.

Applying the configuration needs the kernel drivers out of the way. The switch
is done with `libusb_set_configuration`, which on Linux issues
`USBDEVFS_SETCONFIGURATION` on the usbfs node; a udev rule can grant that to a
normal user, unlike the root-only sysfs attribute. The kernel returns `EBUSY`
while any interface is claimed, so every bound driver is released first with
`libusb_detach_kernel_driver` (also `USBDEVFS_DISCONNECT` underneath); `ipheth`
and an earlier usbfs client both hold interfaces in config 4. Unlike the vendor
request this does not re-enumerate the device: config 6 is active in about
100 ms and, on a VM, the passthrough binding survives. There is no fallback. The
root-only sysfs `bConfigurationValue` write inherited from the usbfs
implementation was removed on 2026-08-01, once libusb had been verified against
hardware; it covered a nearly empty case (root can open the usbfs node anyway)
and reached the configuration by re-enumerating, the one thing this step is
careful to avoid.

Triage for this stage: stuck at 4 or 5 configurations means the `0xC0/0x52`
vendor request failed (check for `EPERM`, and that the phone is unlocked and
trusted); `Failed to set configuration …` means libusb could not open the device
or the device rejected the request (udev rules, or root); a config that reverts
to 4 means something re-enumerated it, usually the system usbmuxd or
`usb_storage`/`ipheth` grabbing the device; and a device that vanishes after the
switch is expected briefly, since the code waits up to 5 s for re-enumeration,
but one that never returns points at a charge-only cable or a VM handing the
device back to the host.

### Conflicting daemons on Linux

The system `usbmuxd` will fight us for the phone, which is the core reason we
run our own mux on Linux. It holds interface 1, the exact vendor-specific
interface our mux claims (`kMuxInterface = 1`). Some distros ship only the
service unit and no socket unit; drop `usbmuxd.socket` from the command if
systemd reports it does not exist.

`systemctl stop` alone is not enough, and the reason is not obvious. The
package ships `/usr/lib/udev/rules.d/39-usbmuxd.rules`, which contains:

```
ACTION=="add", ... ATTR{bConfigurationValue}="0", OWNER="usbmux",
                   ENV{SYSTEMD_WANTS}="usbmuxd.service"
```

The configuration switch deliberately re-enumerates the phone, which fires
`add`, which restarts usbmuxd mid-test; it then claims interface 1 and the next
`libusb_set_configuration` fails with `EBUSY`. Observed exactly this on
2026-08-01: stopped at 13:18:59, restarted by udev at 13:21:50, six seconds
after the vendor request. The same rule also sets `bConfigurationValue=0`,
unconfiguring the phone on plug. So mask it or remove it. Removing the package
is safe and does not cascade: `libimobiledevice-utils` stays, and device-node
access comes from our own `99-carplay.rules` (`GROUP="plugdev"`,
`TAG+="uaccess"`), not from the rule's `OWNER="usbmux"`. Beware `apt autoremove`
afterwards: it will offer to take `libssl-dev` with it.

`gvfsd-gphoto2` is the other one, and it is easy to misread as usbmuxd.
Configuration 6 keeps a PTP/imaging interface at interface 0, so GNOME
auto-mounts the phone as a camera and holds a usbfs claim on it. Any claimed
interface makes `libusb_set_configuration` return `EBUSY`, so this blocks the
switch exactly the way usbmuxd does, but `systemctl` shows nothing wrong and
`lsusb -t` reports the interface as `usbfs`, not as a named driver. Find it by
owner: `ls -l /proc/*/fd 2>/dev/null | grep /dev/bus/usb`, or
`sudo fuser -v /dev/bus/usb/BBB/DDD`, then kill the pid.
`libusb_detach_kernel_driver` cannot help here: a live usbfs claim by another
process is not a kernel driver, and no ioctl takes it away.

Running in a VM: USB passthrough works, but the configuration switch
deliberately re-enumerates the phone, and hypervisors commonly hand a
re-enumerating device back to the host instead of the guest. If the phone
disappears and never returns within the 5 s window, suspect passthrough before
suspecting the driver; re-attach it to the guest and confirm with `lsusb` that
the VID is still visible from inside.

The udev rules give the phone's node group `plugdev` with an ACL (the trailing
`+` in `crw-rw----+ 1 usbmux plugdev`). The devnum changes on every
re-enumeration, so that path moves; the rule is keyed on the Apple VID, so it
follows. This covers every stage, verified unprivileged end to end on
2026-08-01. Older revisions of this document said the NCM stage needed root. It
did not, and the reason it looked that way is worth knowing: the failure it
produced was an `Operation not permitted` on adding the link-local, which
invites `sudo` when the actual cause was the TAP's MAC being pinned too late
(`CARPLAY_TAP_MAC`, in the bridge that is now deleted). Address configuration
was done in-process (`SIOCSIFADDR`/`SIOCSIFHWADDR`), not by shelling out; the
only remaining shell-out was a best-effort `nmcli` call to stop NetworkManager
touching the link.

### Running on macOS

Verified 2026-08-01 against iPhone `00008140…` (`05ac:12a8`), the same phone as
the Linux sessions. Stages 1 through 7 all run on macOS, through iAP2
authentication with the real MFi coprocessor to decoded H.264; one run streamed
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

macOS needs less code than Linux, not more, because it ships both of the things
we hand-rolled. `usb_pipeline.cpp` selects between them with
`CARPLAY_USE_SYSTEM_MUX` and `CARPLAY_USE_SYSTEM_NCM`, which default to the host
and can be overridden so either branch type-checks from either platform. Before
2026-08-01, `usb_pipeline.cpp` and `iap2_session.cpp` (about 1,500 lines) were
dropped from the macOS build purely because they sat in the same CMake
`if(Linux)` block as the bridge, although they had always compiled fine, so a
refactor could break the bring-up path and nobody would find out until the next
Linux build. There is no `CARPLAY_HAVE_APPLE_USB` any more, and nothing in
`apple_usb` is Linux-only. `muxd.cpp` (the usbmux state machine),
`usbmuxd_server.cpp`, `usbmux_client.cpp`, `lockdown_client.cpp`,
`tls_stream.cpp`, `pair_record.cpp` and `carkit_channel.cpp` were always plain
C++, POSIX sockets and OpenSSL, and are where the logic worth unit testing
lives; `usb_device.cpp` and `ncm_discovery.cpp` became portable with the libusb
port, so enumeration, descriptor parsing and transfers are real on macOS rather
than stubbed; and `ncm_bridge.cpp` and `ncm_frame.cpp` were the last platform
split until 2026-08-02. `usb_device_stub.cpp`, `APPLE_USB_NO_TRANSPORT`,
`ncm_bridge_null.cpp` and `APPLE_USB_NO_NCM` are all gone, so every file in the
library compiles and is tested everywhere.

Port 7000 is already taken on macOS. The system AirPlay Receiver, inside
`ControlCenter`, holds `*:7000`, so the receiver's wildcard bind fails with
`EADDRINUSE`. Binding the NCM link-local specifically succeeds alongside it
given `SO_REUSEADDR`, which the receiver already sets. Measured:

```
bind [::]                      reuse=1 -> Address already in use
bind fe80::1ca1:5cd0:be56:35c6 reuse=1 -> OK
```

So `ReceiverConfig::bind_address` is set to the link-local on macOS (it was
declared but unused until then, and is resolved with `getaddrinfo` so the `%en9`
scope comes with it). The phone only ever dials the address we advertised, so
nothing is lost by not holding the wildcard. Turning AirPlay Receiver off in
System Settings would also free the port, but is not required.

Why not fight the system daemon: taking If1 from macOS's usbmuxd would mean
capturing the whole device, and capture is all-or-nothing; it would also strip
the NCM interfaces from `AppleUSBNCM`, which is precisely what the NCM stage
wants to keep. Stopping the daemon is not an option either: `launchctl bootout
system/com.apple.usbmuxd` is refused (`150: Operation not permitted while System
Integrity Protection is engaged`). Using it is both the cheapest and the only
route that leaves the NCM stage intact.

Which interface is the AV link: `AppleUSBNCM` binds both NCM pairs and creates an
interface for each, and the iAP interface (If2) brings up an
`AppleUSBEthernetHost` interface too. Pick by MAC, from `iMACAddress` in the
CDC Ethernet functional descriptor, never by "the interface that just appeared".

```
Apple USB Multiplexor@1  +-o usbmuxd  <AppleUSBHostInterfaceUserClient>
AppleUSBEthernet@2       +-o AppleUSBEthernetHostAQM  +-o en7   <- not this
NCM Control@3 / Data@4   +-o AppleUSBNCMData          +-o en9   <- first pair
NCM Control@5 / Data@6   +-o AppleUSBNCMData          +-o en8   <- second pair
```

`en9`'s MAC `ca:1f:e8:0f:24:b1` shares an allocation with the phone's own
address, which is the same tell the Linux session used to pick the first pair.
Its link-local is a `secured` (RFC 7217) address, not the EUI-64 of the MAC,
same as Linux, and the reason both platforms advertise whatever the kernel
actually assigned rather than a derived address.

Why root, specifically, for the configuration switch: it has to take the phone
away from whatever already owns it, and the two platforms do that very
differently.

| | Linux | macOS |
|---|---|---|
| Granularity | one interface at a time | the whole device at once |
| Permission | udev rules are enough | root, or `com.apple.vm.device-access` |
| Re-enumerates? | no | no (see below) |
| Who is holding it | `ipheth`, `cdc_ncm`, an earlier client | macOS's own `usbmuxd`, always running |

`com.apple.vm.device-access` is a restricted entitlement Apple issues to
virtualization vendors, so root is the only route open to us. `usbprobe` reports
whether the current process has what it needs, and it answers with no phone
attached (`Device capture: UNAVAILABLE -- macOS only lets root take a USB device
away from its own drivers ... Re-run the node with sudo.`).

Three things about macOS capture that are easy to get wrong, all verified
against libusb's darwin backend rather than assumed. First, it does not
re-enumerate: `kUSBReEnumerateCaptureDeviceMask` seizes the device, and libusb
follows it with `darwin_restore_state()`, which reopens the IOKit objects behind
the same `libusb_device_handle` and restores the previous configuration. So the
handle survives, and the "must not re-enumerate" property the configuration
switch is built around holds on both platforms. (An earlier version of this note
claimed capture invalidated the handle. It does not.) Second, it is refcounted
on libusb's cached device, not on the handle, and `darwin_close()` never
releases it, so one capture at the switch covers the whole session; the mux, the
lockdown channel and the NCM lookup each open their own handle and none needs to
capture again. Third, never call `libusb_attach_kernel_driver()` or enable
`libusb_set_auto_detach_kernel_driver()`: either drops the refcount mid-session,
and macOS's `usbmuxd` will take the phone back immediately.

Three more caveats before blaming the code. The default state dir is
`<data dir>/carplay`, where the data dir is `REDLINE_DATA_DIR` or the per-user
location (`libs/core`, see [runtime environment](../reference/environment.html));
under `sudo`, `$HOME` may be `/var/root`, which moves it. On macOS this matters
less than it looks, since the pair record comes from the system usbmuxd, not
from our state dir. The vendor request `0x52` triggers a real bus
re-enumeration, unlike capture; that can drop the capture taken before it, so
the configuration switch takes it again, which is why the code captures on both
sides of the request. And string descriptors are padded: this phone reports its
24-character UDID in a 40-character field, space filled, and libusb returns all
40. That is invisible in a terminal and harmless while both ends of the usbmux
conversation are ours, since `UsbmuxdServer` echoes back the same padded string.
Against the system usbmuxd it fails as "the mux does not list udid=…", which
reads like a mux fault and is a string fault. `readSerial()` and
`readStringDescriptor()` trim; do not undo that.

`apple_usb_usbprobe` genuinely enumerates a phone on macOS, which makes the
descriptor check a thing you can run off the Linux box. `apple_usb_muxctl`
points our own usbmux client at any usbmuxd, ours or Apple's, and a second
argument checks `findDevice()`, the lookup the padding above breaks.

## usbmux and lockdown

The usbmux stage claims interface 1, runs the version/setup handshake, and
serves the standard usbmuxd protocol on a private socket, which is why the stock
libimobiledevice tools still work against it
(`USBMUXD_SOCKET_ADDRESS=UNIX:<our-socket> ideviceinfo`). Verified on hardware
2026-07-21: `idevice_id -l` returns the UDID through our socket and `[carkit]
carkit TLS channel up (iAP2)` appears about 113 ms after start, exercising the
whole chain of mux, plist framing, the `Connect` relay, lockdown pairing and TLS.

Triage for the mux: `could not claim mux interface` means another driver holds
it (`lsusb -t`, and the daemon notes above); `usb reader ended` immediately
means wrong endpoints for this device generation (confirm `0x85`/`0x04` against
`lsusb -v` for config 6); connect attempts timing out (`mux connect ... failed`)
means the SYN/ACK handshake is not completing, and an immediate RST usually
means the phone rejected the port (wrong lockdown port) rather than a framing
bug. For the socket: an empty `idevice_id -l` means our `UsbmuxdServer`
ListDevices reply is wrong, so check the plist packet header framing
(little-endian length/version/message/tag).

The phone must be unlocked, not merely trusted. These are different things and
only one of them prompts you. With the screen locked, lockdown returns `Password
protected (-17)` and every lockdown attempt fails while the USB and mux stages
look perfect. Tapping "Trust" does not clear it.

UDID form matters. libusbmuxd normalises a modern 24-character serial into the
25-character `XXXXXXXX-XXXXXXXXXXXXXXXX` form, and `idevice_new_with_options`
matched against that. The serial we read has no dash, so it is converted in
`openCarkitChannel` before the lookup. Verified differentially while
libimobiledevice was still present:

```
ideviceinfo -u 00008140000138EE0184801C   -> ERROR: Device ... not found!
ideviceinfo -u 00008140-000138EE0184801C  -> reaches lockdownd
```

Without that conversion the stage failed at `idevice_new` with a "device not
found" that looks like a mux bug but is a string-format bug.

Other lockdown triage: a handshake failing with a pairing error means tap
"Trust" on the phone and confirm pair records are being written under
`--state-dir`; `could not start com.apple.carkit.service` means the phone did
not expose the service, so confirm it is genuinely in config 6, since carkit
only exists there; a TLS enable failure is read from `[tls]` at `--verbose`,
which logs the negotiated version and cipher, and the client certificate comes
from the pair record's root key pair. `the phone rejected our pair record`
means the record is stale (phone reset, trust revoked), and the node re-pairs
automatically; no need to delete the state dir.

### How stage 4 replaced libimobiledevice

libimobiledevice was a vendored dependency until 2026-07-31 and was removed once
our implementation had replaced every part of it. Stage 4 is `UsbmuxClient` for
the transport, `LockdownClient` for the handshake, `TlsStream` for both TLS
sessions (the lockdown session and the carkit service connection), and
`PairRecord` for the identity, including minting one, so a device that has never
been trusted pairs on our code. This section is history rather than
instructions; it is kept because the two bugs below were found by diffing
against libimobiledevice's source, and because both are the kind that will be
reintroduced by anyone who assumes the obvious implementation is correct.

Verified on hardware 2026-07-31. Pairing from nothing: with the state dir
emptied, stage 4 reads the device public key, mints a root/host/device
certificate set, sends `Pair`, and stores the record the device's answer
completes. The result is byte-compatible with libimobiledevice's: same fields,
same sizes (root 948, host 964, device 1005, keys 1704, escrow bag 32), same
extensions, same `sha256WithRSAEncryption`. Interop both directions, while
libimobiledevice was still present to check against: it ran a full session to
`AuthenticationSucceeded` on a record we generated, without re-pairing, and we
did the same on a record it wrote; records written by either remain readable.
Stability: 25 consecutive `--max-stage 5` runs with no channel loss, plus a
150-second single session. Before the fix below it was 6 failures in 13.

The certificates carry empty subject and issuer names, which is Apple's design
and what libimobiledevice does too. `openssl verify` therefore reports
"self-signed certificate" for the host and device certificates; it cannot build
a chain by name. That is expected, identical for libimobiledevice's own records,
and not a defect; `X509_verify` against the root's key is the real check, and
`apple_usb_test_pair_record` does exactly that.

If stage 4 regresses and the cause is not obvious, libimobiledevice's source is
still the best reference: `idevice_connection_receive_timeout`,
`lockdownd_start_session` and `pair_record_generate_keys_and_certs`.

### The read-ordering bug, and why the reference's semantics matter

Two fixes took stage 4 from intermittently broken to solid. Both came out of
reading libimobiledevice's source rather than from the symptom.

`recv` must gather, not return the first chunk. This was the whole
intermittency: 6 failures in 13 runs before, 0 in 25 after. The phone would
reset the carkit connection about 0.9 s after the channel came up, on the first
session of a process. `idevice_connection_receive_timeout` loops `SSL_read`
until the caller's buffer is full, returning a partial buffer only once a read
times out, and the iAP2 link layer was written against those semantics.
Returning the first available chunk instead hands back as little as a dozen
bytes per call with a full link-layer poll cycle between calls, so a burst (the
post-authentication flurry, or a 60 KB album artwork frame) leaves the socket
backed up. Our own `UsbmuxdServer` relay then blocks writing into that socket,
which stalls the thread pumping the USB mux, which stalls every other stream on
it. Recorded because it was tested and rejected: this is not about ACK volume.
Runs that died sent 8 ACKs between channel-up and the reset; healthy runs send
11 over the same span. Fewer, not more.

`SSL_read` before `poll`, never after. OpenSSL buffers whole records, so once a
large message is split across reads the remaining plaintext is already decrypted
and held while the socket has nothing to report. Polling first waits out the
entire timeout before returning data it was already holding. The fix is
`SSL_read` first and `poll` only on `WANT_READ`, which needs a non-blocking
socket. This alone took the failure rate from 3/4 to 1/4.

Two more differences from the reference, both found the same way and both real.
A service dies with the session that started it: dropping the `LockdownClient`
once `StartService` returned killed the carkit channel about a second later,
every time. `NativeCarkitChannel` owns it for exactly that reason, and
`LockdownClient`'s destructor sends `StopSession` the way `lockdownd_client_free`
does. And no escrow bag on `StartService`: libimobiledevice's
`lockdownd_start_service` passes `send_escrow_bag=0`; ours originally sent one.
It did not turn out to affect stability, but matching the reference is correct.

## iAP2 and the MFi coprocessor

Verified on hardware 2026-07-21 up to the MFi handshake: the link layer and the
wired identification encoding are correct.

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
accessory v1, 12 file transfer v2). Two caveats are settled by those numbers:
`max_len` really is 65535 so the fragmentation off-by-ten is invisible, and the
phone advertises zero retransmissions/acks, confirming the wired path never
exercises the retransmission/EAK timers.

The phone requests the MFi certificate immediately after accepting
identification, before sending anything else. So `CarPlayAvailability`, and with
it the zero-length-boolean question below, cannot be reached until the
coprocessor works. `--iap2-allow-missing-mfi` runs everything up to that point
anyway, which is the right way to exercise the link while the board is out.

### Reaching the coprocessor

The MFi coprocessor is reached over I²C. On Linux the in-kernel `hid_mcp2221`
driver binds the MCP2221A and registers it as a standard I²C adapter, which
`i2c-dev` exposes as `/dev/i2c-N`; the driver talks to the coprocessor through
that. macOS has no such driver and drives the bridge over USB HID from userspace
instead. The backend follows the host platform; what is configurable is which
adapter the Linux backend opens, because a deployed board has several and
auto-detection only knows to prefer an MCP2221A. With no bridge present it takes
the first `/dev/i2c-N` it finds, which on the LattePanda is the GPU's DDC bus,
where every probe NACKs and the failure looks exactly like a dead coprocessor.
The environment variable `REDLINE_MFI_I2C_DEV` is the deployment's knob (the
image sets it in the `redline-node@carplay` drop-in, next to `REDLINE_DATA_DIR`);
the `--mfi-i2c-device` flag and `apple_mfi_demo`'s argument are for a bench.
`i2c-tools` is worth installing purely as an independent cross-check of the
driver: two separate implementations probing the same bus is the fastest way to
tell a wiring fault from a software one.

The bridge and the coprocessor are separate failure domains, and the MCP2221A
tells you which one is at fault if you read its status. `apple_mfi_demo`
narrates both:

| Symptom | Layer | Meaning |
|---|---|---|
| `MCP2221A device not found` | host | no `/dev/hidraw` node; `hid_mcp2221` is still bound |
| `I2C engine is in state 0x62, not idle; resetting` | bridge | expected once after an unclean exit; the driver self-heals |
| `I2C speed set to 100000 Hz` | bridge | bridge is fully healthy from here on |
| `state: 0x25` (`AddressNACKed`) | board | nothing is answering at that address |

`mcp2221a_i2c_scan` finding no devices while the bridge reports `SCL=1 SDA=1`
means the I²C lines are pulled up and free but the coprocessor is not
acknowledging: a board problem (power, wiring, or the MFi RESET pin held
asserted), not a software one. The library has no GPIO support, so if a breakout
wires MFi RESET to one of the bridge's GP0 to GP3 pins, nothing releases it and
every address will NACK.

### The coprocessor sleeps

The coprocessor sleeps, and the first access after it wakes is NACKed. This is
the single most misleading behaviour on this board. A bus scan that probes each
address once walks straight past it: the wake-up NACK at `0x11` is read as
"nothing here" and the scan moves on to `0x12`, never coming back. Two
consecutive `i2cdetect` runs show it clearly; the first finds nothing, the
second finds `0x11`. It also re-sleeps quickly. Measured on the LattePanda
(DesignWare controller at 100 kHz, 2026-09-13, with a probe that kept the bus
open and timed each transaction):

| | |
|---|---|
| sleeps after | 30 to 60 ms idle |
| first START after sleep | NACKed, or ACKed with a ~12 ms clock stretch |
| responsive again after a NACK | ~0.5 ms |
| after a successful register-select write | busy 0.8 to 1.5 ms, NACKs everything; the pointer stays set |
| after a NACKed write | pointer not set; a following read returns garbage |

The last two rows are what broke the first native-controller attempt: the read
came straight after the write, inside the busy window, and re-issuing the pair
on every failure reopened it, so the demo never read a byte even though
`i2cdetect` saw the part. Over the MCP2221A the USB round trip had hidden the
window. `AppleMFIIC::read_register` therefore retries a NACKed write (pointer
unset) but retries the read alone after a good write, 0.5 ms apart for up to
25 ms, before redoing the pair. Do not "simplify" that away, and do not put the
read back-to-back with the write.

The read retry is bounded by time, not by a count, because one failed read costs
about 0.2 ms on the native controller and about 12 ms over the hidapi bridge
(the engine latches on a NACK and has to be polled back to idle). A count tuned
for one transport is useless or a stall on the other; 25 ms is under the part's
idle-to-sleep threshold on both. The same logic serves both paths, since the
Linux controller and the MCP2221A are different `i2c::Bus` backends, and
`libs/apple_mfi_ic/tests/test_apple_mfi_ic.cpp` drives it against a fake
coprocessor with the measured timing on a native-shaped and a bridge-shaped
transport, checking device info, a full challenge, and the wall time of each.
One physical limit the fake makes explicit: a transport whose NACK recovery
takes longer than the part's ~30 ms sleep threshold can never wake it, because
the recovery itself is idle time. The hidapi backend's ~12 ms is inside that;
keep it there.

A single failed transfer at startup is therefore normal, not a fault. The
opening read fails and the retry succeeds a few milliseconds later.
`read_register` retries the write/read pair as a unit (8 attempts, 20 ms apart)
and only logs an error once all of them are gone. The MCP2221A layer reports
these at DEBUG: it signals failure through its return value, and only the caller
knows whether a failure is terminal. If `[error] I2C read from 0x11 failed ...
the client did not acknowledge its address` appears at ERROR level, that is a
regression in the logging level, not a bus problem; check whether `MFi
coprocessor ready` follows shortly after.

### The MCP2221A userspace driver

The userspace MCP2221A driver was broken until 2026-08-01, and it failed in a way
that looked exactly like a wiring fault. Every scan found zero devices and every
read to 0x11 returned `0x41` forever. "A full-bus scan finds nothing" is not the
hardware tell it appears to be. Three separate bugs, all in
`libs/mcp2221a/mcp2221a.cpp`:

1. Every transfer went to the general-call address 0x00. The report builders
   were explicit specialisations of a variadic `make_report<Cmd>()`, and a
   specialisation only matches when the deduced argument types match exactly.
   Call sites passed promoted `int`s (`make_report<I2CWriteData>(0, addr << 1)`
   deduces `<int, int>`, not `<uint16_t, uint8_t>`), so the specialisation was
   skipped, the primary template packed the arguments consecutively from byte 1,
   and the address landed in the length-MSB field while the address byte stayed
   zero. Nothing ever answered because nothing was ever addressed. Replaced with
   named builders (`makeI2cWrite` etc.) whose parameter types cannot silently
   change the overload.
2. The ACK test read the wrong thing. `ack_status` was the whole of response
   byte 20 compared against zero. DS20005565E table 3-2 says byte 20 carries the
   ACK status in bit 6 only ("if ACK was received from client value is 0, else
   1") with bit 7 and bits 5-0 explicitly "don't care", and they are not zero in
   practice. Measured: `0x00` when 0x11 answers, `0x40` when nothing does. Now
   masked.
3. A NACK latches the engine and the scan never unwound it. An unanswered
   address parks the state machine at `AddressNACKed` (0x25), and while it is
   parked every transfer command is refused with 0x01. `clear_i2c_engine()` now
   cancels back to idle before each probe.

Plus a timing detail: the write command being accepted only means the engine
took it, not that the address has been clocked out. Reading the ACK bit
immediately gives a stale answer, so the scan settles 2 ms first. With those
fixed, on the same hardware that had been "failing" all along:

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

Triage order, corrected: before suspecting the bus, prove the driver. A scan
that finds nothing at all is as likely to be an addressing bug as an electrical
one. Genuine bus faults show up as SCL or SDA stuck low, which the status
response reports directly in bytes 22 and 23; on a healthy idle bus both read 1.
Only once those look right do pull-ups, power, a swapped SDA/SCL pair and a held
reset line become the likely causes. The MCP2221A backend needs no privilege on
macOS, unlike the phone.

Two MCP2221A behaviours bit us on the userspace hidapi path used on macOS; the
kernel driver handles both itself, so they are invisible on Linux. A cancel
issued while the I²C engine is Idle wedges it: the engine drives a STOP that
never completes and latches `StopTimeout` (0x62), which refuses every later
parameter change and survives process exit. `MCP2221A::cancel()` therefore
returns early when already idle; do not "helpfully" remove that guard. And only
a device Reset clears a latched 0x62. Cancel does not; five consecutive cancels
were acknowledged and left the state unchanged. Reset costs a full USB
re-enumeration (measured ~6 s through VMware USB passthrough, and the hidraw
node path is recycled, so a handle opened too early lands on the dying node), so
`open()` resets only when it finds the engine non-idle.

### The zero-length boolean

If the phone reports CarPlay availability but the session never starts, check
this first. LIVI decodes a zero-length `bool` iAP2 parameter as `None`, which
makes `CarPlayAvailability.wired_available` falsy and silently skips sending
`CarPlayStartSession`. That behaviour was ported faithfully (returns `nullopt`),
but the spec arguably intends presence-as-value here. Not observed on the phone
tested during bring-up, which sends a proper one-byte boolean
(`wired=true available=1`). It is not worth a runtime switch, but it is worth
recognising: `runIap2Session` logs a loud warning naming the zero-length case
specifically, because the resulting failure is otherwise completely silent. If
that warning appears, changing `csm::getBool()` to treat a zero-length boolean
as `true` is the one-line fix.

### Other iAP2 caveats

Outbound fragmentation chunks at `max_len - 10` (header+checksum overhead),
where LIVI chunks at `max_len`. Invisible on the wired path (65535, small
messages) but it matters if a phone advertises a small `max_len`. Only the wired
carkit identification is implemented; Bluetooth/wireless transport components
are deliberately not encoded. Retransmission/EAK timers are a structural port
that has never run against a phone in either codebase; only the zero-ack wired
path is exercised in practice, so suspect them if the link is unstable under
load rather than at setup.

One caveat is fixed: the link layer used to treat an empty `recv()` as "no data
yet", never as EOF. `LibimobiledeviceCarkitChannel::recv()` returned an empty
vector for both a timeout and a hard error, and a failed `send()` was
discarded, so a dead link would have spun forever. `CarkitChannel` now exposes
`alive()`, which the transport adapter checks every poll.

Triage: no `[mfi]` certificate means the coprocessor is not reachable, so test
it standalone with `apple_mfi_demo` first. Identification rejected: the phone
lists which components it refused and the code logs them; usually a required
message is missing from the sent/received lists. Challenge signature rejected:
check the protocol major version (2 means SHA-1/20 bytes, 3 means SHA-256/32
bytes); signing the wrong digest length fails silently-ish. Link resets
repeatedly: checksum or sequence handling; `iap2_test_framing` should have
caught pure framing bugs, so suspect retransmission/EAK logic.

`iap2_test_framing` covers 20 groups and about 150 assertions without hardware:
byte-exact checksums, header round-trips, the full start sequence, SYN|ACK
negotiation, RST, corrupted-payload drop-and-retransmit, inbound reassembly
(1 CSM over 3 packets, 2 CSMs in 1 packet), outbound fragmentation against a
64-byte device, out-of-sequence hold/release, EAK emission and EAK-driven
retransmission, the CSM parameter codec including nested groups, identification
encode + rejection handling, route-guidance merge in both arrival orders,
call/power/cellular decode, the MFi authenticator on both protocol majors, and
an end-to-end identification+auth handshake over the link layer.

## The NCM link

### A lookup, not a datapath

There is no bridge here, and that is the whole design. When the phone is put
into the CarPlay configuration, the system's own NCM driver binds the first NCM
function and creates a normal ethernet interface for it: `AppleUSBNCM` on macOS,
`cdc_ncm` on Linux. The handshake needs exactly one thing from that interface,
an IPv6 link-local to put in `CarPlayStartSession`, which the phone then dials
on port 7000. So the stage reads the configuration descriptor and finds the
first NCM function, reads its `iMACAddress` string descriptor (the host MAC the
system driver gave its interface), finds the interface with that MAC, and reads
its link-local. Do not pick "the interface that just appeared": the iAP
interface brings one up too (`AppleUSBEthernetHost` on macOS, `ipheth` on Linux)
and it is not this one.

Verified on hardware. macOS 2026-08-01: `en9` / `ca:1f:e8:0f:24:b1` /
`fe80::1ca1:5cd0:be56:35c6`. Linux 2026-08-02: `enxca1fe80f24b1` /
`fe80::c81f:e8ff:fe0f:24b1`, 4713 frames over three minutes with no errors. No
privilege of any kind is needed.

The CarPlay configuration exposes two NCM pairs, and the system driver claims
the first as soon as the configuration is applied, which is the one we want: its
host MAC shares an allocation with the phone's own address (`ca:1f:e8:0f:…`
here) while the second pair's is unrelated. On Linux the second pair is left
unbound. `CARPLAY_NCM_CTRL_IF` pins the pair if you need to re-test that.

The one piece of setup is that the interface has to be addressed. The NCM
driver creates it but does not bring it up; macOS does that itself, and on Linux
it takes a one-line network profile for systemd-networkd or NetworkManager
(both shipped under `nodes/carplay/udev/`; the commands are on the node page).
Both say the same thing: link-local addressing only, no DHCP, EUI-64
generation. Without that profile NetworkManager actively breaks the stage, and
it does not look like a NetworkManager problem. It treats the phone's NCM
interface as an ordinary ethernet port and tries to get IPv4 configuration from
it. There is no DHCP server on the link, so activation fails, NM deactivates the
interface, and the IPv6 link-local it had already assigned goes away with it.
Then it retries, forever. The symptoms are a stream of link up/down
notifications and a bring-up that fails with `no IPv6 link-local address`,
except when it happens to run inside one of the connected windows, which makes
it look intermittent rather than systematic. `nmcli device status` showing
`connecting (getting IP configuration)` on the phone's interface is the tell.

### Duplicate address detection

Wait for duplicate address detection before binding. A fresh link-local spends
about a second `tentative` while DAD runs. `getifaddrs` reports a tentative
address exactly like a finished one, but `bind()` rejects it with
`EADDRNOTAVAIL`, so taking the first address you see races DAD, and the AirPlay
listener failed to start on a fresh plug perhaps half the time. The address
flags are the only way to tell, and on Linux they are visible only in
`/proc/net/if_inet6` (column 5), not through `getifaddrs`.

| flag | meaning |
|---|---|
| `0x40` | `IFA_F_TENTATIVE`: DAD still running, `bind()` will fail |
| `0x08` | `IFA_F_DADFAILED`: someone answered for it; it will never work |
| `0x80` | `IFA_F_PERMANENT`: usable |

`linkLocalOf()` skips anything tentative or DAD-failed, so the existing 200 ms
poll waits DAD out. Observed directly on 2026-08-02: `flags=c0`
(permanent|tentative) at T+1.0 s, `flags=80` at T+2.0 s, with the node picking
the address up at T+1.4 s only after it cleared.

Triage: no `enx*`/`en*` interface for the phone means the NCM function was never
bound, so confirm configuration 6 and that nothing captured the device away from
the system driver. `no NCM function in configuration 6` means descriptor
discovery found nothing; run `apple_usb_usbprobe` and compare against the
descriptor table above. An interface that `has carrier but no IPv6 link-local`
(or whose `carrier cannot be read`, which is an interface that is down) means
the network profile is not installed or not applied. `has NO CARRIER` is the
phone's doing, not ours; see Carrier, below. `bind(...)
failed: Cannot assign requested address` means a tentative address got through.
Ping works but no inbound TCP: check that `CarPlayStartSession` was sent with
the correct accessory `fe80::` address and port 7000. Nothing at all on the link
before a session is expected, not a fault: the phone only powers up its NCM
data path once a CarPlay session is actually running.

### Carrier

The link-local depends on the phone. `cdc_ncm` reports no carrier until the
phone sends a connection notification on the NCM interrupt endpoint, and the
kernel does not generate a link-local on a link without carrier. Until
2026-09-20 the NCM stage required the address within five seconds, before iAP2
had started, so a phone that raised carrier late, or only once it had identified
the accessory, never got the iAP2 session that would have made it do so. LIVI
does not have that ordering: it reads the address when the phone sends
`CarPlayAvailability`.

Now the stage needs only the interface. With no address it logs which of the
three causes it is (no carrier, carrier but no address, interface down) and
carries on. iAP2 starts, and when the phone reports CarPlay available
`StartSessionGate` holds `CarPlayStartSession`, asking for the address again on
every poll for up to 30 seconds; the session ends after that so the supervisor
retries. The AirPlay listener binds that one address and cannot use the wildcard
(macOS holds `*:7000`), so with no address it is built but not started, and is
started from the same callback, ahead of the send. A phone that raises carrier
unprompted, as the one verified on 2026-08-02 does, takes the old path unchanged.

Not verified on hardware: no phone here has been seen withholding carrier, so
the deferred path is unit-tested (`carplay_test_start_session_gate`) and
otherwise unexercised. What makes a phone raise carrier is still unknown;
neither this stack nor LIVI sends anything NCM-specific to provoke it.

When a transfer fails and the cause is not visible from the driver's own logs,
look at the bus with usbmon:

```bash
sudo modprobe usbmon
sudo setcap cap_net_raw,cap_net_admin+eip $(which tcpdump)
sudo chgrp plugdev /dev/usbmon* && sudo chmod g+r /dev/usbmon*   # not persistent
tcpdump -i usbmon2 -w /tmp/usb.pcap -s 256      # bus 2; match your phone's bus
```

The URB status is what matters. `ENOENT`/`ECONNRESET` on completion means we
cancelled it (our timeout fired and the device never responded); `EPIPE` means
the device stalled; `OK` on a sibling endpoint proves the device is servicing
the bus generally.

### What used to be here, and why it is gone

Until 2026-08-02 Linux ran its own NCM implementation: `NcmBridge` took the NCM
pair away from `cdc_ncm`, drove NTB16 framing in userspace over usbfs, and
bridged that to a TAP device created by a root `carplay-tap.service`. About 1900
lines, plus a systemd unit and a `/dev/net/tun` dependency.

It was inherited from the LIVI Python port, where a userspace bridge was the
only option available. It was never a considered choice against the kernel
driver, and the macOS port, which had to use `AppleUSBNCM` because nothing else
exists there, is what made that visible: the same lookup works on Linux, because
`cdc_ncm` does the same job. There used to be a note here explaining that macOS
could not have a bridge because it has no TAP device. That turned out to be the
wrong way round: macOS did not need one because `AppleUSBNCM` already builds the
link, and neither does Linux.

Measured before deleting it, same phone, same three-minute soak:

| path | frames | fps | errors |
|---|---|---|---|
| kernel `cdc_ncm` | 4713 | 25.24 | 0 |
| userspace TAP bridge | 4625 | 24.75 | 0 |

So the kernel path is not slower, needs no privilege, and deletes the TAP
service. Two classes of bug went with it, both of which had cost real debugging
time. The interrupt-endpoint drain: CDC devices announce link state on the
control interface's interrupt endpoint, and if the host never reads it the phone
stops servicing the bulk OUT endpoint entirely, so every write times out while
reads keep working. Diagnosing that took a usbmon capture and most of a day. The
kernel driver always keeps a URB queued there, so the failure cannot occur. And
the link-local generation race: addrconf derives the link-local when the
interface is brought up, from whatever MAC is set at that instant, and never
revises it. On a persistent TAP that raced the bridge setting the
phone-dictated MAC, non-deterministically, differently across boots on the same
machine. The system driver sets the MAC before the interface exists, so there
is nothing to race.

A known limitation of the old path, recorded in case it ever comes back: TX sent
one ethernet frame per NTB block with no aggregation, one bulk transfer per
frame. `cdc_ncm` aggregates properly, which is the likeliest reason the kernel
path measured marginally faster despite doing the same work.

## The AirPlay session

Verified on hardware 2026-07-21: the complete handshake runs and the phone
streams H.264. In order: `/pair-setup` M1 to M6, `/pair-verify` M1 to M4, the
encrypted control channel, `/auth-setup`, session `SETUP`, `GET /info`,
`RECORD`, `POST /command`, stream `SETUP` (type 110), then a video data
connection carrying the avcC config and encrypted frames.

```
[airplay] stream type 110 -> dataPort 35141 (connectionID 7411721103110128217)
[video]   screen stream connected
[video]   codec config: H.264 (32 bytes Annex-B)
[video]   FIRST FRAME decoded: 116 bytes Annex-B
```

`viewAreas` is what unblocked the stream. Before it, the phone accepted
everything through `RECORD` and then sent `TEARDOWN` about 1 ms later without
ever requesting a stream. The display entry must carry `viewAreas` (with a
nested `safeArea`) and `initialViewArea`; we were advertising `viewAreas` in the
session SETUP `enabledFeatures` while supplying none, which is worse than not
claiming it at all.

### The screen stream

A 128-byte header followed by a body whose length is the header's leading
little-endian `uint32`. `header[4]` is the opcode: 1 is the codec config (an
avcC atom, in the clear), 0 is a frame, ChaCha20-Poly1305 sealed with the entire
128-byte header as AAD and a counter nonce that advances only on frames. The key
is `HKDF-SHA512(pair-verify shared, "DataStream-Salt<streamConnectionID>",
"DataStream-Output-Encryption-Key", 32)`.

The first message on the stream is an empty config, and that is normal. The
phone opens with a well-formed opcode-1 header carrying a zero-length body
(`header 00 00 00 00 op=01 | 00 56 01 c5`, length 0, opcode 1). The real 33-byte
avcC follows about 60 ms later. This is not a framing desync and not a preamble
being misread; the length field really is zero, and the stream stays aligned
either way, which is why it is easy to misdiagnose. `configToAnnexB()` rejects
anything below 9 bytes up front (nothing shorter can be avcC or hvcC) and logs
it at debug. Before that guard existed it fell through to a speculative bare
`hvcC` parse, which logged `nalu: hvcC atom too short (0 bytes)` at error in a
session that only ever advertises H.264: an H.265 complaint about a codec nobody
sent, once per connection. If you see that line again, the guard has been
removed.

`streamConnectionID` is unsigned. It goes into that salt as a decimal string,
and roughly half of all sessions produce a value above `INT64_MAX`, which a
signed plist decode renders negative: a different salt, a different key, and
every frame failing to decrypt. Verified on hardware: `4663436911794014275`
worked, `-3498692594036096197` (really `14948051479673455419`) did not. Format
it as `uint64_t`.

### The event channel

The event channel is encrypted from the first byte with keys derived from the
pair-verify shared secret: `HKDF-SHA512(shared, "Events-Salt",
"Events-Write-Encryption-Key"|"Events-Read-Encryption-Key")`. Unlike the control
channel these are not swapped: the accessory writes with Events-Write and reads
with Events-Read. HID input (touch) is pushed over it as a `POST /command` with
an `hidSendReport` plist: `{type, uuid, hidReport}`, where `hidReport` is the
multitouch report matching the descriptor in `/info` (six bytes per contact:
`[index, down, x-lo, x-hi, y-lo, y-hi]`, pixel coordinates).

### Details worth not rediscovering

pair-setup is transient SRP with password `3939`. Username is `Pair-Setup`.
M5/M6 exchange long-term Ed25519 identities under
`Pair-Setup-Encrypt-Salt`/`-Info` with nonces `PS-Msg05`/`06`.

`A` is occasionally 383 bytes, not 384. Roughly one run in 256 the phone strips
a leading zero from its SRP public key. `srp::Server::verify()` re-pads from the
BIGNUM so this is handled, but a 456-byte M3 body instead of 457 is the tell if
a proof is ever rejected for no apparent reason.

After pair-verify M4 the control channel is encrypted and stays that way: 2-byte
little-endian length, ciphertext, 16-byte Poly1305 tag, the length doubling as
AAD, separate counter nonces per direction starting at zero. The accessory
sends with `Control-Read-Encryption-Key` and receives with
`Control-Write-Encryption-Key`; the naming is from the controller's point of
view. Get this wrong and the phone goes quiet, because our parser sits
waiting for an RTSP header that never comes.

The `/auth-setup` layout is not guessable and cost the most time. The request is
`<1 mode><32 device X25519 pk>`; the response is `<32 our pk><4 cert length
BE><cert><4 signature length BE><signature>`. The signature is over
`SHA-1(our_pk | their_pk)` signed by the coprocessor, then encrypted with
AES-128-CTR where the key is `SHA-1("AES-KEY" | shared)[0:16]` and the IV is
`SHA-1("AES-IV" | shared)[0:16]`. SHA-1, not SHA-512; that single mistake looks
identical to every other failure mode from outside.

The clock sync is mandatory. The session SETUP body carries the phone's
`timingPort`; we must bind our own UDP port, advertise it, and drive RTCP-style
type-210 requests at it (`libs/airplay/timing.cpp`). LIVI's comment is explicit
that the phone tears the session down without it. What the clock sync got wrong
on its first sample is under Hardware session, 2026-08-02.

The crypto primitives are proven; suspect labels and framing, not math.
`airplay_test_crypto` is 90 assertions against published vectors: SHA-1/256/512,
HKDF-SHA512 (RFC 5869 TC1, plus multi-block expansion and the real
`Pair-Setup-Encrypt` / `Control-Salt` labels), X25519 (RFC 7748 §5.2 and §6.1
both sides, plus low-order-key rejection), Ed25519 (RFC 8032 §7.1
key/sign/verify plus mangled-signature/message/key rejection),
ChaCha20-Poly1305 (RFC 8439 §2.8.2 plus AAD/tag/nonce tamper rejection),
AES-128-CTR (NIST SP 800-38A F.5.1), `nonce64`/`nonceLabel` byte layout, and a
full SRP-6a KAT (verifier `v`, server `B`, client `A`, session key `K`, and both
proofs `M1`/`M2`) with negative tests for `A = 0`, `A = N`, wrong password,
wrong username, and mangled proofs. So if pair-setup or pair-verify fails on
hardware, look at message framing, TLV ordering, and which bytes get fed to each
hash.

Triage: pair-setup failing means the TLV8 sequence, or an SRP username that is
not exactly `Pair-Setup`. pair-verify failing means X25519/Ed25519 key handling
or the HKDF labels (`Pair-Verify-*`, `Control-Salt`, `Events-Salt`). auth-setup
failing means an MFi signature over the wrong bytes; the iAP2 stage must pass
first. `/info` accepted but no SETUP means the phone rejected our advertised
capabilities (log the raw `/info` we sent and compare against a known-good
capture). Everything up to RECORD but no video means the event channel keying.

## Video

### How video reaches the screen

The decode thread converts each `AVFrame` with libswscale straight into one of
two reused `QImage`s (`Format_RGB32`, Qt's native raster format), then takes
`_frame_mutex` only long enough to flip a front/back index; no pixels are
copied to publish a frame. `paintEvent` holds that same lock across its
`drawImage`, which is what stops the decoder from overwriting a buffer mid-draw.

swscale converts and scales in one pass, to the widget's size, not the stream's.
The widget publishes its geometry into an atomic on resize and the decode thread
scales to it. This is deliberate: swscale has to walk every pixel for the colour
conversion regardless, so folding the resize in is close to free, whereas
leaving it to `drawImage(rect(), img)` costs a second full transform pass, on
the GUI thread, on every repaint, not once per decoded frame. With overlays
composited above the video that repaint independently of the frame rate, that
difference compounds. The steady-state paint is always a straight blit. Verified
with `--sim-width 640 --sim-height 480` against the 800x600 widget: the log
reads `video scaler ready: 640x480 yuv420p -> 800x600 RGB32` and the dumped
frame is 800x600. `SWS_POINT` is used when the sizes match (swscale's optimised
unscaled converter) and `SWS_BILINEAR` when a real resize is needed. The scaler
context is rebuilt only when source geometry, target geometry, pixel format or
colour range actually changes.

Because rendering goes through `QPainter` into the normal widget backing store,
ordinary Qt Z-ordering applies: sibling widgets can be `raise()`d over the video
and will be visible. A `QVideoWidget` was tried here and reverted (`4d143ae`)
for exactly this reason: its surface composited on top of any overlapping
sibling, even a raised one, so nothing could be layered above the video.

The widget's log narrates every stage of the video path, so a black area can be
read off it:

| Log line | Meaning |
|---|---|
| (nothing) | no `CarPlayVideo` messages arriving; check `inspect hz`, keys, zenoh |
| `video decoder ready (H.264)` | messages arrive, decoder opened |
| `dropped N frame(s) waiting for a keyframe/config` | arriving but no sync point yet; the driver must publish config or a keyframe periodically, not once |
| `video synced on parameter sets/keyframe` | sync achieved |
| `decoder rejected N packet(s)` | bitstream problem: bad Annex-B rewrite, or parameter sets fed as a standalone access unit |
| `cannot convert decoded frame to RGB` | decoded, but swscale could not build a converter for that pixel format / geometry |
| `first video frame decoded and rendered (WxH)` | the picture is live; if the screen is still black, suspect widget geometry/layout, not video |

### Three bugs between frames arriving and a picture on screen

All found running the real dashboard against a live phone (2026-07-22).

1. The phone sends exactly one keyframe. A static CarPlay screen produces one
   IDR at session start and then only P-frames (verified: 1 × NAL type 5, 100 ×
   type 1 in a capture). A dashboard that subscribes late never sees it. The
   driver asks the phone for a fresh keyframe via a `forceKeyFrame` command on
   the encrypted event channel (`Receiver::requestKeyframe`); the phone then
   re-sends parameter sets and an IDR, and any late subscriber syncs within a
   second. When it asks is the subject of the next section.
2. CarPlay decodes to `YUVJ420P` (pix_fmt 12), not `YUV420P` (0). The widget's
   converter rejected anything but format 0 and dropped every frame with
   `cannot convert decoded frame to RGB`. The two formats share layout and the
   converter's coefficients were already full-range, so the fix was to accept
   format 12 as well.
3. The driver published `VideoConfig` once and cached it silently. It must be
   published as its own message and re-published before every keyframe, since
   zenoh has no retained messages.

The design requirement this exposed: zenoh has no retained or latched messages,
so a one-shot `VideoConfig` leaves any subscriber that starts later, or
restarts, permanently black. The driver must republish the parameter sets before
every keyframe (the simulator does this; the real AirPlay path does too), and
the widget syncs on either config or a keyframe since Annex-B keyframes carry
SPS/PPS in band. Verified: a dashboard started 8 s into a running session syncs
within one GOP (about 2 s) and renders. The widget caches a config message and
prepends it to the next access unit rather than feeding it to the decoder alone;
parameter sets by themselves are not a decodable access unit and produce
`AVERROR_INVALIDDATA`.

Two further triage notes from the same session: frames stalling after a while
would be zenoh backpressure on large keyframes, to be measured before switching
to shared memory (measured below, and not the case); touch doing nothing means
verify `nodes/carplay/input` carries events (`inspect echo`), then check the
0..10000 to 0..1 rescale and the HID report.

### Keyframes are gated on somebody actually rendering (2026-08-02)

zenoh has no retained messages, so a renderer that joins mid-stream has nothing
to decode until the next keyframe, and on a static CarPlay screen (a menu, a
stationary map) the phone emits none of its own. The driver therefore asks for
one, and used to ask on a timer alone: roughly every 1.5 to 2 s, forever,
whether or not anything was subscribed. It is now driven by whether anything is
listening, using zenoh's publisher matching status on `nodes/carplay/video`.

| State | Behaviour |
|---|---|
| nothing subscribed | no keyframe requests at all |
| first subscriber arrives | one requested immediately |
| something subscribed | the 1.5 s staleness poll, as before |

The poll is still needed, and this is the reason. zenoh reports a boolean, "is
anything subscribed", so the notification fires on the first subscriber arriving
and the last one leaving, and not when a second joins alongside a first. Running
`inspect echo -k nodes/carplay/video` while the dashboard is already up produces
no event, so that renderer syncs via the poll. Verified against zenoh 1.9.0,
cross-process:

```
initial matching=false
subscriber A connects   -> event, matching=true
subscriber B connects   -> nothing
A drops (B remains)     -> nothing
B drops (last one)      -> event, matching=false
```

To watch it on a bench, run the driver, then attach and detach a subscriber with
`inspect echo -k nodes/carplay/video`. The driver logs `[node] video topic has
subscriber(s)` and then `[airplay] a renderer connected; requesting a keyframe
now`, and the matching pair when the subscriber goes away. If the first line
appears and the second does not, the receiver is not wired to the bridge; if
neither appears, the matching listener is not being declared (it is declared
lazily, on the first `setVideoSubscriberHandler` call).

Lifetime: the bridge outlives any one session, and a background matching
listener cannot be undeclared, so the listener is declared once and dispatches
through whatever handler is installed. `runAttachedSession` detaches it at
teardown alongside the mic, input and location handlers, for the same reason:
they capture session-scoped state that is about to be destroyed.

### Colour range

Channel order is swscale's problem, so a red/blue swap is no longer a failure
mode. What is worth checking is colour range: `renderFrameToBackBuffer()`
normalises the deprecated `YUVJ*` formats to their plain equivalents and drives
the range via `sws_setColorspaceDetails`, full range for `YUVJ420P` or
`color_range == AVCOL_RANGE_JPEG` and limited otherwise. Real CarPlay is
full-range; the `--simulate` x264 stream is limited-range. Getting this
backwards shows up as washed-out or over-contrasty video, not wrong hues.
`CARPLAY_DUMP_RENDER=/path.png` on the dashboard grabs the exact `QImage` the
widget blits, to check pixel values without a screenshot tool.

Historical note: the original hand-rolled converter wrote into a
`Format_RGB888` buffer and swapped red and blue for a while. Greens were
unaffected (the middle byte is always G) and the test pattern was white-on-grey,
so it survived until a real CarPlay frame; a blue Maps dot rendering red was the
giveaway. swscale replaced that loop.

### HEVC (2026-08-02)

HEVC is implemented and verified. It was only ever the advertisement: `nalu.cpp`
already rewrote hvcC as well as avcC and knew HEVC's different keyframe rule
(IRAP 16..23 rather than a single NAL type), the codec travels on every packet,
and the widget already picked `AV_CODEC_ID_HEVC` from it. Two keys turn it on
together, `hevcInfo` in `GET /info` and `"hevc"` in the SETUP
`enabledFeatures`, and sending one without the other leaves the phone on H.264
with nothing to explain why. `display.allow_hevc` in the config sets both. With
it on, the phone chose H.265 immediately and it rendered end to end:

```
[video]   codec config: H.265 (106 bytes Annex-B)
[video]   FIRST FRAME decoded: 331 bytes Annex-B
[carplay] CarPlay video decoder ready (HEVC)
[carplay] first video frame decoded and rendered (800x600)
```

It is an offer, not a demand: the phone chooses, and it takes the offer when
made. Shipped off, because H.264 is the path with every other hardware session
behind it and one HEVC session is not yet a basis for switching the default.

### The swscale warning

The swscale "no accelerated colorspace conversion" warning cannot be fixed, and
does not matter. Probed on this machine: every 32-bit destination format falls
back to the C path for `yuv420p` input.

| dst | bgra | rgba | argb | abgr | bgr0 | rgb0 | 0rgb | 0bgr | rgb24 | bgr24 |
|---|---|---|---|---|---|---|---|---|---|---|
| arm64 | C | C | C | C | C | C | C | C | C | C |

swscale's accelerated yuv2rgb converters are x86 SIMD; the aarch64 coverage does
not include them. So there is no destination format to switch to. Measured cost
of the C path:

| resolution | per frame | at 30 fps |
|---|---|---|
| 800x600 | 0.137 ms | 0.4% of one core |
| 1280x720 | 0.170 ms | 0.5% |
| 1920x720 | 0.214 ms | 0.6% |

The only way to avoid it entirely is to stop converting on the CPU, the
GPU/`QVideoWidget` path, which was implemented and then reverted because nothing
can be layered over that surface. Trading the z-ordering constraint for 0.4% of
a core is not a trade worth making.

What was worth fixing is that the message, and libavcodec's output generally,
went straight to stderr, untimestamped and unfilterable, in the middle of our
own logs. `helpers::routeFfmpegLogsToSpdlog()` now maps them into spdlog, with
ffmpeg's `AV_LOG_INFO` (where codecs put their per-run statistics) landing at
debug. A `--simulate` run used to carry a wall of raw `[libx264 @ 0x...]` lines;
it now carries none, and `--verbose` shows them as `[ffmpeg]` at debug.

### Shared memory

zenoh-c has `ZENOHC_BUILD_WITH_SHARED_MEMORY`, and it is off. Turning it on is
not a flag flip: the publisher must allocate its payload from an SHM provider
instead of a normal buffer, both ends must have the feature and be on the same
host, and it means rebuilding zenoh's Rust from scratch. The reason not to is
the payload. We publish compressed video:

| | |
|---|---|
| video payload | ~16 KB per frame, ~488 KB/s |
| a memcpy of that | ~0.0025% of one core |
| node CPU, real session | ~0.4% |
| raw RGBA at the same size and rate | 55 MB/s; this is what SHM is for |

SHM's benefit scales with payload size, and ours is three orders of magnitude
below where it starts to matter. Revisit only if something ever publishes raw
frames.

## Touch and the other inputs

### How touch reaches the phone, and why it is rate limited

Each touch report costs far more than it looks. `Receiver::sendTouch()` builds a
plist, `plist::encode()`s it, wraps it in an RTSP POST, runs it through
`encryptFrames()`, and writes it to the event channel socket. That channel is
shared with `requestKeyframe()`, the thing that recovers a black screen for a
late-joining renderer. Unthrottled touch on a 500 to 1000 Hz mouse can therefore
delay the keyframe request, which is a much worse failure than a slightly
coarser drag.

Two independent limits, at different altitudes. The widget paces itself to 60 Hz
(`kTouchPublishHz`), leading edge, so a drag starts responding immediately, with
coalescing and a trailing flush for everything inside the interval. The node
enforces a 125 Hz ceiling in `eventSendLoop()`; that is a guardrail, not a
second throttle. It sits well above the widget's rate so it never engages in
normal operation, and exists only to bound a publisher that ignores its own
limit.

60, not 30. The phone derives scroll momentum from the last few samples of a
gesture; at 30 Hz a quick flick only lands two or three, so fling velocity comes
out noisy. The symptom is taps and slow drags feeling fine while flicks feel
inconsistent, easy to misread as a phone-side problem. We also advertise
high-fidelity touch in `/info`, so 30 would undersell what we claim.

The two limits share only the spacing decision, as `helpers::RateGate`
(`libs/helpers/include/helpers/rate_gate.h`), a header-only "may I send at
`now`, and if not how long until I may". They are otherwise different animals
and are deliberately not unified: `airplay::EventQueue` is a bounded
multi-producer queue drained by a writer thread that limits every report
including down and up, while `TouchThrottle`
(`libs/dashboard_widgets/widgets/carplay/include/carplay/touch_throttle.h`) is
single threaded, holds at most one deferred position, and never delays a down or
an up. Folding the widget onto `EventQueue` would also mean the dashboard
linking the AirPlay stack to get a rate limiter.

Three invariants that a naive throttle breaks, all covered by
`carplay_test_touch_throttle` (exact, time injected) and again by
`carplay_test_touch_rate` (through a real widget, real wall clock). Down and up
are never rate limited; they are state transitions, not samples. A drag that
stops moving still reports where it came to rest; without the trailing flush the
last move is swallowed and no further events arrive, so the phone's idea of the
finger stays an interval behind indefinitely, and this is the one that bites.
Motion never coalesces across a down or an up; collapsing a down into a
following move relocates the press and turns a drag into a tap somewhere else,
which is why `sendTouch()` takes a `TouchPhase` rather than the old bare `down`
bool. The receiver could not otherwise tell a down from a move, since both set
the same bit on the wire.

### Nothing blocks on the socket

A single writer thread (`eventSendLoop()`) owns the event channel; `sendTouch()`
and `requestKeyframe()` enqueue and return, so the zenoh subscriber thread and
the keyframe thread can no longer stall on a congested `send()` or on each
other. Keyframe requests are held as an idempotent flag rather than queued, and
jump ahead of pending touch; they carry no ordering relationship to a gesture,
so there is nothing to gain by making them wait behind a drag.

The queue is what bounds a misbehaving publisher: consecutive moves coalesce
onto the tail, so flooding costs a memory write rather than an unbounded queue,
and what the phone eventually sees is where the finger actually is. Only down/up
can accumulate; past 64 queued reports they are dropped with a rate-limited
warning (`event channel backed up`), which only happens if the link itself has
stalled. The queue is also cleared when the event channel closes, so a gesture
orphaned by a disconnect cannot inject a phantom contact into the next session.

All of those rules live in `airplay::EventQueue` (`libs/airplay/event_queue.h`),
which deliberately holds no mutex, no clock and no socket; `Receiver` supplies
all three. `take(now)` is a pure decision given the queue state and an injected
time, so `airplay_test_event_queue` covers ordering, coalescing, keyframe
priority, the rate limit and the drop path with no threads and no hardware.
`eventSendLoop()` is left with only threading and I/O.

One thing that extraction turned up: the "last touch sent" timestamp used to be
left at its default, which is the clock epoch, making never sent
indistinguishable from sent at time zero, so the first report of a session was
rate limited against it. Real `steady_clock` values are far enough past the
epoch that this never showed up in practice. It now lives in `RateGate` as an
explicit flag, fixed once for both users, and both test suites assert at the
epoch precisely because that is the value that breaks.

### Testing the touch path without hardware

Three levels, none of which need a phone, the driver node, or the dashboard:

| Test | Scope | Cost |
|---|---|---|
| `carplay_test_touch_throttle` | widget throttle policy, time injected: interval boundaries to the nanosecond, deferral state machine, gesture transitions | instant |
| `airplay_test_event_queue` | node queue policy, time injected: ordering, coalescing, keyframe priority, drop path | instant |
| `carplay_test_touch_rate` | a real `CarPlayWidget` and a real zenoh subscriber driven with synthetic mouse events, headless | ~2.3 s of wall clock |

The first two are where behaviour is pinned; the third is what proves the policy
is actually wired to the timer and the publisher, which a pure unit test cannot
see. Keep it, but do not add cases to it that the unit tests could hold exactly;
its rate assertion has to use loose bounds because it measures real elapsed time
on a possibly-loaded machine.

### The non-touch input devices

Since 2026-08-02 the accessory advertises four HID devices in `/info` rather
than one: a touchscreen, a rotary controller (select/home/back, a pointer, a
detent wheel), consumer media keys, and a telephony keypad, all in
`libs/airplay/hid.cpp`. Siri is not HID; it is a `requestSiri` command on the
same event channel. This is what the `knob`, `mediaKey`, `telephony` and `siri`
kinds on `nodes/carplay/input` had always claimed to be for. They previously
fell through a `break` in `usb_pipeline.cpp` and went nowhere.

`schemas/carplay_input.capnp` documents what `code` and `value` mean per kind.
The media and telephony codes are the HID usage indices in the descriptors we
advertise, so they cannot be renumbered independently of `hid.h`. Nothing
publishes these events yet, since no widget has a knob or hard keys wired to it,
so on hardware this is exercised by publishing to the topic directly.

Two things make an input device fail silently, and both are what
`airplay_test_hid` checks. A descriptor that disagrees with the report: the
phone accepts the device, then discards every report whose length does not
match what the descriptor declared, with no diagnostic anywhere. The test parses
each descriptor's item stream, sums its Input item bits, and compares against
the report the code actually builds. And a uuid that does not match: the
device's `uuid` in `/info` and the `uuid` on the report are matched as strings,
so `2a2a2a2b` and `0x2A2A2A2B` are two different devices, one of which does not
exist.

Momentary presses are sent as press-then-release pairs, because the phone acts
on the transition: a media key that is never released is a key the phone stops
believing in. The knob's wheel and pointer are relative, so a turn is one report
and needs no release, but `sendKnob()` sends the all-clear anyway, since the
same report carries the button levels.

## Audio

Verified on hardware 2026-07-22: LPCM audio works. Playing music opened a
type-100 `media` stream at 44.1 kHz stereo, about 134 packets/s decrypted with
zero failures, published on zenoh and played through the sink.

```
[airplay] audio stream type 100 'media' -> 44100 Hz 2 ch, dataPort ...
[audio]   first packet on type 100 'media' (44100 Hz, 2 ch)
[carplay] audio sink started: 44100 Hz / 2 ch
```

Audio differs from video in four ways. Streams are on-demand: the phone opens an
audio stream only when there is something to play, an idle CarPlay screen
requests no audio stream at all, and nothing should be expected at RECORD.
Transport is UDP, not TCP: each audio SETUP asks for a `dataPort` and a
`controlPort` (both UDP), and the response must echo `streamConnectionID` or
the phone tears the stream down. The packet layout is `[12B RTP
header][ciphertext][16B tag][8B nonce LE]`, ChaCha20-Poly1305 with AAD = the RTP
header's timestamp+SSRC (bytes 4..12) and nonce = four zero bytes + the 8-byte
tail, with the same per-stream `DataStream-Salt<id>` /
`DataStream-Output-Encryption-Key` derivation as video. And PCM is 16-bit
big-endian on the wire and must be byte-swapped to S16LE for the sink.

### LPCM, and the AAC stream nobody opens

`/info` advertises PCM formats for stream types 100 and 101 (nav prompts, Siri,
calls, alerts, and PCM music), so those work with no codec dependency. Type 102
(buffered entertainment/music) is AAC-LC only in CarPlay. AAC-LC entertainment
audio is implemented (2026-07-23): `/info` advertises AAC-LC (0x400000) for type
102, and `libs/airplay/aac_decoder.cpp` decodes each raw access unit to S16 PCM
with libavcodec (no GStreamer or external process; libavcodec is already linked
for video). The decrypted RTP payload is a raw AAC-LC access unit (no ADTS), so
the decoder is configured with a 2-byte AudioSpecificConfig built from the
negotiated rate/channels. `airplay_test_aac` encodes a 440 Hz tone to AAC-LC and
round-trips it through the decoder at 44.1k and 48k, proving the ASC/extradata
and the float to S16 conversion. LIVI's `rtpAudioDecoder.ts` has the RTP
jitter-buffer pacing if it is ever needed.

Hardware finding (2026-07-23): this wired iPhone never uses the AAC stream. It
routes all music through type 100 as PCM, even when `/info` advertises AAC-LC
for type 102. This was probed by temporarily withdrawing the PCM `media` option
from type 100, leaving only type-102 AAC: the phone did not switch to AAC; it
declined to route audio to CarPlay at all and fell back to playing through its
own speaker. So AAC-LC (type 102) is a wireless-path codec in practice; the
wired path we drive uses PCM and it works cleanly. The decoder stays as a
verified-correct fallback for any phone that does send type 102, but it could
not be exercised end-to-end here, and type 102 stays advertised only as an
addition, never a replacement.

### Playback architecture

The widget plays through `QAudioSink` in pull mode: the network thread pushes
decrypted PCM into a thread-safe ring
(`libs/dashboard_widgets/widgets/carplay/audio_ring.*`) and the sink's own audio
thread pulls at the sample-clock rate, with a short priming cushion and
silence-fill on shortfall. This decouples the bursty network delivery from
steady playback and, unlike the earlier push-mode path, never silently drops
samples on a short write. `AIRPLAY_DUMP_AUDIO=/path.pcm` on the driver writes
the raw S16LE for `aplay -f S16_LE -r <rate> -c <ch>`, the definitive way to
isolate playback from data.

Choppy audio is usually the host, not this code. Verified 2026-07-22 on a
VMware guest: the LPCM data was clean (0 decrypt failures, 0 source-side gaps,
delivered at exactly 1.0× real time), yet playback stuttered, and so did a
YouTube video and a raw `aplay` of the dumped PCM. The tell in the ring stats is
zero underruns but steadily growing overruns: the audio device is draining
slower than real time, so the ring fills and drops the oldest samples. That is a
host problem; an emulated audio device (VMware HD Audio) under CPU contention
(load ~3.2 on 4 vCPUs) cannot sustain real-time playback. No amount of buffering
fixes a device that will not drain at 1×. Remedy at the VM/host level (more
vCPUs, a lighter load, host audio backend, larger PipeWire quantum), not here.
Genuinely choppy data would instead show `[audio] inter-packet gap` warnings
from the driver.

### Microphone uplink

Implemented; control path verified on hardware 2026-07-22. When the phone wants
mic audio (Siri, a call) its main-audio (type 100) SETUP carries a `dataPort` of
its own; that is the signal to send captured audio there. The receiver derives
the input key (same `DataStream-Salt<id>`, `-Input-Encryption-Key` rather than
`-Output-`), fires `MicStatusHandler`, which sets the session's `mic_active` so
the widget starts its `QAudioSource` and publishes captured PCM on
`nodes/carplay/mic`, and `feedMic()` frames that PCM (framesPerPacket, else
20 ms) and RTP+encrypts each frame to the phone, an exact mirror of the working
downlink (BE PCM, AAD = RTP timestamp+SSRC, `nonce64` counter,
`[hdr][ct+tag][nonce8]`).

Verified on hardware up to the audio: triggering Siri opened the uplink
(`[audio] mic uplink up: [fe80::…]:62672 44100 Hz 1 ch, 882 samples/frame`), the
widget started capture, and the framing/keying matched the downlink. End-to-end
voice was not confirmed on the VM: its microphone is near-silent (peak ~194) on
the same emulated audio stack that stutters playback, and the capture happens in
the dashboard, which runs on the Mac. Like audio playback (which the VM mangled
but the Mac plays cleanly), confirm Siri and calls on real host audio.

The OPUS codec is not implemented; it is wireless-only, and the wired path uses
PCM/AAC.

## Metadata over iAP2

### Now playing and album art

Now-playing is wired and verified (2026-07-22). Metadata comes over the iAP2
carkit channel, not AirPlay: after MFi auth succeeds the session sends
`StartNowPlayingUpdates`, then decodes each `NowPlayingUpdate` (0x5001) and
publishes to `nodes/carplay/nowplaying`. Updates are partial: a track change
carries title/artist/album/duration, a tick may carry only `elapsed`, so
`usb_pipeline.cpp` merges each update into a persistent state before publishing
and absent fields are not cleared. They are re-published every 2 s, because
zenoh has no retained messages and a dashboard that connects while a track is
paused (no fresh updates) would otherwise show nothing. Verified on hardware: a
paused Music track published `American Dream / Alabama Shakes / I Must Be
Dreaming`, merged from separate partial updates, with duration and elapsed for
the progress bar.

Album artwork is wired and verified (2026-07-22). After a track change the phone
pushes the cover image over the iAP2 file-transfer session (id 12),
automatically, with no per-track request. The receiver in `iap2_session.cpp`
handles the datagram protocol (`SETUP` acked with `START`, accumulate
`FIRST/DATA/LAST`, complete acked with `SUCCESS`) and hands the assembled JPEG
to the artwork handler, which folds it into the now-playing state and bumps
`album_art_seq`. The widget caches by that sequence and only re-decodes on
change, so the 2 s metadata republish does not thrash it. Verified: a
99,563-byte JPEG arrived intact and matched the track.

### Navigation and calls

Wired and verified on hardware 2026-07-23. Both follow the now-playing pattern:
after auth the session subscribes (`StartRouteGuidanceUpdates`,
`StartCallStateUpdates`) and routes decoded updates to `nodes/carplay/nav` and
`nodes/carplay/call`, merged and re-published every 2 s. Verified with a live
route and a real call:

```
[iap2] navigation: state=1 road '...' -> 'Wagyu Factory'
[node] nav publish: active=true dest='Wagyu Factory' toManeuver=19m remain=30337m eta_in=1860s
[iap2] call: active ('(714) 338-2330' / '7143382330')
[iap2] call: ended ('' / '')
```

Three field-mapping details that hardware settled. `nav.active` derives from
the route-guidance `state`; observed values are `0` = not routing, `1` =
actively guiding (destination present), `3` = transient (calculating), so
`active = state != 0` is correct and during navigation the phone holds
`state=1`. The `nav` distances are named the opposite of intuition in the iAP2
struct: `distance_remaining_m` is total-to-destination, `distance_to_maneuver_m`
is to the next turn (`usb_pipeline.cpp` maps them correctly). And
`current_road_name` is only sent when the phone knows the current road, when it
can place the car on a road from GPS/movement; on a stationary bench phone it is
often empty, and it populated as "Canyon Rd" when the route start resolved. Not
a bug: the field decodes correctly, the phone omits it.

Over iAP2 the phone sends turn-by-turn metadata (road name, next-maneuver type,
turn angle, distance-to-turn, distance/time remaining, ETA), already decoded and
published on `nodes/carplay/nav`. It does not send map imagery over iAP2; the
live map is inside the CarPlay video stream. So a richer nav experience is a
dashboard widget concern (a cluster-style turn-by-turn card rendering the `nav`
topic), not more protocol. There is no such widget yet.

### GPS location uplink

Implemented 2026-07-23, not yet hardware-tested. CarPlay lets the head unit feed
the phone the car's own GPS so the phone can dead-reckon where its signal is
weak (tunnels, garages). The phone requests it with `StartLocationInformation`
(0xFFFA), naming which NMEA families it wants (GGA/RMC/GSV/VTG as presence
flags); the session answers with `LocationInformation` messages carrying NMEA
sentences at about 1 Hz until `StopLocationInformation`. Sentence generation
lives in `libs/iap2/location_nmea.cpp` (GGA + RMC, which cover what the phone
needs; GSV/VTG are not generated), unit-tested by `iap2_test_nmea` for the
`ddmm.mmmm` coordinate encoding, hemispheres, all fields, and the XOR checksum
against a known fix. The fix source is a zenoh topic, `nodes/carplay/location`
(`CarPlayLocation`): any GPS source publishes fixes, the driver caches the
latest and uplinks it, mirroring how mic and input come from the dashboard side.
`--location "lat,lon[,altitude_m,speed_knots,course_deg]"` feeds a static fix
for a bench; starting turn-by-turn in Maps is what makes the phone ask.

## Session lifecycle, night mode and the phone's own commands

Three things the session layer owed the phone, added 2026-08-02. None is
hardware-verified.

TEARDOWN is now read, not only acknowledged. A TEARDOWN naming streams closes
those streams; one with no stream list ends the session. Both were previously
answered with a bare 200 and nothing else, so the dashboard was told the session
had ended only when the node shut down; a phone that unplugged left the widget
showing a live session forever. `endSession()` is the single place that reports
it, and is idempotent: a polite TEARDOWN and the control connection closing
behind it are the same session ending, and the dashboard should hear about it
once. It also drops the queued event-channel work, stops the mic uplink, and
clears the audio-stream registry.

`POST /feedback` now answers with the open audio streams (`{type, sampleRate}`
each) instead of an empty 200. An empty answer reads to the phone as "that
stream is gone", and it tears the stream down and re-opens it every few
seconds. What a full answer would add is a playback anchor, a timestamp and the
sample the sink is currently playing, which paces the phone to real time. We
have none: the PCM goes to the dashboard over zenoh and is played there, so this
side does not know where playback has reached. Inventing one would be worse than
omitting it. If a buffered stream (type 102) is ever exercised end to end and
drifts, this is the first thing to revisit.

Night mode switches CarPlay's own UI between its day and night themes. It is set
by `night_mode:` in the config, with no command-line override, because what the
accessory presents to the phone comes from one place. It is pushed at RECORD
(the phone ignores event commands sent before the session starts, and older iOS
stalls the bring-up about 5 s on one) and re-sent every session, because the
phone does not remember ours and assumes day. It is also reflected in
`CarPlaySessionState.nightMode`, a field that had existed since the schema was
written and was until then always false. There is no light sensor wired to it;
hooking it to the vehicle's headlight state is a `Receiver::setNightMode()` call
from whatever publishes that.

The phone's own commands are routed rather than logged as unhandled:

| Command | What we do |
|---|---|
| `requestUI` | manufacturer button, or an app naming a url; see The manufacturer button |
| `modesChanged` | tracks `speechMode` on appStateID 1, so Siri listening/speaking is logged on the transition; reads the main screen's owner (2026-09-16) and hands it to the screen handover |
| `duckAudio` / `unduckAudio` | logged with the computed linear level. Not acted on: this is the phone asking the head unit to attenuate its own sources, and there are none; the phone mixes its music and prompts before sending them to us |
| `suggestUI` | logged with the url count; the dashboard decides what it shows |
| `disableBluetooth` | logged. Not applicable on the wired path: iAP2 already runs over USB, so there is no Bluetooth link of ours to drop |
| anything else | acknowledged, and logged with its full body, which is how the next one gets identified |

A keepalive port is now advertised. `/info` had always claimed
`keepAliveLowPower`, and the SETUP response never gave the phone anywhere to
send them. A UDP socket is bound and its port returned when the phone's SETUP
asks for it. Nothing reads the datagrams; their arrival is the whole message.

## What the vehicle tells the phone about itself

The accessory's identity reaches the phone by two routes: iAP2 identification
during bring-up, and `GET /info` afterwards. Both are driven from one `vehicle:`
block in the config (2026-08-02). Before that, `/info` carried hard-coded strings
and iAP2 identification still used the defaults it was ported with, so a phone
paired with this stack recorded the accessory as `LIVI`, serial `0123456`. The
phone records some of this against the pairing, so change it before pairing a
phone you care about, or that phone remembers the old identity.

The keys worth setting and the closed-set validation are on the node page.
Deliberately not configurable are the protocol constants: feature bitfields,
audio format masks, `sourceVersion`, the resource arbitration table. Those are
negotiated behaviour rather than vehicle configuration, and a wrong value there
ends the session rather than looking wrong. A zero in the display geometry is
refused for a related reason: it would be advertised as a panel the phone cannot
draw on, and the session comes up and produces nothing.

`--config` is required, including for `--simulate`, which does not read it. One
rule is easier to remember than one rule with an exception, and everything the
accessory tells the phone about itself comes from that file rather than being
half config and half built-in default. The bring-up knobs (`--max-stage`,
`--state-dir`, `--location`, `--iap2-allow-missing-mfi`) stay on the command
line, because they are about taking one layer at a time rather than about what
the vehicle is.

## The manufacturer button

CarPlay draws one tile on its own home screen for the vehicle manufacturer. The
user presses it to hand the screen back to the head unit's native UI. Both
halves were implemented 2026-08-01 and fully verified on hardware 2026-08-02:
the tile, its label, its artwork, and the press.

`GET /info` carries `oemIconVisible`, `oemIconLabel` and an `oemIcons` array
(one entry per rendition: `imageData`, `widthPixels`, `heightPixels`,
`prerendered`). Built by `addOemButtonInfo()` in `libs/airplay/oem_button.cpp`
and unit-tested by `airplay_test_oem_button`. Those key names are not guesses;
they match LIVI's `getInfo.ts` exactly, which is a working implementation. It is
advertised once, at `/info` time. There is no way to show or hide the button
mid-session, so a config change needs a new session to take effect.

`prerendered` must be true, verified both ways on hardware 2026-08-02
(iPhone17,1, AirPlay 950.7.1). With `prerendered: false` the tile appears and is
correctly labelled, and the artwork is an empty square. The identical PNG with
`prerendered: true` renders correctly. The name misleads. It reads as "should
CarPlay apply its own corner mask and shine, as it does for app icons", so false
looks like the tasteful choice for a full-bleed square image. It is not: CarPlay
appears to decline to draw the icon at all. LIVI hard-codes true, which is why
LIVI's icons work; we defaulted to false and shipped an empty tile until a phone
said otherwise. Both `airplay::OemIcon::prerendered` and the config parser's
default for an absent key are now true, and `airplay_test_oem_button` asserts it
with this paragraph's reasoning attached, because the failure mode is invisible
in code review and a future reader would otherwise reasonably flip it back.

If artwork ever goes missing again, the encoding is not the place to look. It
was ruled out by dumping the exact `/info` we send and reading it with macOS's
own parser (`plutil -p /tmp/info.plist` shows `oemIcons -> imageData = {length =
819, bytes = 0x89504e47...}`). Apple's parser reading our plist correctly means
the plist library, the data encoding and the PNG bytes are all fine, and the
problem is in how CarPlay is being asked to treat the icon, which is how
`prerendered` was found.

The press comes back as `requestUI` on the encrypted event channel. The same
command carries an app asking the head unit to open a specific url, so the two
are told apart by whether `params.url` is present and non-empty; no url means
the button. `isOemButtonPress()` is the predicate;
`Receiver::handleEventCommand()` routes it to the `OemButtonHandler`, which
until 2026-09-16 only logged:

```
[airplay] manufacturer button pressed -- phone is asking for the vehicle's own UI
[node] manufacturer button pressed -- returning to the vehicle's UI is not wired up yet
```

Pressing it on the phone produces exactly those lines, so `requestUI` with no
url is confirmed as the wire form of the press, and `isOemButtonPress()`
recognises the real thing rather than only the synthetic one in its test.

Since 2026-09-16 the handler publishes `CarPlayUiEvent{kind: oemButton}` on
`<prefix>/ui_event`, and what happens next is the dashboard's business: a
`page_stack` trigger on that topic leaves the CarPlay page. The node does not
know page names. Not yet re-checked with a phone since the change; the decode
side is unchanged.

## Handing the screen to the car

Leaving the CarPlay page raises a second question: whether to tell the phone.
Real head units send `changeModes` taking the main screen when they show their
own UI, and give it back when CarPlay returns; the phone then routes
navigation prompts to the car's UI and can take the screen back itself for Siri
or a call. None of that had been sent by this stack, and LIVI never sends it.

What was built on 2026-09-16 is off by default (`screen_handover.enabled`).

The dashboard's CarPlay widget publishes `CarPlayVisibility` on change and at
1 Hz. Visibility rather than page names keeps the node ignorant of the
dashboard's layout, and it is a heartbeat because zenoh keeps no last value.
The video subscriber-presence edge was not used for this, because `scope` or
`bag record` subscribing to the video would hold it true.

`carplay::ScreenHandover` is a pure state machine over visibility, recording and
the reported owner. It takes the screen on hide, and gives it back with a
keyframe request on show. A reclaim by the phone while CarPlay is hidden is
published as `screenRequested`. A refused take is not published at all, because
bouncing the dashboard back to CarPlay would undo the driver's own tap. Three
seconds of visibility silence counts as visible.

`airplay/screen_modes.{h,cpp}` is the only place the message format lives.

The constants and what each rests on:

| Constant | Value | Evidence |
|---|---|---|
| main screen `resourceID` | 1 | our `/info` `modes.resources` lists 1 and 2, and LIVI's `modesChanged` logging names 2 as main audio |
| `transferType` take | 1 | already sent in `/info`, which the phone accepts |
| `transferType` untake | 2 | none; follows the numbering |
| `transferPriority`, constraints | 100 | the same values `/info` sends |
| owner key in `modesChanged` | `params.resources[].entity`, 1 = phone, 2 = car | one comment in LIVI's `cpStack.ts`; no captured body |

The hardware sequence that turns this on is on the
[carplay node](../nodes/carplay.html#screen-handover) page. Until a
`modesChanged` body has been captured and checked in, the parser's test
fixtures are synthetic and say so.

`oem_button.enabled` and `oem_button.label` in the config control it, with no
command-line overrides, so what a vehicle showed is answerable from the file
alone. The artwork is generated by `configs/carplay/make_oem_icon.py` (a
steering wheel, at 60/120/180 px); re-run that only if the icons change, since
the PNGs are committed. Icon dimensions are read from each file's PNG header, so
a config only names paths. Without artwork the button is still advertised with
the default label, and the node warns, because CarPlay then draws its own
placeholder, which looks enough like a working button to hide the mistake.

## Persistent pairing, and what it does not buy

The accessory used to generate a fresh Ed25519 identity on every run. It now
loads one from `<state_dir>/airplay_identity` (0600), and files each phone's
long-term public key in `<state_dir>/airplay_pairings`. Added 2026-08-02;
`libs/airplay/pairing_store.cpp`, tested by `airplay_test_pairing_store`. To
force a clean slate, delete the two files.

It does not stop the phone re-pairing, and that was the expectation going in.
Measured on hardware: with a stable identity and the phone's key on file, the
next session still ran a full pair-setup M1 to M6. The phone sends
`X-Apple-HKP: 0`, transient pairing, because wired CarPlay has no Bonjour
advertisement carrying our pairing id and public key, so it has nothing to
recognise us by before it connects. LIVI persists these for its wireless path,
where they do appear in the TXT records.

What it does buy, both confirmed on hardware: the identity stops changing on
every restart, which is correct in itself and a prerequisite for ever offering
wireless; and pair-verify M3 is now enforced. It used to be checked against the
key from the same session's pair-setup, which proves nothing about continuity,
so a mismatch was logged and ignored. It is now checked against the stored key
and a mismatch is refused; the log says `pair-verify M3 signature verified
against the stored key`. If a phone ever legitimately rotates its key it will
fail here until it redoes pair-setup, which files the new one. That is the
intended behaviour, and `airplay_test_pairing_session` covers both the returning
phone and the impostor.

## We were inviting the phone to try wireless CarPlay

The phone asked for our Wi-Fi configuration
(`RequestAccessoryWiFiConfigurationInformation`) on every single session, and we
declined every time. That is not the phone being speculative; it was answering
something we said. iAP2 identification declares two message lists, what the
accessory sends and what it can receive, and ours listed:

| Direction | Message | Meaning |
|---|---|---|
| we send | `AccessoryWiFiConfigurationInformation` | "I will hand over Wi-Fi credentials" |
| we receive | `RequestAccessoryWiFiConfigurationInformation` | "you may ask me for them" |
| we receive | `WirelessCarPlayUpdate` | "tell me about wireless availability" |

The first is the invitation. Claiming to send the credentials message is exactly
how an accessory says it can take part in a handover to wireless CarPlay, so the
phone dutifully opened that conversation on every connect. All three are now
behind `IdentificationConfig::advertise_wireless_carplay`, default false.
`DeviceTransportIdentifierNotification` deliberately stays in the received list
regardless: it also carries the phone's USB transport id, which is about the
link we are actually on. We never advertised a wireless or Bluetooth transport
component, only the USB one, so the message lists were the whole of it.
`/info`'s `bluetoothIDs` is unrelated: it is how the phone correlates the
accessory, and LIVI sends it too on the wired path.

Verified on hardware 2026-08-02. Both messages disappear from the session and
identification is unaffected:

| | before | after |
|---|---|---|
| `RequestAccessoryWiFiConfigurationInformation` | every session | none |
| `WirelessCarPlayUpdate` | every session | none |
| `identification ACCEPTED` | yes | yes |

The rest of the session is untouched: twenty distinct inbound messages, vehicle
status subscribed and answered, RECORD, video decoding. So the phone does not
require those declarations; it was taking us up on an offer. If a future iOS
does reject identification without them, set `advertise_wireless_carplay = true`
to restore the old behaviour.

## What LIVI has that we do not, and why

The stack was compared against LIVI's `src/main/services/projection/driver/cp/`
on 2026-08-02, feature by feature. Everything meaningful that was missing has
been closed (the session lifecycle work, the manufacturer button, and the
non-touch HID devices). What follows is what LIVI has and we deliberately do
not, so the next person to read its source does not re-derive the same
conclusions.

Not applicable to this stack: the iAP2-over-AirPlay tunnel (`iapTunnel.ts`,
stream type 130, and the `iAPSendMessage` relay) exists because the phone moves
iAP2 off Bluetooth after `disableBluetooth`, and LIVI's dongle path has no wired
iAP2 channel to fall back on; ours does, since iAP2 runs over USB on the carkit
channel the whole time, so we open and answer a type-130 SETUP so the phone does
not tear the session down, and interpret nothing on it. The Bluetooth stack
(`BluezDeviceClient`, `BtPairedRegistry`, `disableBluetooth` acting on a real
link): no Bluetooth here. The dongle protocol (`messages/sendable.ts`,
`DongleState`, the Carlinkit transport): we speak to the phone directly, though
the dongle's `SendIconConfig` is where the manufacturer-button key names came
from originally, since the dongle passes them into its own AirPlay server. And
Android Auto (`driver/aa/`) is out of scope.

Closed since the first pass (2026-08-02): persistent pairing (`identity.ts`,
`pairings.ts`), above. Vehicle status: we advertised a VehicleStatusComponent
declaring range and outside temperature, the phone subscribed with
`StartVehicleStatusUpdates` on every single session, and nothing ever answered,
despite `encodeVehicleStatusUpdate()` being written and unit-tested. It is now
driven from `vehicle.status` in the config, and the component is advertised
only when something is configured, because declaring a capability and then
ignoring the subscription is a promise broken every session. Values are static
for now; the shape is the one a live vehicle-state source would fill. And the
four inbound messages the phone sends that went nowhere:
`StartVehicleStatusUpdates` is answered; `WirelessCarPlayUpdate` and
`DeviceTransportIdentifierNotification` are decoded and logged (we had the
decoders and never called them); `RequestAccessoryWiFiConfigurationInformation`
is an explicit, logged decline, the first step of a handover to wireless CarPlay
that this accessory has no Wi-Fi to offer. The phone carries on over USB
regardless, which is what we want.

Applicable, deliberately not done: the instrument-cluster display (alt screen,
stream type 111, `showUI` / `stopUI` / `ALT_UUID`, `cluster-video-config`), a
second CarPlay surface for a digital gauge cluster. This dashboard drives one
screen. Adding it is a second `displayEntry` in `/info` plus a second screen
stream, no new protocol layer, so it is a day's work whenever a second panel
exists. A 48 kHz entertainment rate: LIVI picks 44.1 or 48 kHz for the type-102
stream; we advertise 44.1 only, and type 102 has never been exercised on the
wired path at all, so a second untested variant of it is not worth having.
`disableAudioOutput`: LIVI can mask the audio feature bits to advertise a head
unit with no audio, which is not a configuration this vehicle wants. A playback
anchor in `POST /feedback`, which we cannot produce honestly because playback
happens on the far side of zenoh. And `encodePowerUpdate` /
`encodeCommunicationsUpdate`, written and tested in `libs/iap2`, never called;
unlike vehicle status these are not advertised and the phone has never asked for
them across every session logged, so they are dead code for an unrequested
feature rather than a broken promise, left in place because the encoding is the
hard part and it is done. HEVC was on this list and is done and
hardware-verified (2026-08-02).

## Where the code lives

Restructured 2026-08-02. `libs/airplay/receiver.cpp` had grown to 2870 lines
holding five separable jobs behind four mutexes, and nothing inside it could be
tested without a socket. It is now 1324 lines of RTSP server (accept, frame,
dispatch, session lifecycle) and the rest are units that can be driven from a
test. Every concern that came out of the receiver gained a test, and everything
still inside it has none.

| Unit | What it owns | Test |
|---|---|---|
| `config.h` | what the accessory is, as the phone sees it | — |
| `info_plist.cpp` | the GET /info capability declaration | `airplay_test_info_plist` |
| `pairing_session.cpp` | pair-setup, pair-verify, auth-setup, accessory identity | `airplay_test_pairing_session` |
| `channel_crypto.cpp` | the framed ChaCha20-Poly1305 transport both encrypted channels use | `airplay_test_channel_crypto` |
| `event_channel.cpp` | the input/command connection, its queue and its two threads | `airplay_test_event_queue` (policy) |
| `hid.cpp` | the four input devices: descriptors, /info entries, reports | `airplay_test_hid` |
| `oem_button.cpp` | the manufacturer button, both directions | `airplay_test_oem_button` |
| `media_stream.cpp` | the screen and audio receive loops | — |
| `mic_uplink.cpp` | captured audio going back to the phone | `airplay_test_mic_uplink` |
| `net.cpp` | the two socket shapes (dual-stack, ephemeral port) | — |
| `nalu.cpp`, `crypto.cpp`, `srp.cpp`, `tlv8.cpp`, `aac_decoder.cpp` | as before | yes |
| `rtsp.cpp` | RTSP message framing | `airplay_test_rtsp` |
| `timing.cpp` | NTP clock sync; the arithmetic is split from the socket | `airplay_test_timing` |
| `receiver.cpp` | the RTSP server and session lifecycle that wires the above | — |

Two things are worth knowing before changing any of it. A second display is now
a data change in `info_plist.cpp` plus a second screen stream, rather than
surgery on the RTSP server; that was the point of extracting it. And night mode
is split on purpose: `EventChannel::setNightMode()` records it and
`pushNightMode()` sends it, because when to send is the session's business. The
phone ignores an event command sent before RECORD, and older iOS stalls the
bring-up for seconds on one; the channel cannot see that signal.

On the node side, `runAttachedSession` went from 645 lines to 146 by naming its
two largest stages, `startAirPlayReceiver` and `runIap2Stage`. The stages run
3, 4, 6, 7, 5, which is deliberate: the phone dials the AirPlay port within
milliseconds of the `CarPlayStartSession` that stage 5 sends, so 6 and 7 have to
be listening first. `UsbPipelineOptions` is gone; `NodeConfig` is the one config
struct, filled once in `main()`.

None of this restructuring was verified against a phone when it landed. It is
code motion checked by the build, the unit suites, and `--simulate`. If a
hardware session regresses after 2026-08-02 and the symptom is in the handshake,
the pairing and event-channel commits are where to look first. The same-day
hardware session below found one such regression.

The log prefixes, for isolating a layer with `grep`:

| Prefix | Layer | Source |
|---|---|---|
| `[usb]` | device detect, config-6 switch | `libs/apple_usb/usb_device.cpp` |
| `[muxd]` | usbmux TCP-over-USB | `libs/apple_usb/muxd.cpp` |
| `[usbmuxd]` | usbmuxd socket bridge | `libs/apple_usb/usbmuxd_server.cpp` |
| `[carkit]` | lockdown / TLS / carkit service | `libs/apple_usb/lockdown.cpp` |
| `[iap2]` | iAP2 link layer + control messages | `libs/iap2/` |
| `[mfi]` | MFi coprocessor auth | `libs/iap2/mcp2221a_mfi_signer.cpp` |
| `[ncm]` | NCM interface lookup | `nodes/carplay/usb_pipeline.cpp` (`AvLink`) |
| `[airplay]` | RTSP/AirPlay session | `libs/airplay/` |
| `[video]` / `[audio]` | media streams | `libs/airplay/` |
| `[node]` | zenoh publishing / orchestration | `nodes/carplay/` |

## Hardware session, 2026-08-02

The first hardware run after the large refactor and the feature-parity pass.
iPhone `00008140…` (iPhone17,1, AirPlay 950.7.1) and the real MFi coprocessor,
on macOS. Everything from USB detection through H.264 ran, and three bugs came
out that no amount of desk-checking had.

An event-channel feedback loop, introduced by the refactor and fixed. The event
channel is the only bidirectional one: the phone sends its own commands and
replies to ours. The inbound handling added with the manufacturer button only
considered requests, and a status line (`RTSP/1.0 200 OK`) has the same three
space-separated tokens as a request line, so `parseRequest` accepts it with
`method="RTSP/1.0"`. We answered its replies; it answered ours. Measured: about
1000 messages/second for the entire session, 27,082 error lines in 35 seconds, a
54,847-line log. Video kept flowing throughout, which is exactly why only
hardware found it. `rtsp::Message::isResponse()` now names the distinction; the
same run afterwards logged 672 lines and zero errors. LIVI's `cpStack.ts` has
this guard and it had not been ported.

The clock sync had never worked on the first sample, pre-existing and fixed. Our
clock counts from boot; the phone's timestamps are NTP, seconds since 1900. That
is about 126 years apart, past the ±2³¹ second window a signed 64-bit
fixed-point difference can represent, so the first offset wrapped and came back
with the wrong sign: a true gap of +3.99e9 s computed as −3.05e8 s. The 1/8 slew
then crawled toward correct over ninety-odd seconds, having stepped the wrong
way first, and `syncedNtp()` compounded it by casting a negative nanosecond
count to `uint64_t`. The first sample now adopts the phone's clock via
`ntp::toNanos()` instead of stepping by a difference that cannot express the
gap. On hardware: `clock adopted from the phone (offset 2207213529.836 s)`, 69.9
years, the NTP epoch offset, and zero large phase errors, down from 96.
`airplay_test_timing` had asserted this case was "not reachable in practice". It
is reached on every session. The test now says so.

`prerendered: false` renders an empty tile; see The manufacturer button.

Retracted: the node does not shut down slowly. An earlier version of this
section reported that it could take more than five seconds to exit on SIGTERM,
after three `carplay` processes survived a `pkill` and a five-second wait and
one kept holding the link-local `:7000`. Measured properly on 2026-08-02 with
the hardware back:

| Case | SIGTERM to exit |
|---|---|
| healthy session, streaming video | 1.19 s |
| failed bring-up, inside the retry backoff | 0.60 s |
| a user's own `^C` on a live session | 1.79 s |

and three start/`pkill`/restart cycles left zero stray processes. The original
claim rested on a check that could not have worked: macOS `pgrep` has no `-c`
flag, so `pgrep -c -f ... || echo 0` printed a usage error and then "0" from the
fallback. That "0" was read as "no processes left". The strays were almost
certainly accumulated by starting nodes in the background across several steps
without reliably killing the previous one: test hygiene, not the product. Two
things are worth keeping from it, since the symptom is real when it happens:

```bash
lsof -nP -iTCP:7000 -sTCP:LISTEN | grep carplay    # who holds the port
ps -eo pid=,comm= | awk '$2 ~ /\/carplay$/'         # exact count, no zsh wrappers
```

`ps aux | grep carplay` is not one of them: it also matches the shell whose
command line contains the path, which inflates the count and was the second
wrong number in the same investigation.

A note on method. Two of the three findings were the same shape: code that is
obviously correct in isolation, wrong against a real peer. The event-channel
loop needed a phone that replies; the clock needed a phone whose epoch is not
ours. Simulation cannot produce either, because `--simulate` has no peer. That
is the limit of the hardware-free test net, and worth remembering before the
next "this is desk-checkable" judgement.
