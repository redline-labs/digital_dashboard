// SPDX-License-Identifier: GPL-3.0-or-later
//
// The non-Linux NcmBridge: everything above it builds and links, stage 6 just
// reports that this host cannot carry the AV link.
//
// This exists so the *rest* of the stack is not held hostage by the one piece
// that genuinely needs Linux. Before it, `nodes/carplay` dropped apple_usb
// entirely on macOS, which took usb_pipeline.cpp and iap2_session.cpp out of
// the build with it -- ~1,500 lines that compile here perfectly well and were
// only excluded by association. Four out-of-line methods buy all of that back.
//
// It is deliberately not a partial implementation. macOS can get an ethernet
// segment out of a feth pair plus BPF injection, so a real backend is possible;
// what is not possible is doing it silently or unprivileged, and a bridge that
// half-works would be worse for bring-up than one that says so. If that backend
// is ever written it replaces this file rather than growing out of it.
#include "apple_usb/ncm_bridge.h"

#include <spdlog/spdlog.h>

#include <utility>

namespace apple_usb
{

NcmBridge::NcmBridge(DeviceInfo device) : device_(std::move(device)) {}

NcmBridge::~NcmBridge()
{
    NcmBridge::stop();
}

bool NcmBridge::start()
{
    SPDLOG_ERROR("[ncm] no TAP device on this host, so the CarPlay AV link cannot be carried. "
                 "Stages 1-5 (USB, mux, lockdown/carkit, iAP2) do run here; stage 6 onwards "
                 "needs Linux. Use --max-stage 5 to stop cleanly, or --simulate to exercise "
                 "the dashboard side without a phone.");
    return false;
}

void NcmBridge::stop() {}

}  // namespace apple_usb
