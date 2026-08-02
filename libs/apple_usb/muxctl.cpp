// SPDX-License-Identifier: GPL-3.0-or-later
//
// Drives UsbmuxClient against a running UsbmuxdServer socket, so the client can
// be exercised on hardware independently of the rest of the pipeline.
//
// This is the replacement for the `idevice_id -l` sanity check in
// docs/carplay_bringup.md stage 4. That check used libimobiledevice's tools to
// prove our *server* worked; this one uses our client, so a green run means both
// halves of the usbmux conversation are ours and agree.
//
//     ./build/nodes/carplay/carplay --max-stage 3 --verbose &
//     ./build/libs/apple_usb/apple_usb_muxctl ~/.local/share/carplay/usbmuxd-XXXXXXXX.sock
#include "apple_usb/usbmux_client.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <string>

namespace
{

// The lockdown port, the one thing every device listens on. Connecting to it
// exercises the Connect relay without needing the lockdown protocol itself.
constexpr uint16_t kLockdownPort = 62078;

}  // namespace

int main(int argc, char** argv)
{
    spdlog::set_level(spdlog::level::debug);
    spdlog::set_pattern("%v");

    if (argc < 2)
    {
        std::fprintf(stderr,
                     "usage: %s <usbmuxd-socket-path> [udid]\n\n"
                     "Lists what our usbmux client sees through a usbmux server -- ours on\n"
                     "Linux, or macOS's own at /var/run/usbmuxd. Given a udid, also checks\n"
                     "findDevice(), which is the lookup stage 4 depends on.\n",
                     argv[0]);
        return 2;
    }

    apple_usb::UsbmuxClient client(argv[1]);
    int failures = 0;

    if (const auto buid = client.readBuid(); buid)
    {
        std::printf("BUID: %s\n", buid->c_str());
    }
    else
    {
        std::printf("BUID: FAILED\n");
        ++failures;
    }

    const auto devices = client.listDevices();
    if (devices.empty())
    {
        std::printf("devices: none\n");
        ++failures;
    }
    for (const auto& device : devices)
    {
        std::printf("device: id=%u serial=%s type=%s pid=0x%04x\n", device.device_id,
                    device.serial.c_str(), device.connection_type.c_str(), device.product_id);

        if (const auto record = client.readPairRecord(device.serial); record)
        {
            std::printf("  pair record: %zu bytes\n", record->size());
        }
        else
        {
            std::printf("  pair record: none (the device has not been paired yet)\n");
        }

        // The Connect relay is the part that has to work for lockdown to run at
        // all, so prove it reaches a real port on the phone.
        if (auto conn = client.connect(device.device_id, kLockdownPort); conn)
        {
            std::printf("  connect to lockdown port %u: ok\n", kLockdownPort);
        }
        else
        {
            std::printf("  connect to lockdown port %u: FAILED\n", kLockdownPort);
            ++failures;
        }
    }

    // findDevice() is what openCarkitChannel actually calls, and it is a
    // different code path from listDevices(): it has to reconcile the UDID form
    // the device reports over USB (24 characters, no separator) with the form
    // the mux reports (25 characters, dashed). Getting that wrong looks exactly
    // like a mux failure, so check it with the real thing rather than assuming
    // that listing implies finding.
    if (argc > 2)
    {
        const std::string udid = argv[2];
        std::printf("\nfindDevice(\"%s\"):\n", udid.c_str());
        if (const auto found = client.findDevice(udid); found)
        {
            std::printf("  matched id=%u serial=%s\n", found->device_id, found->serial.c_str());
        }
        else
        {
            std::printf("  NOT FOUND -- this is what stage 4 fails on\n");
            ++failures;
        }
    }

    std::printf(failures == 0 ? "\nusbmux client OK\n" : "\nusbmux client FAILED (%d)\n", failures);
    return failures == 0 ? 0 : 1;
}
