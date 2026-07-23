# Wired CarPlay Bring-Up & Test Plan

This is the iterative test plan for the native wired CarPlay stack. The code was
written in bulk on a macOS dev box **without** an iPhone, MFi coprocessor, or Linux
host attached. This document is the script for stepping through the hardware
paths on a Linux host, in order.

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

**Privileges.** The driver needs raw USB access (usbfs) for the phone, an I²C
adapter node for the MFi coprocessor, and TUN for the NCM bridge. Running as root covers
all three. To run unprivileged instead, install the rules shipped in the repo —
this is a one-time step per machine:

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

This covers stages 2–5. **It is not sufficient for stage 6**: `NcmBridge::
configureInterface()` shells out to `ip link set`, and file capabilities set with
`setcap` on the driver binary are *not* inherited by child processes, so
`cap_net_admin+ep` on `carplay` leaves the `ip` calls failing with `Operation not
permitted`. Until those shell-outs are replaced with in-process netlink, stage 6
needs root.

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

**Conflicting daemons.** The system `usbmuxd` will fight us for the phone (this is
the core reason we run our own mux). It is not merely untidy: it holds
interface 1, the exact vendor-specific interface our mux claims
(`kMuxInterface = 1`). Stop it before testing:

```bash
sudo systemctl stop usbmuxd.socket usbmuxd.service
```

Some distros ship only the service unit and no socket unit; drop
`usbmuxd.socket` from the command if systemd reports it does not exist. If the
service comes back on its own, socket activation restarted it — `sudo systemctl
mask usbmuxd.socket usbmuxd.service` (reverse with `unmask`).

**Running in a VM.** USB passthrough works, but the stage 2 config switch
deliberately re-enumerates the phone, and hypervisors commonly hand a
re-enumerating device back to the *host* instead of the guest. If the phone
disappears and never returns within the code's 5 s window, suspect passthrough
before suspecting the driver — re-attach it to the guest and confirm with
`lsusb` that the VID is still visible from inside.

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
| `kMuxInterface = 1`, `kEpOut 0x04` / `kEpIn 0x85` | ✓ If1, vendor-specific 255/254/2 |
| `kNcmDataAltSetting = 1` | ✓ bulk endpoints live on alt 1 |

Note the vendor request is *sticky but not idempotent-looking*: before it the
phone advertises 5 configurations (config 5 is `…+ NCM`, which looks tempting
but is **not** the CarPlay config), after it 6. Do not "fix" the constant to 5.

**Applying the configuration needs the kernel drivers out of the way.** The
switch is done with `USBDEVFS_SETCONFIGURATION` on the usbfs node, which a udev
rule can grant to a normal user, rather than the root-only sysfs attribute. The
kernel returns **`EBUSY` while any interface is claimed**, so every bound driver
is released first with `USBDEVFS_DISCONNECT` (`ipheth` and an earlier usbfs
client both hold interfaces in config 4). Unlike the vendor request this does
**not** re-enumerate the device — config 6 is active in ~100 ms and, on a VM, the
passthrough binding survives.

**Triage.**
- Stuck at 4 configurations → the `0xC0/0x52` vendor request failed; check for
  `EPERM` (run as root) or that the phone is unlocked and trusted.
- `Failed to set configuration … not writable` → neither path worked: the usbfs
  ioctl failed *and* sysfs is root-only. Install the udev rules from stage 1.
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
- TLS enable fails → version skew in libimobiledevice; try a newer release.

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

**Triage.**
- No `cpusb0` → `/dev/net/tun` missing or no `CAP_NET_ADMIN`.
- Interface up but no ping → the kernel `cdc_ncm` driver may have claimed the
  interface; it must be unbound so we can drive it from userspace.
- Ping works but no inbound TCP → check that `CarPlayStartSession` was sent with
  the correct accessory fe80 address and port 7000.
- **Every bulk OUT times out while reads work** → the interrupt endpoint is not
  being drained; see above. Confirm with usbmon (below): the OUT URBs will show
  as submitted and then `ENOENT`/unlinked, meaning the device never serviced
  them at all, while OUT on the usbmux endpoint `0x04` completes normally.
- `"bulk endpoints not found"` → endpoint discovery reads sysfs `ep_*` right
  after `USBDEVFS_SETINTERFACE`. **Not racy in practice** — it resolved
  correctly on every run here. Altsetting 1 is hardcoded for the data interface
  (as in LIVI) and is correct for this phone.
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
**only on frames**. The key is
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
| `cannot convert decoded frame to RGB` | decoded, but not YUV420P — the converter only handles that format |
| `first video frame decoded and rendered (WxH)` | **the picture is live**; if the screen is still black, suspect widget geometry/layout, not video |

**Colours look wrong (red ↔ blue)?** `convertYuv420ToRgbImage` fills a
`QImage::Format_RGB888` buffer, whose byte order is R, G, B. Writing B into the
first byte swaps red and blue. Greens are unaffected (the middle byte is always
G), and the `--simulate` test pattern is white-on-grey, so this survived
undetected until a real CarPlay frame — a blue Maps dot rendering red is the
giveaway. `CARPLAY_DUMP_RENDER=/path.png` on the dashboard grabs the exact
rendered frame to check pixel values without a screenshot tool.

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

Then Siri/mic (two-way call audio), and finally the supplemental now-playing
widget rendering the same topic the `inspect` dump showed.

## What exists today (read before starting)

Not all stages below are implemented yet. Current state:

| Layer | State |
|---|---|
| USB detect, config-6 switch, usbmux, usbmuxd socket, lockdown (libimobiledevice) | **verified on hardware 2026-07-21** (stages 2–4) |
| iAP2 link layer, identification, MFi auth | **verified on hardware 2026-07-21** (stage 5 complete) |
| iAP2 metadata decode | written, unit-tested |
| NCM ↔ TAP bridge | **verified on hardware 2026-07-21** (stage 6) |
| AirPlay crypto/SRP/plist/NALU foundation | written, KAT-verified |
| **AirPlay RTSP session**: framing, pair-setup, pair-verify, encrypted channel, auth-setup, /info, SETUP, RECORD, clock sync | **written; handshake verified on hardware** |
| **AirPlay screen stream** (H.264 decode to Annex-B, published on zenoh) | **working, verified on hardware** |
| **Late-joining renderer sync** | working — periodic `forceKeyFrame` over the event channel |
| **Widget render (YUVJ420P)** | working — full CarPlay home screen renders |
| **Event channel + touch HID** | written; on-screen touch not yet confirmed |
| **AirPlay audio streams** | **NOT YET WRITTEN** |
| **Node orchestration**, stages 2–6 | done — `usb_pipeline.cpp` + `iap2_session.cpp`, driven by `--max-stage` |
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

Everything from USB up to the carkit TLS channel has now run against a phone.
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
