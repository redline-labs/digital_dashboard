// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef IAP2_MCP2221A_MFI_SIGNER_H_
#define IAP2_MCP2221A_MFI_SIGNER_H_

#include "iap2/mfi_signer.h"

#include "apple_mfi_ic/apple_mfi_ic.h"

namespace iap2
{

// MfiSigner backed by the MFi coprocessor behind an MCP2221A USB-I2C bridge.
class Mcp2221aMfiSigner : public MfiSigner
{
  public:
    Mcp2221aMfiSigner() = default;
    ~Mcp2221aMfiSigner() override;

    // Opens the MCP2221A and probes the coprocessor. Must succeed before use.
    bool init();

    std::optional<std::vector<uint8_t>> certificate() override;
    std::optional<std::vector<uint8_t>> signChallenge(const std::vector<uint8_t>& challenge) override;
    int protocolMajor() override;

  private:
    AppleMFIIC ic_;
    int protocol_major_ = 0;
};

}  // namespace iap2

#endif  // IAP2_MCP2221A_MFI_SIGNER_H_
