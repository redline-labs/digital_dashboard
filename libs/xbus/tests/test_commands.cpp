// SPDX-License-Identifier: GPL-3.0-or-later
//
// The commands this tree sends to an MTi-610, and the replies it reads back.
//
// READ THIS BEFORE TRUSTING ANYTHING BELOW. No MTi has ever seen these bytes.
// libs/gsof's command test carries the same warning and it was earned: its
// record parsers were validated against captures while its command encodings
// were not, and no port on the BD992 accepted one. The difference in this file
// is what the assertions rest on:
//
//   * The FRAMING of every command is checked against five complete messages
//     Xsens printed in the LLCP, checksum included. Four carry no payload or
//     two bytes; the fifth carries eight. Every command here is built by the
//     same make_message() that reproduces all five byte for byte.
//
//   * The PAYLOAD CONTENT of the two commands that have one --
//     SetOutputConfiguration and SetOptionFlags -- rests on the LLCP's tables
//     alone. That is exactly the class of claim that was wrong for the BD992,
//     and docs/mti610.md lists it as deferred to hardware.

#include "xbus/commands.h"

#include "golden/golden_messages.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

using namespace xbus;

template <std::size_t N>
constexpr std::span<const std::uint8_t> bytes(const std::array<std::uint8_t, N>& a)
{
    return std::span<const std::uint8_t>(a.data(), a.size());
}

// ============================================================================
// Compile-time: the payload-carrying vendor vector
//
// The strongest statement available about a command with a payload: our
// encoder, given the flags the LLCP's example sets, must produce the LLCP's
// bytes exactly -- header, payload order, and checksum.
// ============================================================================

static_assert(set_option_flags(0x00000010u, 0x00000000u) == golden::kSetOptionFlagsEnableAhs,
              "SetOptionFlags must reproduce the LLCP's own printed message");

// SetFlags comes first, ClearFlags second. Reversing them would leave the
// vector above passing for the LLCP's example, whose ClearFlags is zero -- so
// the order needs its own assertion against the OTHER example in Table 9,
// "FA FF 48 08 00 00 00 01 00 00 00 02 CS", which sets DisableAutoStore and
// clears DisableAutoMeasurement.
constexpr std::array<std::uint8_t, 13> kSetOneClearAnother = set_option_flags(0x1u, 0x2u);
static_assert(kSetOneClearAnother[7] == 0x01, "SetFlags occupies the first four payload bytes");
static_assert(kSetOneClearAnother[11] == 0x02, "and ClearFlags the second four");

// ============================================================================
// Compile-time: two replies, against bytes a device actually sent
//
// These come from a sample .mtb log shipped in the Xsens SDK rather than from
// the LLCP, so nobody typeset them -- an Xsens device put them on a wire and
// Xsens' own software wrote them to a file. The device is a Bodypack, not an
// MTi, so the DATA is not something a 610 would send; what they settle is the
// two reply LAYOUTS, which is exactly what the LLCP's tables alone could not.
// ============================================================================

static_assert(parse_message(bytes(golden::kFirmwareRevReply)).has_value(),
              "a real FirmwareRev message must parse");

constexpr FirmwareRevision kRealFirmware =
    *FirmwareRevision::parse(parse_message(bytes(golden::kFirmwareRevReply))->data);

// LLCP Table 4: one byte each for major, minor and revision, then a four-byte
// build number, then a four-byte SCM reference. A wrong split here would give
// a plausible version string, which is why it is asserted field by field.
static_assert(kRealFirmware.major == 1);
static_assert(kRealFirmware.minor == 2);
static_assert(kRealFirmware.revision == 0);
static_assert(kRealFirmware.buildNumber == 2243,
              "the build number is four bytes at offset 3, big-endian");
static_assert(kRealFirmware.scmReference == 0x0001656Eu);

