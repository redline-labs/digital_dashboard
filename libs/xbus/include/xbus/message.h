// SPDX-License-Identifier: GPL-3.0-or-later
//
// The XBus message, which is the frame around everything an MTi says and
// everything it is told:
//
//     PREAMBLE(0xFA) | BID | MID | LEN | [EXTLEN:2] | DATA | CHECKSUM
//
// LEN counts only DATA. A LEN of 0xFF means the two bytes that follow carry a
// 16-bit length instead, which is how a message longer than 254 data bytes is
// sent -- in practice only a large MTData2.
//
// CHECKSUM is defined by a property rather than a formula: sum every byte
// EXCEPT the preamble, the checksum byte included, and the low byte of the
// result must be zero. So the byte to transmit is the two's complement of the
// sum of everything from BID onwards. The LLCP prints four complete messages
// in its worked examples and all four are asserted against this in
// tests/test_framing.cpp -- they are the only bytes in this library that came
// out of Xsens' encoder rather than out of a reading of the spec.
//
// Everything here is constexpr, both directions. Parsing at compile time is
// what lets the framing tests assert against those vendor vectors without
// running anything; building at compile time is what lets each configuration
// command be a `constexpr auto` whose bytes are checked byte-for-byte by
// static_assert rather than by an MTi refusing it on a bench.
//
// Reference: MT Low Level Communication Protocol Documentation, document
// MT0101P rev 2019.C, sections 5.1 and 5.2.

#ifndef XBUS_MESSAGE_H
#define XBUS_MESSAGE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "xbus/error.h"
#include "xbus/message_table.h"

