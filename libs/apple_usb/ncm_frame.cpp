// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/iap2/ncm_bridge.py
#include "apple_usb/ncm_frame.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <charconv>
#include <system_error>

namespace apple_usb
{

namespace
{

uint16_t get_le16(const uint8_t* p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

uint32_t get_le32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void put_le16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put_le32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

}  // namespace

std::vector<std::vector<uint8_t>> parseNtb(const std::vector<uint8_t>& ntb)
{
    std::vector<std::vector<uint8_t>> frames;
    const size_t total = ntb.size();
    if (total < kNth16Length)
    {
        SPDLOG_WARN("[ncm] rx NTB too short: {} bytes (< {})", total, kNth16Length);
        return frames;
    }

    const uint32_t sig = get_le32(ntb.data());
    const uint16_t header_len = get_le16(ntb.data() + 4);
    const uint16_t sequence = get_le16(ntb.data() + 6);
    const uint16_t block_len = get_le16(ntb.data() + 8);
    uint16_t ndp_idx = get_le16(ntb.data() + 10);

    if (sig != kNth16Signature)
    {
        SPDLOG_WARN("[ncm] rx NTH16 bad signature at offset 0: 0x{:08x} (want 0x{:08x}), {} bytes",
                    sig, kNth16Signature, total);
        return frames;
    }
    if (block_len > total)
    {
        SPDLOG_WARN("[ncm] rx NTH16 wBlockLength at offset 8 = {} exceeds the {} bytes received",
                    block_len, total);
    }
    SPDLOG_DEBUG("[ncm] rx NTB seq={} blockLen={} headerLen={} ndpIndex={} received={}", sequence,
                 block_len, header_len, ndp_idx, total);

    size_t ndp_count = 0;
    while (ndp_idx != 0)
    {
        if (static_cast<size_t>(ndp_idx) + 12 > total)
        {
            SPDLOG_WARN("[ncm] rx NDP16 index {} does not leave room for a 12-byte NDP in the {} "
                        "bytes received",
                        ndp_idx, total);
            break;
        }
        const uint8_t* ndp = ntb.data() + ndp_idx;
        const uint32_t nsig = get_le32(ndp);
        const uint16_t nlen = get_le16(ndp + 4);
        const uint16_t next_ndp = get_le16(ndp + 6);
        if ((nsig & kNdp16SignatureMask) != (kNdp16Signature & kNdp16SignatureMask))
        {
            SPDLOG_WARN("[ncm] rx NDP16 bad signature at offset {}: 0x{:08x} (want 0x{:08x} with "
                        "the last byte free)",
                        ndp_idx, nsig, kNdp16Signature);
            break;
        }
        if (nlen < 12)
        {
            SPDLOG_WARN("[ncm] rx NDP16 at offset {}: wLength={} is too small for a pointer table",
                        ndp_idx, nlen);
            break;
        }
        ++ndp_count;

        size_t off = static_cast<size_t>(ndp_idx) + 8;
        const size_t end = std::min<size_t>(static_cast<size_t>(ndp_idx) + nlen, total);
        size_t datagrams = 0;
        while (off + 4 <= end)
        {
            const uint16_t d_idx = get_le16(ntb.data() + off);
            const uint16_t d_len = get_le16(ntb.data() + off + 2);
            if (d_idx == 0 || d_len == 0)
            {
                // The (0, 0) terminator.
                break;
            }
            if (static_cast<size_t>(d_idx) + d_len <= total)
            {
                frames.emplace_back(ntb.begin() + d_idx, ntb.begin() + d_idx + d_len);
                ++datagrams;
            }
            else
            {
                SPDLOG_WARN("[ncm] rx datagram pointer at offset {} runs off the block: "
                            "index={} len={} (block has {} bytes)",
                            off, d_idx, d_len, total);
            }
            off += 4;
        }
        SPDLOG_DEBUG("[ncm] rx NDP16 @{} wLength={} datagrams={} nextNdpIndex={}", ndp_idx, nlen,
                     datagrams, next_ndp);

        // The spec allows a chain of NDPs; guard against a device (or a
        // corrupted block) pointing backwards, which would loop forever.
        if (next_ndp != 0 && next_ndp <= ndp_idx)
        {
            SPDLOG_WARN("[ncm] rx NDP16 @{}: wNextNdpIndex={} does not advance; stopping the chain",
                        ndp_idx, next_ndp);
            break;
        }
        ndp_idx = next_ndp;
    }
    SPDLOG_DEBUG("[ncm] rx NTB seq={} yielded {} datagram(s) from {} NDP(s)", sequence,
                 frames.size(), ndp_count);
    return frames;
}

std::vector<uint8_t> buildNtb(const uint8_t* frame, size_t len, uint16_t& seq, uint32_t out_max)
{
    // One datagram per block: NTH16 (12) + NDP16 with a single entry and the
    // (0,0) terminator (16) = 28 bytes of framing, then the ethernet frame.
    // Both 12 and 28 are 4-byte aligned, which satisfies the wNdpOutAlignment
    // and wNdpOutPayloadRemainder every device we have seen reports.
    seq = static_cast<uint16_t>(seq + 1);
    const auto block_len = static_cast<uint16_t>(kTxDatagramOffset + len);

    std::vector<uint8_t> ntb;
    ntb.reserve(kTxDatagramOffset + len + 1);

    // NTH16.
    put_le32(ntb, kNth16Signature);
    put_le16(ntb, static_cast<uint16_t>(kNth16Length));
    put_le16(ntb, seq);
    put_le16(ntb, block_len);
    put_le16(ntb, static_cast<uint16_t>(kNth16Length));  // wNdpIndex: NDP follows the NTH

    // NDP16: header, one datagram entry, terminator.
    put_le32(ntb, kNdp16Signature);
    put_le16(ntb, static_cast<uint16_t>(kNdp16Length));
    put_le16(ntb, 0);  // wNextNdpIndex: no chain
    put_le16(ntb, static_cast<uint16_t>(kTxDatagramOffset));
    put_le16(ntb, static_cast<uint16_t>(len));
    put_le16(ntb, 0);  // terminator index
    put_le16(ntb, 0);  // terminator length

    ntb.insert(ntb.end(), frame, frame + len);

    // A block that is an exact multiple of the bulk max packet size would need
    // a zero-length packet to terminate the transfer; pad instead.
    if (ntb.size() % 512 == 0)
    {
        ntb.push_back(0);
    }
    if (ntb.size() > out_max)
    {
        SPDLOG_WARN("[ncm] tx NTB is {} bytes, over the device's dwNtbOutMaxSize {}", ntb.size(),
                    out_max);
    }
    SPDLOG_DEBUG("[ncm] tx NTB seq={} blockLen={} datagrams=1 frame={} wire={}", seq, block_len,
                 len, ntb.size());
    return ntb;
}

std::string deriveEui64LinkLocal(const std::string& mac)
{
    unsigned b[6] = {0, 0, 0, 0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 6; ++i)
    {
        if (pos + 2 > mac.size())
        {
            return {};
        }
        unsigned value = 0;
        const auto* first = mac.data() + pos;
        // from_chars reports success on a *partial* parse, so "0g" comes back
        // as 0 with ptr on the 'g'. Requiring it to have consumed both
        // characters is what rejects that; without the ptr check a malformed
        // MAC silently derives a plausible-looking but wrong link-local, which
        // then goes into CarPlayStartSession and the phone dials an address we
        // are not listening on.
        const auto [ptr, ec] = std::from_chars(first, first + 2, value, 16);
        if (ec != std::errc{} || ptr != first + 2)
        {
            return {};
        }
        b[i] = value;
        pos += 2;
        if (i < 5)
        {
            if (pos >= mac.size() || mac[pos] != ':')
            {
                return {};
            }
            ++pos;
        }
    }
    if (pos != mac.size())
    {
        return {};
    }
    return fmt::format("fe80::{:x}:{:x}:{:x}:{:x}", ((b[0] ^ 0x02u) << 8) | b[1],
                       (b[2] << 8) | 0xffu, (0xfeu << 8) | b[3], (b[4] << 8) | b[5]);
}

}  // namespace apple_usb
