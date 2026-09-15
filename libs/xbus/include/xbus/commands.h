// SPDX-License-Identifier: GPL-3.0-or-later
//
// The messages this tree sends to an MTi-610, and the replies it reads back.
//
// A CAVEAT THAT APPLIES TO THE WHOLE FILE, and that the tests repeat: no MTi
// has ever seen these bytes. libs/gsof carries the same warning about its
// APPFILE commands, and it earned it -- the record parsers there were
// validated against captures while the command encodings were not, and as of
// 2026-08-23 no port on the BD992 had accepted one. The record parsers in
// mtdata2.h at least decode a format two independent documents describe; the
// encoders here are a reading of the LLCP's tables and nothing more.
//
// What holds them up in the meantime:
//
//   * FIVE COMPLETE MESSAGES that Xsens printed, checksum included, in the
//     LLCP. Every zero-payload command in this file is built by the same
//     make_message() that reproduces those five byte for byte, so the FRAMING
//     of a command is as well established as anything here gets. What is
//     unverified is the payload content of the two commands that have one.
//
//   * Every command is a `constexpr auto` returning std::array, so its bytes
//     are asserted at compile time in tests/test_commands.cpp rather than
//     discovered on a bench.
//
// Reference: MT Low Level Communication Protocol Documentation, MT0101P rev
// 2019.C, section 5.3.

#ifndef XBUS_COMMANDS_H
#define XBUS_COMMANDS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "xbus/byte_order.h"
#include "xbus/data_id.h"
#include "xbus/error.h"
#include "xbus/message.h"

namespace xbus
{

// ---------------------------------------------------------------------------
// Commands with no payload.
//
// Each is a five-byte message and each is checked byte for byte by a
// static_assert. The device answers every one of them with the same message id
// plus one.
// ---------------------------------------------------------------------------

namespace detail
{
constexpr std::array<std::uint8_t, 5> bare(MessageId id)
{
    return make_message(id, std::array<std::uint8_t, 0> {});
}
} // namespace detail

// Leave Measurement state so the configuration can be changed. Valid in both
// states, so it doubles as "confirm we are in Config".
inline constexpr std::array<std::uint8_t, 5> kGoToConfig = detail::bare(MessageId::GoToConfig);

// Start streaming MTData2 with whatever the current configuration says.
inline constexpr std::array<std::uint8_t, 5> kGoToMeasurement =
    detail::bare(MessageId::GoToMeasurement);

// Reset and re-run the WakeUp procedure. Valid in both states.
inline constexpr std::array<std::uint8_t, 5> kReset = detail::bare(MessageId::Reset);

// Answer an unsolicited WakeUp. MUST BE SENT WITHIN 500 ms of receiving one,
// or the device enters Measurement with its stored configuration instead of
// waiting in Config -- which looks exactly like a device that ignored us.
inline constexpr std::array<std::uint8_t, 5> kWakeUpAck = detail::bare(MessageId::WakeUpAck);

// Identification. All four are Config-state only.
inline constexpr std::array<std::uint8_t, 5> kReqDeviceId = detail::bare(MessageId::ReqDeviceId);
inline constexpr std::array<std::uint8_t, 5> kReqProductCode =
    detail::bare(MessageId::ReqProductCode);
inline constexpr std::array<std::uint8_t, 5> kReqFirmwareRevision =
    detail::bare(MessageId::ReqFirmwareRevision);
inline constexpr std::array<std::uint8_t, 5> kReqHardwareVersion =
    detail::bare(MessageId::ReqHardwareVersion);

// Ask what the device is currently configured to output. The reply has the
// same message id and the same payload shape as SetOutputConfiguration.
inline constexpr std::array<std::uint8_t, 5> kReqOutputConfiguration =
    detail::bare(MessageId::OutputConfiguration);

// Ask for the port configuration. The reply is a list of 32-bit words, one per
// serial port, whose BIT LAYOUT THIS LIBRARY DOES NOT DECODE -- the LLCP
// documents it only as Figure 2, an image with no accompanying text, and
// guessing at it would mis-set a baud rate in a way that leaves the device
// unreachable. The words are round-tripped opaquely. See docs/nodes/mti610_bridge.md.
inline constexpr std::array<std::uint8_t, 5> kReqPortConfig = detail::bare(MessageId::PortConfig);

inline constexpr std::array<std::uint8_t, 5> kReqConfiguration =
    detail::bare(MessageId::ReqConfiguration);

inline constexpr std::array<std::uint8_t, 5> kRunSelfTest = detail::bare(MessageId::RunSelfTest);

// ---------------------------------------------------------------------------
// The output configuration.
// ---------------------------------------------------------------------------

// One row of a SetOutputConfiguration payload: an identifier and how often to
// send it. LLCP Table 16.
struct OutputEntry
{
    // The identifier INCLUDING its format nibble -- asking for 0x4020 and
    // asking for 0x4022 are different requests, and the device echoes back
    // whichever it accepted.
    std::uint16_t rawId { 0 };