namespace xbus
{

inline constexpr std::uint8_t kPreamble = 0xFA;

// A LEN of 0xFF selects the two-byte extended length that follows it.
inline constexpr std::uint8_t kExtendedLengthCode = 0xFF;

// A MID of 0xEE selects an extended, 16-bit message id. SEE parse_message()
// FOR WHY THIS LIBRARY REFUSES TO DECODE ONE.
inline constexpr std::uint8_t kExtendedMessageIdCode = 0xEE;

// A stand-alone MT answers on BID 1 ("first device") and on BID 255 ("master
// device"), and always replies with the BID it was addressed by. Messages the
// device originates rather than acknowledges -- in practice only MTData2 --
// always carry 255.
inline constexpr std::uint8_t kBidFirstDevice = 0x01;
inline constexpr std::uint8_t kBidMaster = 0xFF;

// PREAMBLE + BID + MID + LEN before DATA, CHECKSUM after it.
inline constexpr std::size_t kHeaderSize = 4;
inline constexpr std::size_t kChecksumSize = 1;
inline constexpr std::size_t kExtendedLengthSize = 2;

inline constexpr std::size_t kMaxStandardDataSize = 254;

// The LLCP's Table 3 caps an extended-length message at 2048 data bytes; the
// SDK's own xsmessage.h sets XS_MAXDATALEN to 8192 - 7 = 8185. The larger of
// the two is used here because it is what the vendor's implementation accepts,
// and because the checksum is what actually decides a message is real -- being
// stricter than the encoder would turn a legal message into a resync storm,
// while being looser costs only a buffer that briefly waits for bytes the
// framer's cap will eventually discard.
inline constexpr std::size_t kMaxExtendedDataSize = 8192 - (kHeaderSize + kChecksumSize + kExtendedLengthSize);

inline constexpr std::size_t kMaxMessageSize = kMaxExtendedDataSize + kHeaderSize + kChecksumSize + kExtendedLengthSize;

enum class Direction : std::uint8_t
{
    ToDevice,
    ToHost,
    // The message id is used for both a request and a set, and only the
    // presence of a payload tells them apart. See message_table.h.
    Both,
};

enum class MessageId : std::uint8_t
{
#define XBUS_MESSAGE_ENUM(id, Name, snake, dir) Name = id,
    XBUS_MESSAGE_TABLE(XBUS_MESSAGE_ENUM)
#undef XBUS_MESSAGE_ENUM
};

constexpr const char* message_name(MessageId id)
{
    switch (id)
    {
#define XBUS_MESSAGE_NAME(mid, Name, snake, dir) case MessageId::Name: return snake;
        XBUS_MESSAGE_TABLE(XBUS_MESSAGE_NAME)
#undef XBUS_MESSAGE_NAME
    }

    // Not a default: -- the switch must keep failing to compile when a row is
    // added to the table. A device may legitimately send a message id this
    // build has never heard of, and that is what lands here.
    return "unknown";
}

constexpr Direction message_direction(MessageId id)
{
    switch (id)
    {
#define XBUS_MESSAGE_DIR(mid, Name, snake, dir) case MessageId::Name: return Direction::dir;
        XBUS_MESSAGE_TABLE(XBUS_MESSAGE_DIR)
#undef XBUS_MESSAGE_DIR
    }

    return Direction::Both;
}

constexpr bool is_known_message(std::uint8_t id)
{
    switch (id)
    {
#define XBUS_MESSAGE_KNOWN(mid, Name, snake, dir) case mid: return true;
        XBUS_MESSAGE_TABLE(XBUS_MESSAGE_KNOWN)
#undef XBUS_MESSAGE_KNOWN
        default: break;
    }

    return false;
}

// The name to show for a message travelling host -> device.
//
// For a `Both` id the payload is the only thing that distinguishes a request
// from a set, so the caller has to supply it: 0xC0 with no payload asks the
// MTi what its output configuration is, and 0xC0 with a payload changes it.
// Getting this backwards in a log is how an afternoon disappears.
constexpr const char* describe_host_message(MessageId id, bool hasPayload)
{
    if (message_direction(id) != Direction::Both)
    {
        return message_name(id);
    }

    switch (id)
    {
        case MessageId::OptionFlags:         return hasPayload ? "set_option_flags" : "req_option_flags";
        case MessageId::UtcTime:             return hasPayload ? "set_utc_time" : "req_utc_time";
        case MessageId::PortConfig:          return hasPayload ? "set_port_config" : "req_port_config";
        case MessageId::OutputConfiguration: return hasPayload ? "set_output_configuration"
                                                               : "req_output_configuration";
        case MessageId::AlignmentRotation:   return hasPayload ? "set_alignment_rotation"
                                                               : "req_alignment_rotation";

        // Every id whose direction is not Both returned above. Spelled out
        // rather than defaulted so that marking a new row `Both` without
        // giving it a pair of names fails to compile here.
        case MessageId::ReqDeviceId:
        case MessageId::DeviceId:
        case MessageId::ReqConfiguration:
        case MessageId::Configuration:
        case MessageId::RestoreFactoryDef:
        case MessageId::RestoreFactoryDefAck:
        case MessageId::GoToMeasurement:
        case MessageId::GoToMeasurementAck:
        case MessageId::ReqFirmwareRevision:
        case MessageId::FirmwareRevision:
        case MessageId::ReqProductCode:
        case MessageId::ProductCode:
        case MessageId::ReqHardwareVersion:
        case MessageId::HardwareVersion:
        case MessageId::SetNoRotation:
        case MessageId::SetNoRotationAck:
        case MessageId::RunSelfTest:
        case MessageId::SelfTestResults:
        case MessageId::GoToConfig:
        case MessageId::GoToConfigAck:
        case MessageId::MtData2:
        case MessageId::WakeUp:
        case MessageId::WakeUpAck:
        case MessageId::Reset:
        case MessageId::ResetAck:
        case MessageId::ErrorReport:
        case MessageId::WarningReport:
        case MessageId::OptionFlagsAck:
        case MessageId::UtcTimeAck:
        case MessageId::PortConfigAck:
        case MessageId::AdjustUtcTime:
        case MessageId::AdjustUtcTimeAck:
        case MessageId::OutputConfigurationAck:
        case MessageId::AlignmentRotationAck:
            break;
    }

    return message_name(id);
}

// The acknowledgement a request is answered with. The LLCP's rule is flat:
// "a message with a certain MID value will be replied with a message with a
// MID value that is increased by one".
constexpr std::uint8_t ack_of(std::uint8_t requestId)
{
    return static_cast<std::uint8_t>(requestId + 1);
}

constexpr std::uint8_t ack_of(MessageId requestId)
{
    return ack_of(static_cast<std::uint8_t>(requestId));
}

// A validated message. `data` points into the buffer that was parsed, so it
// lives exactly as long as that buffer does.
struct MessageView
{
    std::uint8_t bid { 0 };

