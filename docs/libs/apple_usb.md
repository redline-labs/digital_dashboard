---
title: apple_usb
parent: Libraries
---

# apple_usb

## Overview

The wired path to an iPhone, from the USB descriptors up to an authenticated
TLS channel carrying iAP2 bytes: enumeration and the CarPlay configuration
switch over libusb, a userspace usbmux multiplexer over the phone's bulk
endpoints, a usbmuxd-compatible unix socket server on top of it, the client
half of that same protocol, the lockdown handshake with its pair record and
client-certificate TLS, and CDC-NCM descriptor discovery. It replaced
libimobiledevice and libusbmuxd, which the LIVI stack this was ported from
delegated to; both ends of the usbmux conversation are now in this library.

Every source here builds on every platform, which is what lets the whole
library be built and tested on a developer's macOS box. The protocol and
service layer is plain C++ and POSIX sockets, and the USB transport is libusb,
which is portable. There used to be a platform split for an NCM bridge that
drove NTB16 framing over usbfs into a TAP device; it is gone, because the
system NCM driver (AppleUSBNCM on macOS, cdc_ncm on Linux) already builds that
link when the CarPlay configuration is applied, so the node looks the
interface up instead of rebuilding it. All that survives is descriptor
discovery, which is how the right interface is identified.

What this library deliberately is not: iAP2. `CarkitChannel` hands raw
link-layer bytes to [iap2](iap2.html) and knows nothing about them. The node
that drives the stages in order is [carplay](../nodes/carplay.html); the
reasoning behind the port is in [carplay-port](../design/carplay-port.html).

## Public headers

| Header | |
| --- | --- |
| `apple_usb/usb_device.h` | libusb behind a clean interface: `PortPath`, `DeviceInfo`, the portable descriptor model, `DeviceHandle`, enumeration, `switchToCarPlayConfiguration`, driver detach, and the transfer wrappers. `<libusb.h>` is not included. |
| `apple_usb/ncm_discovery.h` | `NcmFunction` and `findNcmFunctions`: every CDC-NCM function in a configuration, endpoints resolved from the descriptor before any altsetting is selected. |
| `apple_usb/muxd.h` | `MuxHost`, the userspace usbmux multiplexer (magic `0xFEEDFACE`) over bulk endpoints, and `MuxTcpConn`, one TCP-over-USB stream. |
| `apple_usb/usbmuxd_server.h` | `UsbmuxdServer`: exposes a `MuxHost` on a unix socket speaking the standard usbmuxd plist protocol, with pair records and the BUID under a state directory. |
| `apple_usb/usbmuxd_framing.h` | The 16-byte little-endian header plus XML plist framing, bounds-checked, and `alternateUdidForm`. |
| `apple_usb/usbmux_client.h` | `UsbmuxClient`, the side libusbmuxd used to occupy, and `MuxConnection`, the byte stream a successful Connect turns the socket into. |
| `apple_usb/byte_stream.h` | `ByteStream`: the stream lockdown is written against, so the protocol code is identical before and after the switch to TLS. |
| `apple_usb/tls_stream.h` | `TlsStream`: client-certificate TLS over an existing `ByteStream`, presenting the pair record's root identity and verifying nothing. |
| `apple_usb/pair_record.h` | `PairRecord`: parse either plist format, encode binary, and `generate` a fresh three-certificate identity. |
| `apple_usb/lockdown_client.h` | `LockdownClient` and `LockdownError`: QueryType, GetValue, Pair, StartSession, StartService. |
| `apple_usb/lockdown.h` | `CarkitChannel` and `openCarkitChannel`, the one call that runs the whole handshake and returns the iAP2 channel. |

## Using it

Link the CMake target `apple_usb`; it carries `plist` publicly and links
libusb and OpenSSL privately. The node's pipeline in
`nodes/carplay/usb_pipeline.cpp` is the reference caller: enumerate, switch
configuration, bring up the mux and serve usbmuxd, open the carkit channel.

```cpp
auto devices = apple_usb::listAppleDevices();          // opens nothing
if (!apple_usb::switchToCarPlayConfiguration(devices[0])) { /* ... */ }
auto device = apple_usb::findDeviceAt(devices[0].port); // address changed

auto mux = std::make_unique<apple_usb::MuxHost>(*device);
mux->open();
apple_usb::UsbmuxdServer server(*mux, socket_path, state_dir);
server.start();

std::unique_ptr<apple_usb::CarkitChannel> carkit = apple_usb::openCarkitChannel(
    udid, socket_path, state_dir, [&] { return stopping.load(); });
// carkit->send / carkit->recv carry raw iAP2 link-layer bytes
```

On macOS the system usbmuxd at `/var/run/usbmuxd` stands in for the `MuxHost`
and `UsbmuxdServer` pair; the node selects that with `CARPLAY_USE_SYSTEM_MUX`,
which defaults to the host and can be overridden so either branch type-checks
from either platform. Two tools build alongside the library: a read-only
enumeration and descriptor dump, the first thing to run when hardware shows
up, and a client that talks to a running usbmuxd socket, ours or the system's.