    // Hertz. Either of the two special values asks the device for the fastest
    // it can manage, and it reports what that turned out to be.
    std::uint16_t frequencyHz { 0 };

    constexpr bool operator==(const OutputEntry&) const = default;
};

// "The maximum frequency for the given data identifier." 0x0000 means the same
// thing; the device answers with 0xFFFF for both, and also forces it on the
// timestamp and status identifiers, whose output frequency it ignores because
// they accompany every packet.
inline constexpr std::uint16_t kMaxFrequency = 0xFFFF;

// LLCP: "a list of maximum 32 data identifiers".
inline constexpr std::size_t kMaxOutputEntries = 32;

inline constexpr std::size_t kOutputEntrySize = 4;

// True for the identifiers whose frequency the device ignores. Setting one to
// anything but kMaxFrequency is not an error, but the device will answer with
// kMaxFrequency, and a configuration check that did not know this would report
// drift on every pass and rewrite the configuration forever.
constexpr bool frequency_is_ignored(std::uint16_t rawId)
{
    const DataGroup group = data_group(rawId);
    return group == DataGroup::Timestamp || group == DataGroup::Status;
}

// Encode a SetOutputConfiguration payload. Runtime rather than constexpr
// because the list comes from YAML, and because a non-empty payload is what
// makes this a Set rather than the Req that shares its message id.
//
// Writes into `out` and returns how many bytes were used; the caller owns the
// buffer and this never allocates.
constexpr Result<std::size_t> encode_output_config(std::span<const OutputEntry> entries,
                                                   std::span<std::uint8_t> out)
{
    if (entries.size() > kMaxOutputEntries)
    {
        return too_long(static_cast<std::uint16_t>(entries.size()));
    }

    const std::size_t need = entries.size() * kOutputEntrySize;
    if (out.size() < need)
    {
        return truncated(static_cast<std::uint16_t>(out.size()));
    }

    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        write_u16(out, i * kOutputEntrySize, entries[i].rawId);
        write_u16(out, i * kOutputEntrySize + 2, entries[i].frequencyHz);
    }

    return need;
}

// Read one back. `index` is the row, and the caller checks the count first.
constexpr Result<OutputEntry> parse_output_entry(std::span<const std::uint8_t> payload,
                                                 std::size_t index)
{
    const std::size_t at = index * kOutputEntrySize;
    if (payload.size() < at + kOutputEntrySize)
    {
        return truncated(static_cast<std::uint16_t>(payload.size()));
    }

    return OutputEntry { read_u16(payload, at), read_u16(payload, at + 2) };
}

// How many rows a ReqOutputConfigurationAck payload holds. A payload whose
// length is not a multiple of four means the reply was misread, so it counts
// as zero rather than as a truncated last row.
constexpr Result<std::size_t> output_entry_count(std::span<const std::uint8_t> payload)
{
    if ((payload.size() % kOutputEntrySize) != 0)
    {
        return length_mismatch(0, static_cast<std::uint16_t>(payload.size()));
    }

    return payload.size() / kOutputEntrySize;
}

// ---------------------------------------------------------------------------
// Option flags.
// ---------------------------------------------------------------------------

// LLCP Table 9. Only the flags the table marks as applying to the 600-series
// are named; the rest exist on other products and setting one here would be a
// request this device has no meaning for.
enum class OptionFlag : std::uint32_t
{
    // Use the BusId stored in the eMTS for all XBus communication rather than
    // answering on the master id.
    EnableConfigurableBusId = 0x00000040,
    // In-run Compass Calibration: compensates for magnetic disturbances that
    // move with the object.
    EnableInRunCompassCalibration = 0x00000080,
};