    // The wire byte, not the enum, so an unrecognised id round-trips. Compare
    // with is() rather than casting.
    std::uint8_t id { 0 };

    std::span<const std::uint8_t> data;

    constexpr bool is(MessageId wanted) const
    {
        return id == static_cast<std::uint8_t>(wanted);
    }

    // True when the message used the two-byte length form. Kept because
    // re-encoding has to reproduce it and because a device that starts using
    // it is worth noticing.
    bool extendedLength { false };
};

// The sum that has to come out to zero, over BID onwards. The accumulator is
// deliberately 8-bit: the property is defined modulo 256 and letting it wrap
// is the definition, not an overflow.
constexpr std::uint8_t checksum_sum(std::span<const std::uint8_t> afterPreamble)
{
    std::uint8_t sum = 0;
    for (std::uint8_t byte : afterPreamble)
    {
        sum = static_cast<std::uint8_t>(sum + byte);
    }
    return sum;
}

// The checksum byte to transmit for a message whose bytes from BID up to (but
// not including) the checksum are `afterPreamble`.
constexpr std::uint8_t checksum(std::span<const std::uint8_t> afterPreamble)
{
    return static_cast<std::uint8_t>(0u - checksum_sum(afterPreamble));
}

// How many bytes a message with `dataSize` data bytes occupies in total.
constexpr std::size_t message_size(std::size_t dataSize)
{
    return kHeaderSize + (dataSize > kMaxStandardDataSize ? kExtendedLengthSize : 0) +
           dataSize + kChecksumSize;
}

// Validate one message at the front of `bytes`. Trailing bytes are ignored, so
// this can be pointed at a stream buffer.
//
// Truncated means "not enough bytes yet" and is the only error a stream reader
// should treat as routine; everything else means the framing is wrong and the
// reader has to resynchronise.
constexpr Result<MessageView> parse_message(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < kHeaderSize + kChecksumSize)
    {
        return truncated(0);
    }

    if (bytes[0] != kPreamble)
    {
        return bad_framing(0);
    }

    const std::uint8_t bid = bytes[1];
    const std::uint8_t id = bytes[2];

    // An extended message id puts two further bytes somewhere after the fixed
    // header, and NOTHING SAYS WHERE. The LLCP does not document the form at
    // all; the SDK's xsmessage.h says only that the bytes after the header
    // "may be part of extended fields or actual data", which leaves open
    // whether LEN counts them. Either reading gives a different total size, so
    // decoding one would be a guess that looks like a fact -- and a wrong
    // guess here does not fail loudly, it mis-frames every message after it.
    //
    // An MTi-610 never sends one. Refusing means the framer drops a byte and
    // rescans, which self-heals at the cost of a few counted resyncs, and
    // leaves this as the one place to fix if a device ever does.
    if (id == kExtendedMessageIdCode)
    {
        return unsupported_format(id, 2);
    }

    const std::uint8_t lengthByte = bytes[3];
    const bool extended = (lengthByte == kExtendedLengthCode);

    std::size_t dataSize = lengthByte;
    std::size_t dataOffset = kHeaderSize;

