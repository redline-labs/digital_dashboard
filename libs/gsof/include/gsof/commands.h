// SPDX-License-Identifier: GPL-3.0-or-later
//
// Telling the receiver what to output, and reading back what it is already
// outputting.
//
// Configuration rides on APPFILE (0x64), which is a command AND a report: the
// same packet type, the same transport header, the same record framing in both
// directions. Send one to change settings; send GETAPPFILE (0x65) and the
// receiver sends one back describing what it has. That symmetry is what makes
// read-before-write possible, and read-before-write is what stops the node
// rewriting a configuration that was already correct.
//
// An application file's payload is a four-byte file control block followed by
// TLV records:
//
//     VERSION(03h) | DEVICE TYPE | START FLAG | FACTORY FLAG | records...
//
// The record this library cares about is 07h, Output Message: one per message
// the receiver emits, naming the port, the rate and -- for GSOF -- which
// record.
//
// Everything that builds bytes here is constexpr, so a command can be a
// `constexpr auto` whose encoding is checked against the ICD's tables by
// static_assert. The alternative is discovering a wrong constant when a
// receiver on a bench declines to do anything, which is a much slower way to
// read a table.
//
// Reference: receiverhelp.trimble.com/oem-gnss/, "Command 64h, APPFILE",
// "Output message record", "File control information block",
// "Command 65h, GETAPPFILE".

#ifndef GSOF_COMMANDS_H
#define GSOF_COMMANDS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "gsof/byte_order.h"
#include "gsof/error.h"
#include "gsof/records.h"
#include "gsof/tlv.h"
#include "gsof/transport.h"
#include "gsof/trimcomm.h"

namespace gsof::appfile
{

// ============================================================================
// The file control block
// ============================================================================

inline constexpr std::size_t kFileControlSize = 4;

// "Always 3 for this version of the specification."
inline constexpr std::uint8_t kSpecVersion = 0x03;

struct FileControl
{
    std::uint8_t specVersion { kSpecVersion };

    // Identifies the receiver family. The ICD does not publish the value for
    // any particular model, so DO NOT GUESS IT: read an application file back
    // with GETAPPFILE and echo whatever the receiver reported. That is the
    // whole reason ControlClient reads before it writes, and it means this
    // field needs no per-model table.
    std::uint8_t deviceType { 0 };

    // Apply the file as soon as the transfer completes, rather than merely
    // storing it.
    bool startImmediately { true };

    // Reset to factory defaults before applying the records. Almost always
    // wrong for us: the node is changing a few output messages, not taking
    // over the receiver, and a factory reset would drop the very IP socket
    // configuration that lets us talk to it.
    bool resetToFactoryFirst { false };

    constexpr std::array<std::uint8_t, kFileControlSize> encode() const
    {
        return {
            specVersion,
            deviceType,
            static_cast<std::uint8_t>(startImmediately ? 1 : 0),
            static_cast<std::uint8_t>(resetToFactoryFirst ? 1 : 0),
        };
    }

