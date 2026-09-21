// SPDX-License-Identifier: GPL-3.0-or-later
#include "iap2/http_mfi_signer.h"

#include "httplib_wrapped/httplib_include.h"

#include <spdlog/spdlog.h>

#include <charconv>
#include <string>

namespace iap2
{

namespace
{

// The chip is slow and gets slower. A sign takes the better part of a second on
// its own, and apple_mfi_ic retries the access that wakes it from the sleep it
// enters after ~30 ms idle, so a single call can legitimately span several
// seconds before it has anything to say. 15 s is long enough that a timeout
// here means something is actually wrong, and short enough that the iAP2
// authentication above gives up on its own terms rather than hanging.
constexpr time_t kReadTimeoutSec = 15;

// Connecting is a LAN round trip or it is a mistake in the URL.
constexpr time_t kConnectTimeoutSec = 3;

}  // namespace

HttpMfiSigner::HttpMfiSigner(std::string base_url, std::string token)
    : base_url_(std::move(base_url)), token_(std::move(token))
{
    while (!base_url_.empty() && base_url_.back() == '/')
    {
        base_url_.pop_back();
    }
}

std::optional<std::vector<uint8_t>> HttpMfiSigner::request(const char* path,
                                                           const std::vector<uint8_t>* body)
{
    // A client per call. The three calls happen once each per phone, seconds
    // apart, so there is no connection to keep alive worth the thread-safety
    // question a shared client would raise -- signChallenge can be reached from
    // the iAP2 thread and the AirPlay thread both.
    httplib::Client client(base_url_);
    client.set_connection_timeout(kConnectTimeoutSec, 0);
    client.set_read_timeout(kReadTimeoutSec, 0);
    client.set_write_timeout(kReadTimeoutSec, 0);

    httplib::Headers headers;
    if (!token_.empty())
    {
        headers.emplace("Authorization", "Bearer " + token_);
    }

    httplib::Result result =
        body == nullptr
            ? client.Get(path, headers)
            : client.Post(path, headers,
                          reinterpret_cast<const char*>(body->data()), body->size(),
                          "application/octet-stream");

    if (!result)
    {
        SPDLOG_ERROR("[mfi] {}{} unreachable: {}", base_url_, path,
                     httplib::to_string(result.error()));
        return std::nullopt;
    }
    if (result->status != 200)
    {
        // The proxy puts its reason in the body; it is the only diagnostic that
        // crosses the network, so log it rather than just the status.
        SPDLOG_ERROR("[mfi] {}{} -> HTTP {}: {}", base_url_, path, result->status, result->body);
        return std::nullopt;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(result->body.data());
    return std::vector<uint8_t>(bytes, bytes + result->body.size());
}

bool HttpMfiSigner::init()
{
    const auto body = request("/mfi/v1/protocol", nullptr);
    if (!body)
    {
        return false;
    }

    const auto* first = reinterpret_cast<const char*>(body->data());
    const auto* last = first + body->size();
    int major = 0;
    const auto parsed = std::from_chars(first, last, major);
    if (parsed.ec != std::errc{} || major <= 0)
    {
        SPDLOG_ERROR("[mfi] proxy returned a protocol version we cannot read ({} bytes)",
                     body->size());
        return false;
    }

    protocol_major_ = major;
    SPDLOG_INFO("[mfi] remote coprocessor at {} (protocol major {})", base_url_, protocol_major_);
    return true;
}

std::optional<std::vector<uint8_t>> HttpMfiSigner::certificate()
{
    return request("/mfi/v1/certificate", nullptr);
}

std::optional<std::vector<uint8_t>> HttpMfiSigner::signChallenge(
    const std::vector<uint8_t>& challenge)
{
    return request("/mfi/v1/sign", &challenge);
}

int HttpMfiSigner::protocolMajor()
{
    return protocol_major_;
}

}  // namespace iap2