    if (extended)
    {
        if (bytes.size() < kHeaderSize + kExtendedLengthSize + kChecksumSize)
        {
            return truncated(static_cast<std::uint16_t>(bytes.size()), id);
        }

        dataSize = (static_cast<std::size_t>(bytes[4]) << 8) | static_cast<std::size_t>(bytes[5]);
        dataOffset = kHeaderSize + kExtendedLengthSize;

        if (dataSize > kMaxExtendedDataSize)
        {
            return too_long(static_cast<std::uint16_t>(kHeaderSize));
        }

        // A device that used the long form for a payload the short form could
        // carry is doing something unusual but not illegal, so this is not an
        // error. It is worth knowing about, which is what MessageView's
        // extendedLength flag is for.
    }

    const std::size_t total = dataOffset + dataSize + kChecksumSize;

    if (bytes.size() < total)
    {
        return truncated(static_cast<std::uint16_t>(bytes.size()), id);
    }

    // Every byte from BID through the checksum inclusive.
    if (checksum_sum(bytes.subspan(1, total - 1)) != 0u)
    {
        return bad_checksum(static_cast<std::uint16_t>(total - 1));
    }

    return MessageView { bid, id, bytes.subspan(dataOffset, dataSize), extended };
}

// Build a message around a fixed-size payload. The size is in the type, so the
// result is a std::array a caller can hold as `constexpr auto` -- which is the
// point: a command whose bytes are wrong then fails to compile against a
// static_assert instead of being refused by an MTi on a bench.
//
// Only the standard length form is produced. Nothing this library sends to a
// device is longer than 254 bytes, and an encoder for a form we cannot test
// would be an untested branch pretending to be a feature.
template <std::size_t N>
constexpr std::array<std::uint8_t, N + kHeaderSize + kChecksumSize> make_message(
    MessageId id, const std::array<std::uint8_t, N>& data, std::uint8_t bid = kBidMaster)
{
    static_assert(N <= kMaxStandardDataSize,
                  "a standard-length XBus message carries at most 254 data bytes");

    std::array<std::uint8_t, N + kHeaderSize + kChecksumSize> out {};
    out[0] = kPreamble;
    out[1] = bid;
    out[2] = static_cast<std::uint8_t>(id);
    out[3] = static_cast<std::uint8_t>(N);

    for (std::size_t i = 0; i < N; ++i)
    {
        out[kHeaderSize + i] = data[i];
    }

    out[out.size() - 1] = checksum(std::span<const std::uint8_t>(out.data() + 1, out.size() - 2));

    return out;
}

// The runtime form, for a payload whose size is only known once the device's
// current configuration has been read back -- SetOutputConfiguration is the
// only real caller. Writes into `out` and returns how many bytes were used, or
// Truncated when `out` is too small: the caller owns the buffer and the
// encoder never allocates, the same shape as gsof::trimcomm::encode_packet.
constexpr Result<std::size_t> encode_message(MessageId id, std::span<const std::uint8_t> data,
                                             std::span<std::uint8_t> out,
                                             std::uint8_t bid = kBidMaster)
{
    if (data.size() > kMaxStandardDataSize)
    {
        return too_long(static_cast<std::uint16_t>(data.size()));
    }

    const std::size_t total = kHeaderSize + data.size() + kChecksumSize;
    if (out.size() < total)
    {
        return truncated(static_cast<std::uint16_t>(out.size()));
    }

    out[0] = kPreamble;
    out[1] = bid;
    out[2] = static_cast<std::uint8_t>(id);
    out[3] = static_cast<std::uint8_t>(data.size());

    for (std::size_t i = 0; i < data.size(); ++i)
    {
        out[kHeaderSize + i] = data[i];
    }

    out[total - 1] = checksum(out.subspan(1, total - 2));

    return total;
}

} // namespace xbus

#endif // XBUS_MESSAGE_H
