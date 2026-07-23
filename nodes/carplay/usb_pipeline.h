// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wired USB bring-up pipeline: device detection, the CarPlay configuration
// switch, the usbmux TCP-over-USB host, the usbmuxd-compatible socket, and the
// lockdown/carkit TLS channel.
//
// The stage numbering matches docs/carplay_bringup.md so a bring-up session can
// stop at whichever layer is under investigation.
#ifndef CARPLAY_USB_PIPELINE_H_
#define CARPLAY_USB_PIPELINE_H_

#include "zenoh_bridge.h"

#include <atomic>
#include <string>

namespace carplay
{

struct UsbPipelineOptions
{
    // Highest docs/carplay_bringup.md stage to attempt (2..7). Lower values stop
    // early, which keeps a failure at one layer from being masked by the noise
    // of the next one failing as a consequence.
    int max_stage = 7;

    // Where pair records and the accessory identity live. Empty selects a
    // default under the user's runtime directory.
    std::string state_dir;

    // Stage 5 knobs; see Iap2SessionOptions.
    bool allow_missing_mfi = false;

    // Set to true while an AirPlay session is recording, so the caller's idle
    // session-state publisher stands down. Optional.
    std::atomic<bool>* recording = nullptr;
};

// Runs the pipeline up to options.max_stage, logging each stage with the
// prefixes docs/carplay_bringup.md greps for. Blocks until `stop` is set once
// the requested stages have come up. Returns true if every attempted stage
// succeeded.
bool runUsbPipeline(const UsbPipelineOptions& options, ZenohBridge& bridge,
                    std::atomic<bool>& stop);

}  // namespace carplay

#endif  // CARPLAY_USB_PIPELINE_H_
