// SPDX-License-Identifier: GPL-3.0-or-later
//
// Big-endian 16-bit access, which is what every length, identifier and
// parameter tag in iAP2 is.
//
// Its own header because all three layers need it -- the link layer's packet
// header, the control-message framing, and the parameter codec -- and it was
// copied into each of them.
#ifndef IAP2_BYTE_ORDER_H_
#define IAP2_BYTE_ORDER_H_

#include <cstdint>
#include <vector>

namespace iap2
{

inline void put_be16(std::vector<uint8_t>& out, uint16_t value)
{
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

inline uint16_t get_be16(const uint8_t* p)
{
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

}  // namespace iap2

#endif  // IAP2_BYTE_ORDER_H_
