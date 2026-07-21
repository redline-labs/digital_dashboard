// SPDX-License-Identifier: GPL-3.0-or-later
#include "iap2/mcp2221a_mfi_signer.h"

#include <spdlog/spdlog.h>

namespace iap2
{

Mcp2221aMfiSigner::~Mcp2221aMfiSigner()
{
    ic_.close();
}

bool Mcp2221aMfiSigner::init()
{
    if (!ic_.init())
    {
        SPDLOG_ERROR("Failed to open MFi coprocessor via MCP2221A");
        return false;
    }

    const auto info = ic_.query_device_info();
    if (!info)
    {
        SPDLOG_ERROR("MFi coprocessor did not answer device-info query");
        return false;
    }

    protocol_major_ = info->authentication_protocol_major_version;
    SPDLOG_INFO("MFi coprocessor ready: {}", info->to_string());
    return true;
}

std::optional<std::vector<uint8_t>> Mcp2221aMfiSigner::certificate()
{
    auto cert = ic_.read_certificate_data();
    if (cert.empty())
    {
        SPDLOG_ERROR("MFi certificate read returned no data");
        return std::nullopt;
    }
    return cert;
}

std::optional<std::vector<uint8_t>> Mcp2221aMfiSigner::signChallenge(const std::vector<uint8_t>& challenge)
{
    return ic_.sign_challenge(challenge);
}

int Mcp2221aMfiSigner::protocolMajor()
{
    return protocol_major_;
}

}  // namespace iap2
