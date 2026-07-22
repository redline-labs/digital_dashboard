// SPDX-License-Identifier: GPL-3.0-or-later
//
// Stage 5 of docs/carplay_bringup.md: the iAP2 link layer, accessory
// identification, and MFi authentication, carried over the lockdown/carkit TLS
// channel established in stage 4.
#ifndef CARPLAY_IAP2_SESSION_H_
#define CARPLAY_IAP2_SESSION_H_

#include "apple_usb/lockdown.h"
#include "iap2/mfi_signer.h"

#include <atomic>
#include <functional>
#include <optional>
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

    // Shared with the AirPlay receiver, which needs the same coprocessor for
    // /auth-setup. Owned by the caller; null means open one internally.
    iap2::MfiSigner* signer = nullptr;

    // Seconds to wait for the link to negotiate before giving up.
    unsigned negotiate_timeout_ms = 10000;

    // Invoked once the phone reports wired CarPlay available. Returns the
    // accessory endpoint the phone should dial, or nullopt when the transport
    // is not up -- in which case CarPlayStartSession is not sent.
    //
    // This is a callback rather than a plain field because the NCM link is
    // brought up *after* the iAP2 session exists: the address does not exist
    // yet when the session starts.
    struct Endpoint
    {
        std::string link_local_address;  // accessory fe80::, from NcmBridge
        std::string device_identifier;   // accessory MAC, colon separated
        uint32_t port = 7000;
    };
    std::function<std::optional<Endpoint>()> endpoint_provider;
};

// Drives the iAP2 session until the link dies or `stop` is set. Returns true if
// identification (and, unless allowed to be missing, MFi authentication)
// succeeded.
bool runIap2Session(apple_usb::CarkitChannel& channel, const Iap2SessionOptions& options,
                    std::atomic<bool>& stop);

}  // namespace carplay

#endif  // CARPLAY_IAP2_SESSION_H_
