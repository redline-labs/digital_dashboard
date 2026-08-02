// SPDX-License-Identifier: GPL-3.0-or-later

#include "airplay/channel_crypto.h"

#include "airplay/crypto.h"

#include <algorithm>

namespace airplay
{

void ChannelCrypto::activate(Bytes inbound, Bytes outbound)
{
    inbound_key_ = std::move(inbound);
    outbound_key_ = std::move(outbound);
    // Both counters restart with the keys: they are part of the same shared
    // state, and a key change with a stale counter authenticates nothing.
    inbound_counter_ = 0;
    outbound_counter_ = 0;
    active_ = true;
}

void ChannelCrypto::deactivate()
{
    active_ = false;
}

bool ChannelCrypto::open(Bytes& cipher, Bytes& plain)
{
    while (cipher.size() >= 2)
    {
        const size_t length =
            static_cast<size_t>(cipher[0]) | (static_cast<size_t>(cipher[1]) << 8);
        const size_t frame = 2 + length + 16;
        if (cipher.size() < frame)
        {
            break;  // more of this frame still arriving
        }

        const Bytes aad(cipher.begin(), cipher.begin() + 2);
        const Bytes sealed(cipher.begin() + 2, cipher.begin() + static_cast<long>(frame));
        const auto opened =
            crypto::chachaOpen(inbound_key_, crypto::nonce64(inbound_counter_), sealed, aad);
        if (!opened)
        {
            return false;
        }
        ++inbound_counter_;
        plain.insert(plain.end(), opened->begin(), opened->end());
        cipher.erase(cipher.begin(), cipher.begin() + static_cast<long>(frame));
    }
    return true;
}

Bytes ChannelCrypto::seal(const Bytes& plain)
{
    Bytes out;
    size_t offset = 0;
    while (offset < plain.size())
    {
        const size_t take = std::min(kMaxFramePlaintext, plain.size() - offset);
        const Bytes aad{static_cast<uint8_t>(take & 0xFF),
                        static_cast<uint8_t>((take >> 8) & 0xFF)};
        const Bytes chunk(plain.begin() + static_cast<long>(offset),
                          plain.begin() + static_cast<long>(offset + take));
        const Bytes sealed =
            crypto::chachaSeal(outbound_key_, crypto::nonce64(outbound_counter_), chunk, aad);
        ++outbound_counter_;

        out.insert(out.end(), aad.begin(), aad.end());
        out.insert(out.end(), sealed.begin(), sealed.end());
        offset += take;
    }
    return out;
}

}  // namespace airplay
