// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef APPLE_USB_PAIR_RECORD_H_
#define APPLE_USB_PAIR_RECORD_H_

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace apple_usb
{

// The trust relationship between this host and one phone, established once by
// the "Trust This Computer?" prompt and reused on every connection after.
//
// Stored as a property list -- binary in practice, since that is what wrote the
// records already on disk, though the parser accepts either. The certificates
// and keys are PEM blobs inside it.
struct PairRecord
{
    // Presented as the TLS client certificate. Note that this is the *root*
    // pair, not the host pair: the device pins the root identity for the
    // session, and the host certificate is only part of what pairing exchanged.
    std::vector<uint8_t> root_certificate;
    std::vector<uint8_t> root_private_key;

    std::vector<uint8_t> host_certificate;
    std::vector<uint8_t> host_private_key;
    std::vector<uint8_t> device_certificate;

    // Sent with StartService for services that ask for it, proving the session
    // may unlock data protected while the device is locked.
    std::vector<uint8_t> escrow_bag;

    // Identify this host to lockdown on StartSession. A record is only valid
    // for the BUID it was created under.
    std::string host_id;
    std::string system_buid;

    std::string wifi_mac_address;

    // Parses either format. Returns nullopt when the blob is not a plist, or
    // when a field StartSession cannot proceed without is missing.
    static std::optional<PairRecord> parse(const std::vector<uint8_t>& blob);

    // Serializes back to a binary plist, the format the records on disk use.
    std::vector<uint8_t> encode() const;
};

}  // namespace apple_usb

#endif  // APPLE_USB_PAIR_RECORD_H_
