// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/mfiSigner.ts
#ifndef IAP2_MFI_SIGNER_H_
#define IAP2_MFI_SIGNER_H_

#include <cstdint>
#include <optional>
#include <vector>

namespace iap2
{

// Interface to the Apple MFi authentication coprocessor. Used in two places:
// the iAP2 authentication handshake and the AirPlay /auth-setup (MFiSAP)
// exchange.
class MfiSigner
{
  public:
    virtual ~MfiSigner() = default;

    // The accessory certificate chain (PKCS7/DER) burned into the chip.
    virtual std::optional<std::vector<uint8_t>> certificate() = 0;

    // Sign a challenge. Expected challenge size depends on protocolMajor():
    // version 2 uses SHA-1 (20 bytes), version 3 uses SHA-256 (32 bytes).
    virtual std::optional<std::vector<uint8_t>> signChallenge(const std::vector<uint8_t>& challenge) = 0;

    virtual int protocolMajor() = 0;
};

}  // namespace iap2

#endif  // IAP2_MFI_SIGNER_H_