// SetOptionFlags. Two 32-bit words: bits to set, then bits to clear. A zero in
// both leaves a flag alone, which is why this takes two masks rather than one
// value -- there is no way to write the whole word, only to move named bits.
constexpr std::array<std::uint8_t, 13> set_option_flags(std::uint32_t setMask,
                                                        std::uint32_t clearMask)
{
    std::array<std::uint8_t, 8> payload {};
    write_u32(payload, 0, setMask);
    write_u32(payload, 4, clearMask);
    return make_message(MessageId::OptionFlags, payload);
}

// ---------------------------------------------------------------------------
// Replies.
// ---------------------------------------------------------------------------

// The device id from MID 0x01.
//
// EIGHT BYTES ON THE 600-SERIES, four on the 1/10/100-series. The LLCP
// documents the long form as four zero bytes followed by the four-byte id,
// which means a parser that read the first four bytes of a 600-series reply
// would return zero and look like a device that had not answered.
struct DeviceIdReply
{
    std::uint64_t deviceId { 0 };

    static constexpr Result<DeviceIdReply> parse(std::span<const std::uint8_t> payload)
    {
        if (payload.size() == 8) { return DeviceIdReply { read_u64(payload, 0) }; }
        if (payload.size() == 4) { return DeviceIdReply { read_u32(payload, 0) }; }
        return length_mismatch(static_cast<std::uint8_t>(MessageId::DeviceId),
                               static_cast<std::uint16_t>(payload.size()));
    }
};

// MID 0x13, LLCP Table 4.
struct FirmwareRevision
{
    std::uint8_t major { 0 };
    std::uint8_t minor { 0 };
    std::uint8_t revision { 0 };
    std::uint32_t buildNumber { 0 };
    std::uint32_t scmReference { 0 };

    static constexpr std::size_t kSize = 11;

    static constexpr Result<FirmwareRevision> parse(std::span<const std::uint8_t> payload)
    {
        if (payload.size() < kSize)
        {
            return truncated(static_cast<std::uint16_t>(payload.size()),
                             static_cast<std::uint8_t>(MessageId::FirmwareRevision));
        }

        return FirmwareRevision {
            .major = read_u8(payload, 0),
            .minor = read_u8(payload, 1),
            .revision = read_u8(payload, 2),
            .buildNumber = read_u32(payload, 3),
            .scmReference = read_u32(payload, 7),
        };
    }
};

// MID 0x1F.
struct HardwareVersion
{
    std::uint8_t major { 0 };
    std::uint8_t minor { 0 };

    static constexpr Result<HardwareVersion> parse(std::span<const std::uint8_t> payload)
    {
        if (payload.size() < 2)
        {
            return truncated(static_cast<std::uint16_t>(payload.size()),
                             static_cast<std::uint8_t>(MessageId::HardwareVersion));
        }
        return HardwareVersion { read_u8(payload, 0), read_u8(payload, 1) };
    }
};

// MID 0x1D. ASCII, up to 20 bytes, e.g. "MTi-610R-2A5G4". The device pads with
// spaces rather than NULs on some products, so both are trimmed.
constexpr std::string_view parse_product_code(std::span<const std::uint8_t> payload)
{
    std::size_t end = payload.size();
    while (end > 0 && (payload[end - 1] == ' ' || payload[end - 1] == '\0'))
    {
        --end;
    }

    return std::string_view(reinterpret_cast<const char*>(payload.data()), end);
}

// MID 0x42. The first byte is the error code; a DeviceError of 0x28 carries
// five further bytes of device detail, which are passed through rather than
// decoded because the LLCP does not document them.
struct ErrorReply
{
    std::uint8_t code { 0 };
    std::span<const std::uint8_t> detail;

    constexpr DeviceError deviceError() const { return static_cast<DeviceError>(code); }

    static constexpr Result<ErrorReply> parse(std::span<const std::uint8_t> payload)
    {
        if (payload.empty())
        {
            return truncated(0, static_cast<std::uint8_t>(MessageId::ErrorReport));
        }

        return ErrorReply { payload[0], payload.subspan(1) };
    }
};

} // namespace xbus

#endif // XBUS_COMMANDS_H