    static constexpr Result<FileControl> parse(std::span<const std::uint8_t> b)
    {
        if (b.size() < kFileControlSize)
        {
            return truncated(static_cast<std::uint16_t>(b.size()));
        }

        return FileControl { b[0], b[1], b[2] != 0, b[3] != 0 };
    }
};

// ============================================================================
// Output message records
// ============================================================================

// Application file record types. Only the one we build is named; the rest are
// listed so a configuration read back from a receiver can be reported by name
// rather than as a number, and so it is obvious what is being left alone.
enum class AppRecordType : std::uint8_t
{
    OutputMessage = 0x07,
    Antenna = 0x08,
};

// What kind of message a port emits. From the output message type table.
enum class OutputType : std::uint8_t
{
    // Turns this port's output off. Not the same as AllOff, which turns off
    // every output on the port in one record.
    Off = 0,
    Cmr = 2,
    Rtcm = 3,
    Rt17 = 4,
    NmeaGga = 6,
    Gsof = 10,
    Pps = 11,
    NmeaGsa = 38,
    NmeaRmc = 40,
    AllOff = 0xFF,
};

constexpr const char* to_string(OutputType type)
{
    switch (type)
    {
        case OutputType::Off:     return "off";
        case OutputType::Cmr:     return "CMR";
        case OutputType::Rtcm:    return "RTCM";
        case OutputType::Rt17:    return "RT17";
        case OutputType::NmeaGga: return "NMEA GGA";
        case OutputType::Gsof:    return "GSOF";
        case OutputType::Pps:     return "1PPS";
        case OutputType::NmeaGsa: return "NMEA GSA";
        case OutputType::NmeaRmc: return "NMEA RMC";
        case OutputType::AllOff:  return "all off";
    }

    // The table has ~30 NMEA-2000 entries we do not name. Reached for those.
    return "other";
}

// Zero-based port index. The ICD numbers ports from one in its prose and from
// zero in the byte, which is exactly the kind of detail that costs an
// afternoon, so only the byte value appears here.
enum class PortIndex : std::uint8_t
{
    Serial1 = 0,
    Serial2 = 1,
    Serial3 = 2,
    Serial4 = 3,
    Multiplexed = 4,
    PulsePerSecond = 5,
    Usb = 15,
    Bluetooth1 = 17,
    Bluetooth2 = 18,
    Bluetooth3 = 19,
    // The ones that matter here: an IP socket configured as a TCP server is
    // what the node connects to.
    IpSocket1 = 20,
    IpSocket2 = 21,
    IpSocket3 = 22,
    IpSocket4 = 23,
    IpSocket5 = 24,
    IpSocket6 = 25,
    IpSocket7 = 26,
    IpSocket8 = 27,
    IpSocket9 = 28,
    IpSocket10 = 29,
    NtripServer = 31,
    NtripCaster1 = 32,
    NtripCaster2 = 33,
    NtripCaster3 = 34,
    UsbToSerial = 43,
};

// The complete frequency table. Note that it is NOT ordered by rate and NOT
// contiguous: 0Bh (2 Hz) sits between 10 minutes and 15 seconds, and 0Eh is
// absent. Anything that treats these as ordinals is wrong.
enum class Frequency : std::uint8_t
{
    Off = 0x00,
    Hz10 = 0x01,
    Hz5 = 0x02,
    Hz1 = 0x03,
    Every2Seconds = 0x04,
    Every5Seconds = 0x05,
    Every10Seconds = 0x06,
    Every30Seconds = 0x07,
    Every60Seconds = 0x08,
    Every5Minutes = 0x09,
    Every10Minutes = 0x0A,
    Hz2 = 0x0B,
    Every15Seconds = 0x0C,
    Hz20 = 0x0D,
    Hz50 = 0x0F,
    Hz100 = 0x10,
    OnceImmediately = 0xFF,
};

constexpr const char* to_string(Frequency rate)
{
    switch (rate)
    {
        case Frequency::Off:             return "off";
        case Frequency::Hz10:            return "10hz";
        case Frequency::Hz5:             return "5hz";
        case Frequency::Hz1:             return "1hz";
        case Frequency::Every2Seconds:   return "2s";
        case Frequency::Every5Seconds:   return "5s";
        case Frequency::Every10Seconds:  return "10s";
        case Frequency::Every30Seconds:  return "30s";
        case Frequency::Every60Seconds:  return "60s";
        case Frequency::Every5Minutes:   return "5min";
        case Frequency::Every10Minutes:  return "10min";
        case Frequency::Hz2:             return "2hz";
        case Frequency::Every15Seconds:  return "15s";
        case Frequency::Hz20:            return "20hz";
        case Frequency::Hz50:            return "50hz";
        case Frequency::Hz100:           return "100hz";
        case Frequency::OnceImmediately: return "once";
    }

    // 0Eh, and anything a later firmware adds.
    return "unknown";
}

// True when `value` is a frequency this build knows. Used at config load, so a
// typo in YAML is refused there rather than sent to a receiver.
constexpr bool is_known_frequency(std::uint8_t value)
{
    switch (static_cast<Frequency>(value))
    {
        case Frequency::Off:
        case Frequency::Hz10:
        case Frequency::Hz5:
        case Frequency::Hz1:
        case Frequency::Every2Seconds:
        case Frequency::Every5Seconds:
        case Frequency::Every10Seconds:
        case Frequency::Every30Seconds:
        case Frequency::Every60Seconds:
        case Frequency::Every5Minutes:
        case Frequency::Every10Minutes:
        case Frequency::Hz2:
        case Frequency::Every15Seconds:
        case Frequency::Hz20:
        case Frequency::Hz50:
        case Frequency::Hz100:
        case Frequency::OnceImmediately:
            return true;
    }

    return false;
}

// One output message record, decoded. This is the unit the node diffs: what
// the receiver is doing versus what the YAML asks for.
struct OutputMessage
{
    OutputType outputType { OutputType::Off };
    PortIndex port { PortIndex::IpSocket1 };
    Frequency rate { Frequency::Off };
    std::uint8_t offsetSeconds { 0 };

