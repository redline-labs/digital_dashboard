// SPDX-License-Identifier: GPL-3.0-or-later

#include "mototrbo/nai.h"

namespace mototrbo::nai::tms
{

namespace
{

// Header bytes before the text, per Moto.Net's own encoder:
//   00 <len> <flags> 00 <0x80|seq> 04 0D 00 0A 00
constexpr std::size_t kHeaderSize = 10;
constexpr std::size_t kBodyLengthAdjust = 8;

// Reserved/system bits, always set, plus the acknowledgement request.
constexpr std::uint8_t kFlagsBase = 0xA0;
constexpr std::uint8_t kFlagAckRequested = 0x40;

constexpr std::uint8_t kSequenceMask = 0x1F;

} // namespace

Result<std::vector<std::uint8_t>> encode_text(std::string_view text, std::uint8_t sequence, bool requiresAck)
{
    const std::size_t bodyLength = text.size() * 2 + kBodyLengthAdjust;
    if (bodyLength > 0xFFu)
    {
        return invalid_argument();
    }

    for (const char character : text)
    {
        if (static_cast<unsigned char>(character) > 0x7Fu)
        {
            return invalid_argument();
        }
    }

    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + text.size() * 2);
    out.push_back(0x00);
    out.push_back(static_cast<std::uint8_t>(bodyLength));
    out.push_back(static_cast<std::uint8_t>(kFlagsBase | (requiresAck ? kFlagAckRequested : 0u)));
    out.push_back(0x00);
    out.push_back(static_cast<std::uint8_t>(0x80u | (sequence & kSequenceMask)));
    out.push_back(0x04);
    out.push_back(0x0D);
    out.push_back(0x00);
    out.push_back(0x0A);
    out.push_back(0x00);

    for (const char character : text)
    {
        out.push_back(static_cast<std::uint8_t>(character));
        out.push_back(0x00);
    }

    return out;
}

Result<TextMessage> parse(std::span<const std::uint8_t> datagram)
{
    if (datagram.size() < 6)
    {
        return truncated(static_cast<std::uint16_t>(datagram.size()));
    }

    TextMessage message;
    message.type = static_cast<MessageType>(datagram[2] & kSequenceMask);
    message.sequence = datagram[4] & kSequenceMask;

    // The text starts at 6, or after a `0D 00 0A ..` address prefix when one
    // is present.
    std::size_t offset = 6;
    if (datagram.size() >= offset + 4 && datagram[offset] == 0x0D && datagram[offset + 2] == 0x0A)
    {
        offset += 4;
    }

    // UTF-16LE, read back as ASCII. Moto.Net's receive path trims four
    // trailing bytes; ours does not, because our encoder adds none and no
    // radio-originated message has been captured to say whether one is there.
    // [HW-VERIFY]
    for (; offset + 1 < datagram.size(); offset += 2)
    {
        if (const std::uint8_t character = datagram[offset]; character != 0)
        {
            message.text.push_back(static_cast<char>(character));
        }
    }

    return message;
}

} // namespace mototrbo::nai::tms