// The output-configuration entry layout, from a real acknowledgement. This is
// the one part of SetOutputConfiguration that is no longer a reading of a
// table -- see the note in golden_messages.h.
constexpr MessageView kRealAck = *parse_message(bytes(golden::kOutputConfigAckReply));
static_assert(kRealAck.is(MessageId::OutputConfigurationAck));
static_assert(*output_entry_count(kRealAck.data) == 1,
              "a four-byte payload is exactly one entry");

constexpr OutputEntry kRealEntry = *parse_output_entry(kRealAck.data, 0);
static_assert(kRealEntry.rawId == 0x10A0, "the identifier is big-endian, first");
static_assert(kRealEntry.frequencyHz == 1, "and the frequency big-endian, second");

// The same message re-encoded from the parsed entry must reproduce the
// device's own bytes. Encoder and decoder are checked against each other
// everywhere else; here they are both checked against a device.
constexpr std::array<std::uint8_t, 9> reencodeAck()
{
    std::array<std::uint8_t, 4> payload {};
    std::array<std::uint8_t, 9> out {};
    const std::array<OutputEntry, 1> entries { kRealEntry };

    if (!encode_output_config(entries, payload).has_value()) { return {}; }
    if (!encode_message(MessageId::OutputConfigurationAck, bytes(payload), out).has_value())
    {
        return {};
    }
    return out;
}

static_assert(reencodeAck() == golden::kOutputConfigAckReply,
              "re-encoding a real acknowledgement must reproduce it byte for byte");

// ============================================================================
// Compile-time: the zero-payload commands
// ============================================================================

// ReqDID is one of the five vendor vectors, so this is the one command whose
// bytes are confirmed outright.
static_assert(kReqDeviceId == golden::kReqDeviceId,
              "ReqDID must reproduce the LLCP's bytes exactly");

// The rest are the same construction with a different message id. Each is
// asserted whole rather than by its id byte, so a change to make_message()
// that happened to keep the id right still fails here.
static_assert(kGoToConfig == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x30, 0x00, 0xD1 });
static_assert(kGoToMeasurement == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x10, 0x00, 0xF1 });
static_assert(kReset == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x40, 0x00, 0xC1 });
static_assert(kWakeUpAck == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x3F, 0x00, 0xC2 });
static_assert(kReqProductCode == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x1C, 0x00, 0xE5 });
static_assert(kReqFirmwareRevision == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x12, 0x00, 0xEF });
static_assert(kReqHardwareVersion == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x1E, 0x00, 0xE3 });
static_assert(kReqOutputConfiguration == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0xC0, 0x00, 0x41 });
static_assert(kReqPortConfig == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x8C, 0x00, 0x75 });
static_assert(kReqConfiguration == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x0C, 0x00, 0xF5 });
static_assert(kRunSelfTest == std::array<std::uint8_t, 5> { 0xFA, 0xFF, 0x24, 0x00, 0xDD });

// Every one of them must parse back, which catches a checksum that is
// self-consistently wrong in the encoder.
static_assert(parse_message(bytes(kGoToMeasurement)).has_value());
static_assert(parse_message(bytes(kWakeUpAck))->data.empty(),
              "a command with no payload is the request form of its message id");

// ReqOutputConfiguration and SetOutputConfiguration share message id 0xC0, and
// the empty payload above is the entire difference. This is the trap that
// makes message_table.h a table.
static_assert(kReqOutputConfiguration[2] == static_cast<std::uint8_t>(MessageId::OutputConfiguration));
static_assert(describe_host_message(MessageId::OutputConfiguration,
                                    !parse_message(bytes(kReqOutputConfiguration))->data.empty()) ==
                  std::string_view("req_output_configuration"));

// ============================================================================
// Run time
// ============================================================================

