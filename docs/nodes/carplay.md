---
title: carplay
parent: Nodes
redirect_from: /carplay_bringup.html
---

# carplay

## Overview

`nodes/carplay` is the wired CarPlay driver. It moves an iPhone into its CarPlay
USB configuration, runs usbmux, lockdown and iAP2 over that link, authenticates
as an accessory through an MFi coprocessor, and then hosts the AirPlay session
the phone projects onto: H.264 or H.265 video, PCM audio, touch and the other
HID inputs, and the now-playing, navigation and call metadata that arrive over
iAP2. Everything the phone sends is published on zenoh and drawn by the
`carplay` widget in the dashboard (`libs/dashboard_widgets/widgets/carplay`);
touch, microphone audio and GPS fixes come back in on zenoh topics. The whole
Apple-side stack is in this tree and there is no libimobiledevice dependency, so
do not install `libimobiledevice-dev` or `libplist-dev` expecting them to be
used. The history of the port and what was tried is in
[CarPlay port](../design/carplay-port.html).

A real session needs a Linux host, an unlocked iPhone on a data cable, and an
MFi authentication coprocessor reachable over I²C: an MCP2221A USB bridge on a
desk, or the carrier's I²C header on the LattePanda. Since 2026-08-01 the full
session also runs on macOS by a different route through the same stack; see
[Building on macOS](#building-on-macos). Without any of that, `--simulate`
publishes a synthetic session on the real topics, so the dashboard side can be
run anywhere. The full pipeline has worked end to end since 2026-07-22, with
audio, touch, metadata and the manufacturer button verified on hardware since.
The stack is a port of LIVI (https://github.com/f-io/LIVI, GPL-3.0).

## Running it

Build prerequisites on Linux, then build and run the hardware-free tests before
touching a phone. A failure there is a logic bug, not a hardware problem.

```bash
sudo apt install libavcodec-dev libssl-dev iproute2
cmake --build build -j4                  # -j unbounded OOMs on an 8 GB box
ctest --test-dir build -L unit
```

The node needs raw USB access to the phone and an I²C adapter node for the
coprocessor. Running as root covers both; to run unprivileged, install the udev
rules and the module list once per machine. `udevadm trigger` re-applies the
rules to already-plugged devices, and you must be in `plugdev`.

```bash
sudo cp nodes/carplay/udev/99-carplay.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules && sudo udevadm trigger
sudo cp nodes/carplay/udev/carplay-i2c.conf /etc/modules-load.d/
sudo modprobe hid_mcp2221 && sudo modprobe i2c-dev
i2cdetect -l                              # expect "MCP2221 usb-i2c bridge"
i2cdetect -y 0                            # expect a device at 0x11 (maybe on the second run)
```

Mask the system `usbmuxd` (stopping it is not enough: its udev rule restarts it
when the configuration switch re-enumerates the phone), and give the phone's NCM
interface a link-local address with whichever profile matches who owns links on
the machine.

```bash
sudo systemctl mask usbmuxd.socket usbmuxd.service

# systemd-networkd (head unit, headless)
sudo cp nodes/carplay/udev/80-carplay-ncm.network /etc/systemd/network/
sudo systemctl restart systemd-networkd

# NetworkManager (most desktops)
sudo cp nodes/carplay/udev/carplay-ncm.nmconnection /etc/NetworkManager/system-connections/
sudo chown root:root /etc/NetworkManager/system-connections/carplay-ncm.nmconnection
sudo chmod 600 /etc/NetworkManager/system-connections/carplay-ncm.nmconnection
sudo nmcli connection reload
```

{: .warning }
The `chmod 600` is not optional: NetworkManager silently ignores a keyfile that
is group- or world-readable. Without the profile it treats the phone as an
ethernet port, fails to get IPv4, and takes the IPv6 link-local down with it.

Then plug in an unlocked, trusted iPhone and start the node and the dashboard.

```bash
./build/nodes/carplay/carplay --config configs/carplay/carplay.yaml --verbose
./build/apps/dashboard/dashboard -c configs/dashboard/carplay_demo.yaml
```

`--config` (short form `-c`) is required, including for `--simulate`, which does
not read it. Everything the accessory tells the phone about itself comes from
that file. The bring-up knobs stay on the command line, because they are about
taking one layer at a time rather than about what the vehicle is.

| Option | Meaning |
|---|---|
| `-c, --config <yaml>` | required; start from `configs/carplay/carplay.yaml` |
| `--max-stage <2..7>` | stop the pipeline after a bring-up stage, so one layer's failure is not buried under the next |
| `--iap2-allow-missing-mfi` | run iAP2 identification up to the certificate request with no coprocessor; CarPlay will not start |
| `--mfi-i2c-device /dev/i2c-N` | which I²C adapter holds the coprocessor; defaults to `$REDLINE_MFI_I2C_DEV`, else auto-detect |
| `--state-dir <dir>` | accessory identity and pair records; defaults to `<data dir>/carplay` (see [runtime environment](../reference/environment.html)) |
| `--location "lat,lon[,alt_m,speed_kn,course_deg]"` | a static GPS fix for the location uplink, instead of a publisher on the location topic |
| `--simulate`, `--sim-width`, `--sim-height`, `--sim-fps` | synthetic session, no phone |
| `-v, --verbose` | `SPDLOG_DEBUG` output, including ffmpeg's as `[ffmpeg]` |

{: .note }
Auto-detection of the I²C adapter only knows to prefer an MCP2221A. With no
bridge present it takes the first `/dev/i2c-N`, which on the LattePanda is the
GPU's DDC bus, and every probe there NACKs like a dead coprocessor. Name the bus:
the image sets `REDLINE_MFI_I2C_DEV=/dev/i2c-13` in the `redline-node@carplay`
drop-in; the flag is for a bench. Leave both unset on a desktop with the bridge.

`configs/carplay/carplay.yaml` documents every field: the `vehicle:` block,
`display:` geometry and `allow_hevc`, `device_id`, `night_mode`, the
`oem_button:` tile, and `screen_handover:`. The phone records some of the identity against the pairing,
so change it before pairing a phone you care about. Enumerated keys are closed
sets and a typo stops the node rather than taking a default; so does a zero in
the display geometry. The values worth setting rather than leaving:

| Key | Why |
|---|---|
| `vehicle.right_hand_drive` | CarPlay mirrors its own layout for it |
| `vehicle.engine_type` | gas, diesel, electric or cng; affects what the phone offers |
| `vehicle.serial_number` | how the phone tells two units apart |
| `display.physical_width_mm` | CarPlay sizes text and touch targets from it |
| `device_id` | give each unit its own if you run more than one |

## Running without hardware

The node can publish a synthetic session on the real topics: an encoded H.264
test pattern, a 440 Hz PCM tone, and rotating now-playing and navigation
metadata.

```bash
./build/nodes/carplay/carplay -c configs/carplay/carplay.yaml --simulate --verbose   # terminal 1
./build/apps/dashboard/dashboard -c configs/dashboard/carplay_demo.yaml              # terminal 2
```

You should see the moving test pattern with a sweeping white box, hear the tone,
and watch the now-playing widget cycle tracks. Touching the video area logs
input events in terminal 1. If something is broken in simulation, it is not a
CarPlay bug. `--sim-width/--sim-height/--sim-fps` change the stream; the rates:

```
inspect hz -k nodes/carplay/video       ->  30 msgs/s   (matches --sim-fps)
inspect hz -k nodes/carplay/audio       ->  50 msgs/s   (20 ms PCM chunks)
inspect hz -k nodes/carplay/nowplaying  ->   1 msgs/s
```

## Running the stack away from the MFi chip

The MFi authentication coprocessor is soldered to one board. Everything else in
the stack is portable, so a problem that wants a laptop's tooling -- a debugger,
a second phone, a `usbmon` capture -- is otherwise pinned to whichever machine
owns the chip. `mfi_proxy` serves the chip over HTTP so the node can run
somewhere else.

On the board with the chip. The node and the proxy cannot both have it: the
chip has no arbitration, and two processes interleaving transactions on
`/dev/i2c-13` will corrupt both. Stop the node first.

```bash
systemctl stop redline-node@carplay
mfi_proxy --bind 0.0.0.0 --mfi-i2c-device /dev/i2c-13 --token "$(cat /data/mfi-token)"
```

On the machine running the stack:

```bash
./build/nodes/carplay/carplay -c configs/carplay/carplay.yaml \
    --mfi-remote http://10.0.0.91:8099 --mfi-remote-token "$TOKEN"
```

`--mfi-remote` replaces `--mfi-i2c-device` entirely; the chip sits behind the
same three calls either way, which is what `iap2::MfiSigner` being an interface
buys. Success looks like one line before any phone is involved:

```
[mfi] remote coprocessor at http://10.0.0.91:8099 (protocol major 2)
```

That line is the reachability check -- the proxy is up, the chip answered, and
both ends agree on the routes. If it does not appear, the error above it is the
proxy's own response body, forwarded verbatim.

**This is a bench tool, and it is not in the image.** Signing is the step that
proves to a phone that a licensed Apple accessory is on the far end of the
cable, and serving it lets anyone who can reach the port borrow that proof.
Copy the binary across to use it. `/data` is the board's persistent partition,
so it survives an image update; `/lib64` does not exist there, because Yocto
puts the loader in `/lib` and a host-built binary asks for the other path:

```bash
scp build/libs/iap2/mfi_proxy root@10.0.0.91:/data/
ssh root@10.0.0.91 /lib/ld-linux-x86-64.so.2 /data/mfi_proxy --help
```

It binds to `127.0.0.1` by default, and an SSH tunnel (`ssh -L
8099:localhost:8099 root@10.0.0.91`) needs nothing more than that. Pass
`--bind`/`--token` only if you want it on the network, and stop it when you are
done.

Two gotchas, both of which cost time once already:

- cpp-httplib sets `SO_REUSEPORT`, so a **second** proxy on the same port
  starts silently and the kernel splits connections between the two. From the
  client that looks like a proxy intermittently ignoring its own `--token`.
  Check for a stale instance before believing anything stranger.
- The chip is slow -- a signature took 540 ms end to end over the LAN, nearly
  all of it the chip. The client's read timeout is 15 s for that reason.

To check the proxy without the node at all, `protocol` is a plain integer and
the other two are raw bytes:

```bash
curl -s http://10.0.0.91:8099/mfi/v1/protocol                    # -> 2
curl -s -o cert.der http://10.0.0.91:8099/mfi/v1/certificate
openssl pkcs7 -inform DER -in cert.der -print_certs -noout       # -> Apple iPod Accessories
head -c 20 /dev/urandom > chal.bin                               # 20 bytes for protocol 2
curl -s --data-binary @chal.bin -o sig.bin http://10.0.0.91:8099/mfi/v1/sign
```

A signature can be checked against the certificate it came with. Protocol 2
signs the 20 bytes you send *as* the SHA-1 digest, so recovering the RSA block
should hand back a PKCS#1 v1.5 DigestInfo ending in exactly those bytes:

```bash
openssl pkcs7 -inform DER -in cert.der -print_certs | openssl x509 -pubkey -noout > pub.pem
openssl pkeyutl -verifyrecover -pubin -inkey pub.pem -in sig.bin \
    -pkeyopt rsa_padding_mode:none | xxd | tail -2
```

## Bring-up checklist

Work the stages in order; each depends on the previous, and `--max-stage N`
stops after stage N. Every layer prefixes its log lines (`[usb]`, `[muxd]`,
`[usbmuxd]`, `[carkit]`, `[lockdown]`, `[tls]`, `[iap2]`, `[mfi]`, `[ncm]`,
`[airplay]`, `[video]`, `[audio]`, `[node]`), so `--verbose 2>&1 | grep
'\[muxd\]'` isolates one.

**1. Host.** The setup under Running it. Before touching the phone,
`./build/libs/apple_usb/apple_usb_usbprobe --drivers` shows which interfaces
already have something bound, and `./build/libs/apple_mfi_ic/apple_mfi_demo`
reads the coprocessor's certificate on its own (`Valid: Yes`).

**2. USB detection and the configuration switch** (`--max-stage 2`, grep
`[usb]`). Expect the phone at VID `05ac` with its UDID and port path, the
`0xC0/0x52` vendor request taking it from 5 advertised configurations to 6, and
`bConfigurationValue` becoming 6 while the port path stays constant.
`cat /sys/bus/usb/devices/<port>/bConfigurationValue` confirms it. Configuration
6 is sticky across unplugs.

**3. usbmux** (`--max-stage 3`, grep `[muxd]|[usbmuxd]`). Expect
`mux on interface 1 (class ff/fe/02), bulk in 0x85 / out 0x04`, the handshake
completing, and `[usbmuxd] serving <udid8> on /tmp/...sock` with the socket on
disk. Any stock libimobiledevice tool works against it:
`USBMUXD_SOCKET_ADDRESS=UNIX:/tmp/<our-socket> idevice_id -l` lists the UDID.

**4. Lockdown and the carkit TLS channel** (`--max-stage 4`, grep `[carkit]`).
Expect `[carkit] carkit TLS channel up (iAP2) udid=...` about 100 ms after
start. A phone that has never been trusted prompts on screen; the pair record
lands under `--state-dir`, and a phone that has revoked trust is re-paired
automatically.

**5. iAP2 and MFi** (`--max-stage 5`, grep `[iap2]|[mfi]`). Expect the SYN/ACK
negotiation, `identification ACCEPTED`, the coprocessor answering
`RequestAuthenticationCertificate` (908 bytes) and the challenge (128 bytes),
then `<- AuthenticationSucceeded`. The phone requests the certificate
immediately after accepting identification, so nothing past this point can be
reached without the coprocessor; `--iap2-allow-missing-mfi` runs up to it.

**6. NCM link** (`--max-stage 6`, grep `[ncm]`). Expect the first NCM function
pair chosen (`control iface 3 ... iMACAddress string 18`), then an interface
named after the phone's host MAC (`enx...`) up with a link-local, and the phone
answering `ping6 -c3 ff02::1%<iface>`. No privilege is needed. Nothing else
appears on the link until a session starts.

**7. AirPlay handshake** (`--max-stage 7`, grep `[airplay]`). Expect inbound TCP
on `[fe80::...]:7000`, `/pair-setup`, `/pair-verify` and `/auth-setup`
completing, `GET /info`, `SETUP`, `RECORD`, then `stream type 110`, `screen
stream connected`, `codec config: H.264` and `FIRST FRAME decoded`.

**8. Video and touch.** Start the dashboard. Expect the CarPlay home screen to
render and respond to taps and drags. Kill and restart the dashboard: video must
recover within a second or two, because the node asks the phone for a fresh
keyframe when a subscriber appears. Confirm the bus independently with
`inspect hz nodes/carplay/video` (30 to 60 Hz) and `inspect echo
nodes/carplay/input` while touching the widget.

**9. Audio.** Play music or start navigation; an idle screen requests no audio
stream. Expect `audio stream type 100 'media' -> 44100 Hz 2 ch` and sound from
the widget's sink. Triggering Siri opens the microphone uplink (`mic uplink
up: ...`) and the widget starts capture.

**10. Metadata.** `inspect echo nodes/carplay/nowplaying` while music plays,
`.../nav` during turn-by-turn, `.../call` during a call. Navigation also makes
the phone request location; with `--location` set, expect `[iap2] location
requested` followed by an NMEA uplink at about 1 Hz.

**11. Manufacturer tile and screen handover.** Tap the manufacturer tile on
CarPlay's home screen: expect `[node] ui event: manufacturer button` and, with
a [page_stack](../apps/dashboard/pages.html) trigger on `ui_event`, the
dashboard leaves CarPlay. The dashboard log says `hidden: video decode paused`
and `inspect echo nodes/carplay/visibility` reads `visible = false`. Then work
through [Screen handover](#screen-handover) below.

## Screen handover

When the dashboard hides CarPlay, the node can tell the phone the car has taken
the screen (`changeModes` take), give it back when CarPlay is shown again
(untake, a keyframe request, and optionally `requestUI`), and publish
`screenRequested` if the phone takes the screen back for Siri or a call while
CarPlay is hidden. The dashboard's CarPlay widget reports whether it is on screen
on `visibility` once a second; three seconds of silence counts as visible, so a
dashboard that exits hands the screen back.

```yaml
screen_handover:
  enabled: false
  request_ui_on_show: false
  visibility_stale_ms: 3000
```

{: .warning }
Off by default, and it stays off until a phone has accepted it. The `changeModes`
and car-to-phone `requestUI` messages have never been sent to a phone by this
stack, and LIVI never sends them either. A wrong resource constant can end the
session rather than look wrong. The message builders and what each constant is
based on are in `libs/airplay/screen_modes.cpp`.

With it off, the dashboard still pauses video decoding while CarPlay is hidden,
the phone keeps streaming, and the tile still works. The receiver logs who owns
the screen from every `modesChanged` either way (`[airplay] screen now owned by
...`).

The first session with it enabled, on a bench with a phone, in order:

1. With `--verbose`, capture one `modesChanged` body and check it carries
   `params.resources[]` with `resourceID` and `entity`, the screen as resource 1
   owned by entity 1. Check the capture holds no device identifiers, then commit
   it as the golden for `airplay_test_screen_modes`, whose fixtures are
   synthetic until then.
2. Tap the tile with music playing: `[airplay] changeModes: taking the screen`,
   then `screen now owned by the vehicle`. Music must carry on without a gap, and
   the session must survive. If it ends, set `enabled: false` and stop here.
3. Trigger Siri from the vehicle page: expect `screen now owned by the phone`,
   `[node] ui event: phone took the screen back`, and the dashboard back on
   CarPlay showing Siri.
4. Take a call on the vehicle page and check the same.
5. Return to CarPlay with the page button: untake, then video within a second
   and touch working at once. If CarPlay's UI does not come forward, try
   `request_ui_on_show: true`.
6. Quit the dashboard while CarPlay is hidden: within about three seconds the
   node logs `no visibility from the dashboard` and gives the screen back.

## What it publishes

Every key is under `--key-prefix`, default `nodes/carplay`. The schemas are in
`schemas/carplay_*.capnp`.

| Key | Schema | Direction | Notes |
|---|---|---|---|
| `video` | `CarPlayVideo` | out | Annex-B access units, H.264 or H.265; parameter sets are re-sent before every keyframe |
| `audio` | `CarPlayAudio` | out | S16LE PCM, 20 ms chunks in simulation |
| `session` | `CarPlaySessionState` | out | device connected, bring-up phase, screen size, night mode, mic state; re-published every second, recording or not |
| `nowplaying` | `CarPlayNowPlaying` | out | merged partial updates, album art by sequence; re-published every 2 s |
| `nav` | `CarPlayNav` | out | turn-by-turn metadata, re-published every 2 s |
| `call` | `CarPlayCall` | out | call state, re-published every 2 s |
| `ui_event` | `CarPlayUiEvent` | out | one message per occurrence: the manufacturer tile, the phone taking or returning the screen, an app asking for the head unit's UI |
| `input` | `CarPlayInput` | in | `touch`, `knob`, `mediaKey`, `telephony`, `siri`; `code` and `value` per kind are documented in the schema |
| `mic` | `CarPlayAudio` | in | captured PCM while the phone has asked for the uplink |
| `location` | `CarPlayLocation` | in | GPS fixes for the NMEA uplink |
| `visibility` | `CarPlayVisibility` | in | whether the dashboard's CarPlay widget is on screen, once a second; drives [screen handover](#screen-handover) |

Nothing publishes `knob`, `mediaKey` or `telephony` yet; on hardware they are
exercised by publishing to the topic directly.

## Health

`nodes/carplay/health` ([NodeHealth](../libs/node_health.html)), once a second and
at once on any change:

| Check | Not ok when |
|---|---|
| `usb` | USB bring-up did not complete (fault) |
| `session` | never: it reports whether a session is recording or idle |

`inspect health` prints them.

## Troubleshooting

Most failures are silent, or look like a different layer's fault. In stage order:

| Symptom | What it is |
|---|---|
| Stuck at 5 configurations, `EPERM` | the vendor request needs root or the udev rules, and an unlocked, trusted phone |
| `Failed to set configuration`, `EBUSY` | something holds an interface: the system `usbmuxd` (mask it), or `gvfsd-gphoto2` auto-mounting the PTP interface, which `systemctl` does not show; find it with `sudo fuser -v /dev/bus/usb/BBB/DDD` and kill it |
| Phone vanishes after the switch and never returns | expected briefly (5 s window); if it stays gone, a charge-only cable, or a VM handing the re-enumerated device back to the host |
| Stages 2 and 3 fine, stage 4 fails with `Password protected (-17)` | the phone's screen is locked. Trust does not clear it; enter the passcode and keep it awake |
| `the phone rejected our pair record` | trust was revoked or the phone reset; the node re-pairs on its own |
| No `[mfi]` certificate, every I²C probe NACKs | wrong bus (see the `REDLINE_MFI_I2C_DEV` note above), or `hid_mcp2221` not loaded. `apple_mfi_demo` and `i2cdetect -y N` are two independent implementations of the same probe |
| One failed I²C transfer at startup, at debug; `i2cdetect` finds nothing the first run and `0x11` the second | normal: the coprocessor sleeps after 30 to 60 ms idle and NACKs the access that wakes it. `read_register` retries; at error level it is a logging regression, so check that `MFi coprocessor ready` follows |
| `[iap2]` warning about a zero-length boolean, and no session | this phone did not do it, but a zero-length `CarPlayAvailability` decodes as absent and the session is never requested; the one-line fix is in `csm::getBool()` |
| No `enx*` interface | the NCM function was never bound; confirm configuration 6 and that nothing captured the device |
| `has carrier but no IPv6 link-local`, or `its carrier cannot be read`; link flapping | the network profile is missing or unapplied; `nmcli device status` showing `connecting (getting IP configuration)` is the tell |
| `has NO CARRIER`, then `CarPlayStartSession is held` | the phone has not brought its end of the NCM link up. Not fatal: iAP2 carries on and the session starts when the address appears. If it ends in `never got an address` after 30 s, the phone never raised carrier at all; the attempt is retried |
| Black video, no widget log | no `CarPlayVideo` arriving; check `inspect hz`, the keys in the dashboard config, zenoh |
| `dropped N frame(s) waiting for a keyframe/config` persisting | the node is not requesting keyframes; expect `[node] video topic has subscriber(s)` then `a renderer connected; requesting a keyframe now` |
| `first video frame decoded and rendered` but the screen is black | the picture is live; suspect widget geometry, not video. `CARPLAY_DUMP_RENDER=/path.png` on the dashboard saves the exact image blitted |
| Choppy audio, zero underruns, growing overruns | the host audio device drains slower than real time (an emulated device under load). `AIRPLAY_DUMP_AUDIO=/path.pcm` on the node gives raw S16LE for `aplay`; if that stutters too, it is the host |
| Manufacturer tile shows an empty square | an icon with `prerendered: false`; the default is true, keep it |
| Tapping the manufacturer tile does nothing on the dashboard | the node publishes `ui_event` and nothing more; the dashboard needs a `page_stack` trigger on it. `inspect echo nodes/carplay/ui_event` shows whether the tap arrived |
| Session ends right after leaving CarPlay | `screen_handover.enabled: true` with a message the phone rejects; set it false and record what the log showed |
| Something holds port 7000 after a restart | `lsof -nP -iTCP:7000 -sTCP:LISTEN \| grep carplay`; count processes with `ps -eo pid=,comm= \| awk '$2 ~ /\/carplay$/'`, not `ps aux \| grep` |

The per-stage triage, and how to read a usbmon capture when the node's own log
does not say why a transfer failed, are in
[CarPlay port](../design/carplay-port.html).

## Building on macOS

The whole node builds and links on macOS, and since 2026-08-01 stages 1 through
7 run there against a real phone and coprocessor. macOS supplies two things
Linux needs our code for, so `usb_pipeline.cpp` selects with
`CARPLAY_USE_SYSTEM_MUX` and `CARPLAY_USE_SYSTEM_NCM`, defaulting to the host.

| Stage | Linux | macOS |
|---|---|---|
| 2, configuration switch | usbfs and udev rules | whole-device capture, root |
| 3, mux | our `MuxHost` drives interface 1 | the system usbmuxd already does |
| 4, usbmuxd socket | our `UsbmuxdServer` on a private path | `/var/run/usbmuxd` |
| 4, pair record | we mint one; the phone prompts for trust | already exists; no prompt |
| 6, NCM link | `cdc_ncm` and a network profile | `AppleUSBNCM`, addressed by the system |
| 7 onward | portable | portable |

Root is needed exactly once, for the configuration switch, because macOS only
lets root take a USB device away from its own drivers. Configuration 6 is sticky,
so every later run is unprivileged, and the MCP2221A needs no privilege either.

```bash
sudo ./build/nodes/carplay/carplay -c configs/carplay/carplay.yaml --max-stage 2 --verbose  # once
./build/nodes/carplay/carplay -c configs/carplay/carplay.yaml --verbose                     # thereafter
```

{: .warning }
Do not stop the system usbmuxd on macOS. It is the mux we use, SIP refuses
`launchctl bootout` anyway, and taking interface 1 from it would need
whole-device capture, which also strips the NCM interfaces stage 6 depends on.

The system AirPlay Receiver holds `*:7000`, so the node binds the NCM
link-local specifically rather than the wildcard; the phone only ever dials the
address we advertised. Under `sudo`, `$HOME` may be `/var/root`, which moves the
default state dir; pass `--state-dir` for one shared location.
`apple_usb_usbprobe` reports whether this process can capture, and
`apple_usb_muxctl <socket> <udid>` points our usbmux client at any daemon, ours
or Apple's; neither needs phone-side setup.
