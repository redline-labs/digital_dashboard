// SPDX-License-Identifier: GPL-3.0-or-later
#include "iap2/shared_mfi_signer.h"

#include <spdlog/spdlog.h>

namespace iap2
{

SharedMfiSigner::SharedMfiSigner(Factory factory, std::chrono::milliseconds retry_every) :
    factory_(std::move(factory)), retry_every_(retry_every)
{
}

MfiSigner* SharedMfiSigner::openLocked()
{
    if (signer_)
    {
        return signer_.get();
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_attempt_ && now - *last_attempt_ < retry_every_)
    {
        return nullptr;
    }
    const bool first = !last_attempt_.has_value();
    last_attempt_ = now;
    signer_ = factory_();
    if (signer_)
    {
        if (!first)
        {
            SPDLOG_INFO("[mfi] coprocessor came up late, protocol major {}", signer_->protocolMajor());
        }
        return signer_.get();
    }
    SPDLOG_WARN("[mfi] coprocessor unavailable; trying again in {} ms", retry_every_.count());
    return nullptr;
}

bool SharedMfiSigner::ready()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return openLocked() != nullptr;
}

std::optional<std::vector<uint8_t>> SharedMfiSigner::certificate()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    MfiSigner* signer = openLocked();
    return signer != nullptr ? signer->certificate() : std::nullopt;
}

std::optional<std::vector<uint8_t>> SharedMfiSigner::signChallenge(const std::vector<uint8_t>& challenge)
{
    const std::lock_guard<std::mutex> lock(mutex_);
    MfiSigner* signer = openLocked();
    return signer != nullptr ? signer->signChallenge(challenge) : std::nullopt;
}

int SharedMfiSigner::protocolMajor()
{
    const std::lock_guard<std::mutex> lock(mutex_);
    MfiSigner* signer = openLocked();
    return signer != nullptr ? signer->protocolMajor() : 0;
}

}  // namespace iap2