void testOutputConfigRoundTrip()
{
    // A configuration close to what configs/mti610/mti610.yaml asks for.
    const std::vector<OutputEntry> wanted {
        { 0x1020, kMaxFrequency },  // packet counter
        { 0x1060, kMaxFrequency },  // sample time fine
        { 0xE020, kMaxFrequency },  // status word
        { 0x4022, 100 },            // acceleration, fp16.32
        { 0x8022, 100 },            // rate of turn, fp16.32
        { 0xC022, 100 },            // magnetic field, fp16.32
        { 0x0810, 1 },              // temperature
    };

    std::array<std::uint8_t, kMaxOutputEntries * kOutputEntrySize> payload {};
    const Result<std::size_t> used = encode_output_config(wanted, payload);

    check(used.has_value(), "the output configuration encodes");
    if (!used) { return; }

    check(*used == wanted.size() * 4, "four bytes per entry");

    const std::span<const std::uint8_t> encoded(payload.data(), *used);
    const Result<std::size_t> count = output_entry_count(encoded);
    check(count.has_value() && *count == wanted.size(), "and reads back as the same count");

    std::vector<OutputEntry> readBack;
    for (std::size_t i = 0; i < *count; ++i)
    {
        const Result<OutputEntry> entry = parse_output_entry(encoded, i);
        check(entry.has_value(), "entry " + std::to_string(i) + " parses");
        if (entry) { readBack.push_back(*entry); }
    }

    check(readBack == wanted, "and as the same entries, in the same order");

    // The format nibble survives. Asking for 0x4022 and asking for 0x4020 are
    // different requests, and an encoder that masked it off would silently
    // configure the device for float32 while the parser expected fp16.32 --
    // which decodes, and is wrong.
    check(readBack.size() > 3 && readBack[3].rawId == 0x4022,
          "the format nibble is part of the request, not decoration");
}

void testOutputConfigLimits()
{
    // "A list of maximum 32 data identifiers."
    const std::vector<OutputEntry> tooMany(kMaxOutputEntries + 1, OutputEntry { 0x4020, 100 });
    std::array<std::uint8_t, 256> payload {};
    check(!encode_output_config(tooMany, payload).has_value(), "33 entries are refused");

    const std::vector<OutputEntry> exactly(kMaxOutputEntries, OutputEntry { 0x4020, 100 });
    check(encode_output_config(exactly, payload).has_value(), "32 are accepted");

    // A buffer that cannot hold the result.
    std::array<std::uint8_t, 8> tiny {};
    check(!encode_output_config(exactly, tiny).has_value(), "a short buffer is refused");

    // A reply whose length is not a multiple of four was misread. Counting it
    // as seven-and-a-bit entries would hand the caller a truncated last row
    // that looks like a real configuration.
    const std::array<std::uint8_t, 6> ragged {};
    check(!output_entry_count(ragged).has_value(), "a ragged reply is refused, not rounded down");

    const std::array<std::uint8_t, 0> empty {};
    check(output_entry_count(empty).has_value() && *output_entry_count(empty) == 0,
          "an empty reply is a device configured to output nothing, which is legal");
}

void testFrequencyIsIgnoredForPacketMetadata()
{
    // The LLCP: for data sent with every packet, "the Output Frequency will be
    // ignored and will be set to 0xFFFF". A configuration check that did not
    // know this would see drift on every pass and rewrite the configuration
    // forever, which is a loop that only shows up against real hardware.
    check(frequency_is_ignored(0x1020), "packet counter");
    check(frequency_is_ignored(0x1060), "sample time fine");
    check(frequency_is_ignored(0xE020), "status word");
    check(frequency_is_ignored(0xE010), "status byte");

    check(!frequency_is_ignored(0x4020), "acceleration has a real frequency");
    check(!frequency_is_ignored(0xC020), "and so does the magnetic field");
    check(!frequency_is_ignored(0x0810), "and the temperature");
}