    // Only meaningful when outputType is Gsof.
    bool isGsof { false };
    std::uint8_t gsofRecordType { 0 };
    std::uint8_t gsofOffsetSeconds { 0 };

    // A GSOF output is identified by (port, record). Two records for the same
    // pair are the same output at possibly different rates, which is exactly
    // the drift the node exists to notice.
    constexpr bool sameOutputAs(const OutputMessage& other) const
    {
        if (outputType != other.outputType || port != other.port)
        {
            return false;
        }
        return !isGsof || gsofRecordType == other.gsofRecordType;
    }
};

// The record byte counts, excluding the two TLV header bytes. The ICD lists
// 04h, 05h and 06h; GSOF uses the longest.
inline constexpr std::uint8_t kOutputMessageLengthSimple = 4;
inline constexpr std::uint8_t kOutputMessageLengthGsof = 6;

inline constexpr std::size_t kOutputMessageRecordSizeSimple = kOutputMessageLengthSimple + kTlvHeaderSize;
inline constexpr std::size_t kOutputMessageRecordSizeGsof = kOutputMessageLengthGsof + kTlvHeaderSize;

// Build an output message record asking for one GSOF record on one port.
constexpr std::array<std::uint8_t, kOutputMessageRecordSizeGsof> gsof_output_record(
    PortIndex port, Frequency rate, RecordType record, std::uint8_t offsetSeconds = 0,
    std::uint8_t gsofOffsetSeconds = 0)
{
    return {
        static_cast<std::uint8_t>(AppRecordType::OutputMessage),
        kOutputMessageLengthGsof,
        static_cast<std::uint8_t>(OutputType::Gsof),
        static_cast<std::uint8_t>(port),
        static_cast<std::uint8_t>(rate),
        offsetSeconds,
        static_cast<std::uint8_t>(record),
        gsofOffsetSeconds,
    };
}

// The four-byte form, for output types that carry no sub-record.
constexpr std::array<std::uint8_t, kOutputMessageRecordSizeSimple> simple_output_record(
    OutputType type, PortIndex port, Frequency rate, std::uint8_t offsetSeconds = 0)
{
    return {
        static_cast<std::uint8_t>(AppRecordType::OutputMessage),
        kOutputMessageLengthSimple,
        static_cast<std::uint8_t>(type),
        static_cast<std::uint8_t>(port),
        static_cast<std::uint8_t>(rate),
        offsetSeconds,
    };
}

// Stop one GSOF record on one port. Turning an output off is the same record
// with the rate set to Off -- there is no separate "remove" -- which is why
// the node's write plan is uniform whether it is adding, retiming or removing.
constexpr std::array<std::uint8_t, kOutputMessageRecordSizeGsof> gsof_output_off(PortIndex port, RecordType record)
{
    return gsof_output_record(port, Frequency::Off, record);
}

// Stop EVERYTHING on a port, in one record. Blunt, and the node only issues it
// under the exclusive port policy.
constexpr std::array<std::uint8_t, kOutputMessageRecordSizeSimple> all_output_off(PortIndex port)
{
    return simple_output_record(OutputType::AllOff, port, Frequency::Off);
}

// Decode one output message record's BODY (the bytes after type and length).
constexpr Result<OutputMessage> parse_output_message(std::span<const std::uint8_t> body)
{
    if (body.size() < kOutputMessageLengthSimple)
    {
        return truncated(static_cast<std::uint16_t>(body.size()),
                         static_cast<std::uint8_t>(AppRecordType::OutputMessage));
    }

    OutputMessage out {};
    out.outputType = static_cast<OutputType>(body[0]);
    out.port = static_cast<PortIndex>(body[1]);
    out.rate = static_cast<Frequency>(body[2]);
    out.offsetSeconds = body[3];

    if (out.outputType == OutputType::Gsof)
    {
        if (body.size() < kOutputMessageLengthGsof)
        {
            // A GSOF output record without its sub-record names no record, so
            // there is nothing to compare against a desired configuration.
            return length_mismatch(static_cast<std::uint8_t>(AppRecordType::OutputMessage),
                                   static_cast<std::uint16_t>(body.size()));
        }

        out.isGsof = true;
        out.gsofRecordType = body[4];
        out.gsofOffsetSeconds = body[5];
    }

    return out;
}

// ============================================================================
// Commands
// ============================================================================

// GETAPPFILE: ask the receiver for a stored application file.
//
// Index 0 is documented as the factory defaults. Which index holds the RUNNING
// configuration is not documented, which is why the node makes it a config
// field and `--probe` walks the indices reporting what each one contains.
constexpr std::array<std::uint8_t, 2 + trimcomm::kOverheadSize> get_application_file(std::uint16_t fileIndex)
{
    std::array<std::uint8_t, 2> data {};
    write_u16(data, 0, fileIndex);
    return trimcomm::make_packet(trimcomm::PacketType::GetAppFile, data);
}

// GETOPT: ask which receiver options are installed. Page 0, 1 or 2.
constexpr std::array<std::uint8_t, 1 + trimcomm::kOverheadSize> get_options(std::uint8_t page = 0)
{
    return trimcomm::make_packet(trimcomm::PacketType::GetOpt, std::array<std::uint8_t, 1> { page });
}

// Build a single-page APPFILE command around a payload of records.
//
// Single page only, deliberately. A page holds 252 record bytes, which is 31
// GSOF output records -- more than any sane configuration -- and the multi-page
// send path would be code with no way to reach it from this node. If a
// configuration ever needs more, it is sent as several APPFILE commands, each
// of which the receiver acknowledges separately: strictly better than one
// transfer where a lost middle page fails the lot.
//
// The three transport bytes are transmission number, page 0, max page 0.
constexpr std::size_t kMaxAppFileRecordBytes =
    trimcomm::kMaxDataSize - kTransportHeaderSize - kFileControlSize;

constexpr Result<std::size_t> encode_application_file(const FileControl& control,
                                                      std::span<const std::uint8_t> records,
                                                      std::uint8_t transmissionNumber,
                                                      std::span<std::uint8_t> out)
{
    if (records.size() > kMaxAppFileRecordBytes)
    {
        return too_long(static_cast<std::uint16_t>(records.size()));
    }

    std::array<std::uint8_t, trimcomm::kMaxDataSize> data {};
    std::size_t at = 0;

    data[at++] = transmissionNumber;
    data[at++] = 0;  // page index
    data[at++] = 0;  // max page index

    for (const std::uint8_t byte : control.encode())
    {
        data[at++] = byte;
    }
    for (const std::uint8_t byte : records)
    {
        data[at++] = byte;
    }

    return trimcomm::encode_packet(trimcomm::PacketType::AppFile,
                                   std::span<const std::uint8_t>(data.data(), at), out);
}

// ============================================================================
// Reading a configuration back
// ============================================================================

// What a receiver told us it is doing. Bounded rather than a vector so the
// whole thing stays usable in a constant expression, and because a page cannot
// hold more than this anyway.
inline constexpr std::size_t kMaxOutputMessages = kMaxAppFileRecordBytes / kOutputMessageRecordSizeGsof;

struct ApplicationFile
{
    FileControl control {};

