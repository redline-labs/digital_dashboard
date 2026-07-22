// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 5 of docs/carplay_bringup.md: the iAP2 link layer, accessory
// identification, and MFi authentication, carried over the lockdown/carkit TLS
// channel established in stage 4.
#ifndef CARPLAY_IAP2_SESSION_H_
#define CARPLAY_IAP2_SESSION_H_

#include "apple_usb/lockdown.h"

#include <atomic>
#include <string>

namespace carplay
{

struct Iap2SessionOptions
{
    // Continue past MFi authentication when the coprocessor is unreachable.
    // The phone will refuse CarPlay, but the link layer, identification and the
    // phone's own message traffic are still exercised -- which is worth doing
    // while the MFi board is being repaired.
    bool allow_missing_mfi = false;

    // docs/carplay_bringup.md flags this: LIVI decodes a zero-length iAP2
    // boolean as absent, which makes CarPlayAvailability.wired_available falsy
    // and silently skips CarPlayStartSession. Setting this treats a zero-length
    // bool in that message as "true" (presence-as-value), which is the doc's
    // one-line experiment.
    bool zero_length_bool_is_true = false;

    // Seconds to wait for the link to negotiate before giving up.
    unsigned negotiate_timeout_ms = 10000;
};

// Drives the iAP2 session until the link dies or `stop` is set. Returns true if
// identification (and, unless allowed to be missing, MFi authentication)
// succeeded.
bool runIap2Session(apple_usb::CarkitChannel& channel, const Iap2SessionOptions& options,
                    std::atomic<bool>& stop);

}  // namespace carplay

#endif  // CARPLAY_IAP2_SESSION_H_