void testDeviceIdWidth()
{
    // Eight bytes on a 600-series, four on the others. Reading the first four
    // of the long form returns zero, which looks like a device that did not
    // answer rather than like a parser that read the wrong half.
    const std::array<std::uint8_t, 8> longForm { 0x00, 0x00, 0x00, 0x00, 0x03, 0xE8, 0x12, 0x34 };
    const Result<DeviceIdReply> wide = DeviceIdReply::parse(longForm);
    check(wide.has_value() && wide->deviceId == 0x03E81234ull,
          "a 600-series device id is the LAST four bytes of eight");

    const std::array<std::uint8_t, 4> shortForm { 0x03, 0xE8, 0x12, 0x34 };
    const Result<DeviceIdReply> narrow = DeviceIdReply::parse(shortForm);
    check(narrow.has_value() && narrow->deviceId == 0x03E81234ull,
          "and a 1/10/100-series one is all four");

    const std::array<std::uint8_t, 6> neither {};
    check(!DeviceIdReply::parse(neither).has_value(), "any other length is refused");
}

void testIdentificationReplies()
{
    const std::array<std::uint8_t, 11> fw {
        0x01, 0x0A, 0x03,        // 1.10.3
        0x00, 0x00, 0x04, 0xD2,  // build 1234
        0xDE, 0xAD, 0xBE, 0xEF   // scm reference
    };
    const Result<FirmwareRevision> parsed = FirmwareRevision::parse(fw);
    check(parsed.has_value(), "firmware revision parses");
    if (parsed)
    {
        check(parsed->major == 1 && parsed->minor == 10 && parsed->revision == 3,
              "with major, minor and revision as single bytes");
        check(parsed->buildNumber == 1234, "and a four-byte build number at offset 3");
        check(parsed->scmReference == 0xDEADBEEF, "and the SCM reference behind it");
    }

    const std::array<std::uint8_t, 10> shortFw {};
    check(!FirmwareRevision::parse(shortFw).has_value(), "ten bytes is not eleven");

    const std::array<std::uint8_t, 2> hw { 0x02, 0x01 };
    const Result<HardwareVersion> version = HardwareVersion::parse(hw);
    check(version.has_value() && version->major == 2 && version->minor == 1, "hardware version");

    // A product code, padded the way some products pad it.
    const std::array<std::uint8_t, 16> code {
        'M', 'T', 'i', '-', '6', '1', '0', 'R', '-', '2', 'A', '5', 'G', '4', ' ', ' '
    };
    check(parse_product_code(code) == "MTi-610R-2A5G4", "the product code is trimmed");
}

void testErrorReply()
{
    const std::array<std::uint8_t, 1> parameterInvalid { 0x21 };
    const Result<ErrorReply> simple = ErrorReply::parse(parameterInvalid);
    check(simple.has_value() && simple->deviceError() == DeviceError::ParameterInvalid,
          "an error reply carries its code");
    check(simple && simple->detail.empty(), "and usually nothing else");

    // 0x28 carries five further bytes the LLCP does not document. They are
    // passed through rather than decoded, because a decode of undocumented
    // bytes is a guess that reads as a fact in a log.
    const std::array<std::uint8_t, 6> deviceError { 0x28, 0x01, 0x02, 0x03, 0x04, 0x05 };
    const Result<ErrorReply> detailed = ErrorReply::parse(deviceError);
    check(detailed.has_value() && detailed->detail.size() == 5,
          "a device error passes its five detail bytes through");

    const std::array<std::uint8_t, 0> empty {};
    check(!ErrorReply::parse(empty).has_value(), "an empty error reply is itself an error");

    // The one error code that is almost always self-inflicted, and the reason
    // it is named: an output configuration the serial link cannot carry.
    check(std::string(to_string(DeviceError::TimerOverflow)).find("rate too high") !=
              std::string::npos,
          "timer overflow says what actually causes it");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testOutputConfigRoundTrip();
    testOutputConfigLimits();
    testFrequencyIsIgnoredForPacketMetadata();
    testDeviceIdWidth();
    testIdentificationReplies();
    testErrorReply();

    return failures == 0 ? 0 : 1;
}
