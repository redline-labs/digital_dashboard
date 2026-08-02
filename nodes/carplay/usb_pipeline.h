// SPDX-License-Identifier: GPL-3.0-or-later
//
// The whole per-phone session lifecycle, from a phone appearing on USB to a
// CarPlay session running: device detection and the configuration switch, the
// usbmux host and its socket, lockdown and the carkit TLS channel, the NCM
// link, the AirPlay receiver, and the iAP2 session that carries metadata.
//
// (The name is historical -- this began as the USB half. What it owns now is
// the session, of which USB is the bottom layer.)
//
// The stage numbering matches docs/carplay_bringup.md so a bring-up session can
// stop at whichever layer is under investigation, and the stages appear in
// runAttachedSession in the order they must be *started*, which is not their
// numeric order: 3, 4, 6, 7, then 5.
#ifndef CARPLAY_USB_PIPELINE_H_
#define CARPLAY_USB_PIPELINE_H_

#include "node_config.h"
#include "zenoh_bridge.h"

#include <atomic>
#include <optional>
#include <string>

namespace carplay
{

// Runs the pipeline up to config.max_stage, logging each stage with the
// prefixes docs/carplay_bringup.md greps for. Blocks until `stop` is set once
// the requested stages have come up. Returns true if every attempted stage
// succeeded.
//
// `recording` is set true while an AirPlay session is live, so the caller's idle
// session-state publisher stands down. Optional.
bool runUsbPipeline(const NodeConfig& config, ZenohBridge& bridge, std::atomic<bool>& stop,
                    std::atomic<bool>* recording = nullptr);

}  // namespace carplay

#endif  // CARPLAY_USB_PIPELINE_H_