    std::size_t outputCount { 0 };
    std::array<OutputMessage, kMaxOutputMessages> outputs {};

    // Application file records that are not output messages -- antenna,
    // reference station, and so on. Counted, not decoded: the node has no
    // business rewriting them, but "the receiver has 6 settings we did not
    // touch" is worth reporting.
    std::size_t otherRecordCount { 0 };

    constexpr std::span<const OutputMessage> view() const
    {
        return std::span<const OutputMessage>(outputs.data(), outputCount);
    }
};

// Decode a reassembled APPFILE payload -- the bytes AFTER the three transport
// header bytes have been stripped by the page assembler.
//
// A record that fails to decode is counted as "other" rather than failing the
// whole file. The reason is the same one that makes the TLV walk skip unknown
// types: a receiver whose configuration contains one setting this build cannot
// read must still be able to report the outputs it does have, or the node
// cannot diff anything at all.
constexpr Result<ApplicationFile> parse_application_file(std::span<const std::uint8_t> payload)
{
    const Result<FileControl> control = FileControl::parse(payload);
    if (!control.has_value())
    {
        return std::unexpected(control.error());
    }

    ApplicationFile out {};
    out.control = *control;

    TlvIterator it(payload.subspan(kFileControlSize));

    while (!it.done())
    {
        const Result<TlvRecord> record = it.next();
        if (!record.has_value())
        {
            return std::unexpected(record.error());
        }

        if (record->type != static_cast<std::uint8_t>(AppRecordType::OutputMessage))
        {
            ++out.otherRecordCount;
            continue;
        }

        const Result<OutputMessage> message = parse_output_message(record->body);
        if (!message.has_value())
        {
            ++out.otherRecordCount;
            continue;
        }

        if (out.outputCount >= kMaxOutputMessages)
        {
            return too_long(static_cast<std::uint16_t>(out.outputCount));
        }

        out.outputs[out.outputCount++] = *message;
    }

    return out;
}

} // namespace gsof::appfile

#endif // GSOF_COMMANDS_H