```bash
./build/libs/apple_usb/apple_usb_usbprobe [--serial]
./build/libs/apple_usb/apple_usb_muxctl ~/.local/share/carplay/usbmuxd-XXXXXXXX.sock [udid]
```

## Behaviour worth knowing

A phone is tracked by `PortPath`, not serial number. The Apple vendor request
that unlocks the CarPlay configurations re-enumerates the device, changing its
address and its usbfs node but not which socket it is plugged into, so
`switchToCarPlayConfiguration` waits for the device to come back at the same
port before selecting configuration `6`, and the caller re-reads `DeviceInfo`
afterwards. Enumeration leaves `serial` empty on purpose; `readSerial` is an
endpoint-0 control transfer that works while kernel drivers hold the
interfaces.

{: .warning }
Everything from the configuration switch onwards takes the device away from
the drivers that own it. Linux gets that from the udev rules under
`nodes/carplay/udev/`; macOS grants it only to root, so the node runs under
`sudo` there. `canDetachDevices` is a cheap preflight that says so in words
rather than as a bare `LIBUSB_ERROR_ACCESS` several layers down.

Driver detach is explicit rather than libusb's auto-detach, which only fires
on claim; the configuration switch needs interfaces released without claiming
them. After taking an endpoint over from a kernel driver, `usbClearHalt`
resets its data toggle, or the device NAKs every write as a duplicate and
bulk writes time out while reads keep working. Transfers throw
`std::system_error` with libusb codes mapped onto errno, so `ETIMEDOUT` stays
distinct from `ENODEV`.

An iPhone in the CarPlay configuration exposes two CDC-NCM functions, and
which carries the AV link is not self-evident, so `findNcmFunctions` returns
all of them in interface order. The host MAC comes from the `iMACAddress`
string descriptor; on macOS, where the kernel hands over a ready-made network
interface, that is the only reliable way to match an interface to its
function.

`MuxHost` runs a reader thread and `alive()` goes false once it stops, which
is how an unplug is noticed. `UsbmuxdServer` runs an accept thread and one per
client, and speaks the standard protocol so stock tools work against the
socket (`USBMUXD_SOCKET_ADDRESS=UNIX:<path> ideviceinfo`). The framing's
length field is the one number an unprivileged local client fully controls
that becomes an allocation size, hence its own header and a `1 MiB` ceiling.
Modern iPhones report a 24-character UDID whose dashed and undashed spellings
libusbmuxd and lockdown disagree on; a pair record filed under one is
invisible under the other, and `findDevice` matches both. This cost a real
hardware session.

`ByteStream::recvSome` keeps timeout (`0`) and closed (`-1`) distinct, since a
poller would otherwise spin forever on a dead link. `CarkitChannel::recv`
returns empty for both and carries `alive()` for the same reason.

Lockdown speaks XML but may answer in either format, so replies are sniffed
with `plist::looksBinary`. Pair records on disk are binary, and the parser
accepts either, including records libimobiledevice wrote. The TLS client
certificate is the record's root pair, not the host pair, and neither TLS
session verifies the peer: the device's certificate is signed by an authority
that exists only inside the pair record, so trust comes from the pairing
having happened.

`openCarkitChannel` pairs a device with no record, re-pairs one the device
rejected, and reuses a record otherwise. A first pair blocks on "Trust This
Computer?", which has no sensible deadline, so it waits indefinitely and polls
`abort` about once a second; `PasswordProtected` means the screen is locked
and tapping Trust will not clear it. The returned channel also owns the
`LockdownClient` that started the service, and that is load-bearing: dropping
the client resets the carkit port about a second later, right after
`AuthenticationSucceeded`.

## Tests

```bash
ctest --test-dir build -L apple_usb
```

Four targets, all labelled `apple_usb` and `unit`, all driving the portable
half against mocks, so they run on macOS with no phone and no usbfs.
`apple_usb_test_usbmux_client` runs the client against a mock server speaking
the wire format: ReadBUID, ListDevices, both UDID spellings, pair record read
and save, a Connect relay with echo, timeout versus closed, and a refused
Connect; `apple_usb_muxctl` is its hardware counterpart.
`apple_usb_test_ncm_discovery` runs discovery against synthetic descriptors
encoding what a phone is expected to look like, written with no hardware to
check against; if `apple_usb_usbprobe` disagrees with `carPlayLikeConfig()`
on a real phone, this file is what needs correcting.
`apple_usb_test_pair_record` checks the generated identity up to the wire
(root self-signed and CA, host and device signed by root, the on-disk round
trip, a libimobiledevice record) and cannot say whether an iPhone accepts it.
`apple_usb_test_usbmuxd_framing` feeds the header parser lengths below the
header and far above any real message, and pins the UDID conversion.
