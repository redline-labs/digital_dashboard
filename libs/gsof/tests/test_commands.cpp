// SPDX-License-Identifier: GPL-3.0-or-later
//
// Building configuration commands, and reading a configuration back.
//
// A CAVEAT THAT MATTERS, and the same one the PCAN backend carries: NONE OF
// THESE BYTES HAVE BEEN SEEN BY A RECEIVER. The GSOF record parsers are
// checked against real captures; the command encodings are checked against the
// ICD's tables and against their own decoders, which is a weaker statement. If
// a BD992 on a bench ignores a configuration command, the constants here --
// the output type, the port index, the frequency byte, the file control block
// -- are the first thing to suspect, and `bd992 --probe` is the tool for it.
//
// What the tests below DO establish is that the encoder agrees with the ICD
// tables byte for byte, and that encode and decode are inverses. Those are
// exactly the mistakes that would otherwise be found by a silent no-op.

#include "gsof/commands.h"
#include "gsof/records.h"
#include "gsof/transport.h"
#include "gsof/trimcomm.h"

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

using namespace gsof;
using namespace gsof::appfile;

// ============================================================================
// The output message record, against the ICD's tables
// ============================================================================

// From "Output message record": RECORD TYPE 07h, RECORD LENGTH 06h for GSOF,
// OUTPUT MESSAGE TYPE 10 (0Ah) for GSOF, PORT INDEX 20 (14h) for the first IP
// socket, FREQUENCY 01h for 10 Hz, then the GSOF sub-message type and its
// offset. Every one of those is a lookup in a different table, so the whole
// eight bytes are written out rather than derived.
constexpr auto kLlhAt10Hz = gsof_output_record(PortIndex::IpSocket1, Frequency::Hz10, RecordType::LatLongHeight);

static_assert(kLlhAt10Hz == std::array<std::uint8_t, 8> { 0x07, 0x06, 0x0A, 0x14, 0x01, 0x00, 0x02, 0x00 },
              "the GSOF output message record, byte for byte from the ICD tables");

// The record type in the last-but-one byte is the GSOF record number, so this
// is also a check that RecordType's values are the wire values.
constexpr auto kAttitudeAt20Hz =
    gsof_output_record(PortIndex::IpSocket2, Frequency::Hz20, RecordType::AttitudeInfo);

static_assert(kAttitudeAt20Hz == std::array<std::uint8_t, 8> { 0x07, 0x06, 0x0A, 0x15, 0x0D, 0x00, 0x1B, 0x00 },
              "record 27 is 1Bh, IP socket 2 is 15h, 20 Hz is 0Dh");

// Turning one output off is the same record at rate zero. There is no separate
// removal command, which is why the node's write plan is uniform.
constexpr auto kLlhOff = gsof_output_off(PortIndex::IpSocket1, RecordType::LatLongHeight);

static_assert(kLlhOff[4] == 0x00, "rate Off");
static_assert(kLlhOff[6] == kLlhAt10Hz[6], "and it still names the same record");
static_assert(kLlhOff[3] == kLlhAt10Hz[3], "on the same port");

// The four-byte form, and the two different ways of saying "stop".
constexpr auto kEverythingOff = all_output_off(PortIndex::IpSocket1);

static_assert(kEverythingOff == std::array<std::uint8_t, 6> { 0x07, 0x04, 0xFF, 0x14, 0x00, 0x00 },
              "FFh turns off every output on the port");
static_assert(kEverythingOff[1] == kOutputMessageLengthSimple, "and is the short form of the record");

constexpr auto kNmeaGga = simple_output_record(OutputType::NmeaGga, PortIndex::Serial1, Frequency::Hz1);

static_assert(kNmeaGga == std::array<std::uint8_t, 6> { 0x07, 0x04, 0x06, 0x00, 0x03, 0x00 },
              "NMEA GGA is type 6, serial 1 is port index 0, 1 Hz is 03h");

// ============================================================================
// The frequency table is not ordered and not contiguous
// ============================================================================

// If anyone ever "tidies" this enum into rate order, these stop compiling.
static_assert(static_cast<std::uint8_t>(Frequency::Hz2) == 0x0B,
              "2 Hz sits between 10 minutes and 15 seconds in the table");
static_assert(static_cast<std::uint8_t>(Frequency::Every10Minutes) == 0x0A);
static_assert(static_cast<std::uint8_t>(Frequency::Every15Seconds) == 0x0C);
static_assert(static_cast<std::uint8_t>(Frequency::Hz10) < static_cast<std::uint8_t>(Frequency::Hz1),
              "a faster rate has a SMALLER byte -- the values are not ordinals");

static_assert(is_known_frequency(0x0D), "20 Hz");
static_assert(!is_known_frequency(0x0E), "0Eh is absent from the table");
static_assert(!is_known_frequency(0x11), "and so is everything above 100 Hz up to FFh");
static_assert(is_known_frequency(0xFF), "once-only");

// ============================================================================
// Commands
// ============================================================================

// From "Command 65h, GETAPPFILE": STX, STATUS, 65h, LENGTH, then the system
// file index as a big-endian short, checksum, ETX.
constexpr auto kGetAppFile = get_application_file(1);

static_assert(kGetAppFile.size() == 8);
static_assert(kGetAppFile[0] == trimcomm::kStx);
static_assert(kGetAppFile[2] == 0x65);
static_assert(kGetAppFile[3] == 0x02, "two data bytes");
static_assert(kGetAppFile[4] == 0x00 && kGetAppFile[5] == 0x01, "the index is big-endian, like everything else");
static_assert(kGetAppFile[7] == trimcomm::kEtx);
static_assert(trimcomm::parse_packet(kGetAppFile).has_value(), "and the checksum is right");

// From "Command 4Ah, GETOPT": one data byte, the options page.
constexpr auto kGetOpt = get_options(0);

static_assert(kGetOpt.size() == 7);
static_assert(kGetOpt[2] == 0x4A);
static_assert(kGetOpt[3] == 0x01 && kGetOpt[4] == 0x00);
static_assert(trimcomm::parse_packet(kGetOpt).has_value());

// ============================================================================
// The file control block
// ============================================================================

static_assert(FileControl {}.encode() == std::array<std::uint8_t, 4> { 0x03, 0x00, 0x01, 0x00 },
              "spec version 3, device type echoed (0 until read back), start on, no factory reset");

// The default MUST NOT reset to factory. A factory reset would drop the IP
// socket configuration that the connection carrying the command is using.
static_assert(!FileControl {}.resetToFactoryFirst,
              "resetting to factory would drop the socket the command arrived on");
static_assert(FileControl {}.startImmediately);

static_assert(FileControl::parse(FileControl { 0x03, 0x2A, true, false }.encode())->deviceType == 0x2A,
              "the device type round-trips, which is what lets it be echoed rather than guessed");

// ============================================================================
// A whole APPFILE command, hand-computed
// ============================================================================

// data  = txNum 01, page 00, maxPage 00
//       | control 03 00 01 00
//       | record  07 06 0A 14 01 00 02 00
// LENGTH = 15 = 0Fh
// checksum = (00 + 64 + 0F + sum(data)) mod 256
//          = 0x64 + 0x0F + (0x01 + 0x03 + 0x01 + 0x07+0x06+0x0A+0x14+0x01+0x02)
//          = 0x64 + 0x0F + 0x33 = 0xA6
constexpr std::array<std::uint8_t, 21> kExpectedAppFile {
    0x02, 0x00, 0x64, 0x0F,
    0x01, 0x00, 0x00,
    0x03, 0x00, 0x01, 0x00,
    0x07, 0x06, 0x0A, 0x14, 0x01, 0x00, 0x02, 0x00,
    0xA6, 0x03,
};

constexpr std::array<std::uint8_t, 21> kBuiltAppFile = [] {
    std::array<std::uint8_t, 21> out {};
    const auto record = gsof_output_record(PortIndex::IpSocket1, Frequency::Hz10, RecordType::LatLongHeight);
    const Result<std::size_t> written = encode_application_file(FileControl {}, record, 0x01, out);
    // A failed encode leaves the array zeroed, which the comparison below
    // catches; there is nowhere to report it from inside a constant expression.
    return written.has_value() ? out : std::array<std::uint8_t, 21> {};
}();

static_assert(kBuiltAppFile == kExpectedAppFile, "a complete APPFILE command, hand-computed from the ICD");

// ============================================================================
// Run-time: round trips and malformed input
// ============================================================================

// Encode a set of desired outputs, then decode the packet the way the receiver
// would have to, and check nothing was lost on the way.
void test_application_file_round_trip()
{
    std::vector<std::uint8_t> records;
    const auto append = [&records](std::span<const std::uint8_t> bytes) {
        records.insert(records.end(), bytes.begin(), bytes.end());
    };

    append(gsof_output_record(PortIndex::IpSocket1, Frequency::Hz10, RecordType::LatLongHeight));
    append(gsof_output_record(PortIndex::IpSocket1, Frequency::Hz10, RecordType::Velocity));
    append(gsof_output_record(PortIndex::IpSocket1, Frequency::Hz1, RecordType::PositionType));
    append(simple_output_record(OutputType::NmeaGga, PortIndex::Serial1, Frequency::Hz1));

    std::array<std::uint8_t, trimcomm::kMaxPacketSize> packet {};
    const Result<std::size_t> written =
        encode_application_file(FileControl { kSpecVersion, 0x7B, true, false }, records, 0x2A, packet);

    check(written.has_value(), "the command encodes");
    if (!written.has_value())
    {
        return;
    }

    // Decode exactly as a receiver would: outer packet, transport header,
    // then the application file.
    const Result<trimcomm::PacketView> view =
        trimcomm::parse_packet(std::span<const std::uint8_t>(packet.data(), *written));
    check(view.has_value() && view->is(trimcomm::PacketType::AppFile), "and is a valid APPFILE packet");
    if (!view.has_value())
    {
        return;
    }

    PageAssembler assembler;
    const Result<PageAssembler::Feed> fed = assembler.feed(view->data);
    check(fed.has_value() && *fed == PageAssembler::Feed::Complete,
          "APPFILE uses the same transport header as GENOUT, so the same assembler completes it");
    if (!fed.has_value())
    {
        return;
    }
    check(assembler.header().transmissionNumber == 0x2A, "the transmission number survives");

    const Result<ApplicationFile> file = parse_application_file(assembler.payload());
    check(file.has_value(), "and the application file decodes");
    if (!file.has_value())
    {
        return;
    }

    check(file->control.deviceType == 0x7B, "the device type is carried through unchanged");
    check(file->control.startImmediately, "as are the flags");
    check(file->outputCount == 4, "all four output records come back");

    if (file->outputCount == 4)
    {
        check(file->outputs[0].isGsof && file->outputs[0].gsofRecordType == 2 &&
                  file->outputs[0].rate == Frequency::Hz10 && file->outputs[0].port == PortIndex::IpSocket1,
              "the first is lat/long/height at 10 Hz on IP socket 1");
        check(file->outputs[2].rate == Frequency::Hz1 && file->outputs[2].gsofRecordType == 38,
              "the third is position type at 1 Hz");
        check(!file->outputs[3].isGsof && file->outputs[3].outputType == OutputType::NmeaGga,
              "and the NMEA record is decoded as a four-byte record, not misread as GSOF");
    }
}

void test_a_configuration_too_large_for_one_page_is_refused()
{
    // Rather than being silently truncated, which would leave the receiver
    // configured with a prefix of what was asked for.
    const std::vector<std::uint8_t> records(kMaxAppFileRecordBytes + 1, 0x00);

    std::array<std::uint8_t, trimcomm::kMaxPacketSize> packet {};
    const Result<std::size_t> written = encode_application_file(FileControl {}, records, 0, packet);

    check(!written.has_value() && written.error().kind == ErrorKind::TooLong,
          "a configuration that does not fit one page is refused, not truncated");

    // And the largest that does fit is accepted, so the bound is not off by one.
    const std::vector<std::uint8_t> justFits(kMaxAppFileRecordBytes, 0x00);
    check(encode_application_file(FileControl {}, justFits, 0, packet).has_value(),
          "and the largest configuration that does fit is accepted");
}

void test_records_we_do_not_understand_are_counted_not_fatal()
{
    // A receiver's application file holds antenna settings, reference station
    // settings and more. The node has no business rewriting them, but it must
    // still be able to read the outputs alongside them -- otherwise a
    // configured receiver is one the node cannot diff at all.
    std::vector<std::uint8_t> payload;
    for (const std::uint8_t byte : FileControl {}.encode())
    {
        payload.push_back(byte);
    }

    // An antenna record (08h), which we deliberately do not decode.
    payload.push_back(0x08);
    payload.push_back(0x04);
    payload.insert(payload.end(), { 0x01, 0x02, 0x03, 0x04 });

    const auto output = gsof_output_record(PortIndex::IpSocket1, Frequency::Hz5, RecordType::AttitudeInfo);
    payload.insert(payload.end(), output.begin(), output.end());

    // A record type from a firmware newer than this build.
    payload.push_back(0x7E);
    payload.push_back(0x02);
    payload.insert(payload.end(), { 0xAA, 0xBB });

    const Result<ApplicationFile> file = parse_application_file(payload);

    check(file.has_value(), "an application file containing unfamiliar records still parses");
    if (!file.has_value())
    {
        return;
    }
    check(file->outputCount == 1, "the output message between them is found");
    check(file->outputs[0].gsofRecordType == 27 && file->outputs[0].rate == Frequency::Hz5,
          "and decoded correctly despite its neighbours");
    check(file->otherRecordCount == 2, "and the two records we do not model are counted, not dropped silently");
}

void test_malformed_output_messages()
{
    // Three bytes cannot be an output message record.
    const std::array<std::uint8_t, 3> tooShort { 0x0A, 0x14, 0x01 };
    const Result<OutputMessage> shortResult = parse_output_message(tooShort);
    check(!shortResult.has_value() && shortResult.error().kind == ErrorKind::Truncated,
          "an output message record shorter than four bytes is refused");

    // A GSOF output message whose sub-record is missing names no record, so
    // there is nothing to compare against a desired configuration.
    const std::array<std::uint8_t, 4> gsofWithoutSubRecord { 0x0A, 0x14, 0x01, 0x00 };
    const Result<OutputMessage> gsofResult = parse_output_message(gsofWithoutSubRecord);
    check(!gsofResult.has_value() && gsofResult.error().kind == ErrorKind::LengthMismatch,
          "a GSOF output message without its sub-record is refused rather than read as record 0");

    // A four-byte record for a non-GSOF type is complete and must NOT be
    // treated as truncated.
    const std::array<std::uint8_t, 4> nmea { 0x06, 0x00, 0x03, 0x00 };
    const Result<OutputMessage> nmeaResult = parse_output_message(nmea);
    check(nmeaResult.has_value() && !nmeaResult->isGsof,
          "a four-byte non-GSOF record is complete");

    // A file control block that is cut short.
    const std::array<std::uint8_t, 3> shortControl { 0x03, 0x00, 0x01 };
    check(!parse_application_file(shortControl).has_value(),
          "an application file too short for its control block is refused");
}

void test_same_output_identity()
{
    // The node diffs by (type, port, gsof record), NOT by rate -- otherwise a
    // rate change would read as "remove one output, add another" and the
    // receiver would be told to turn the message off and on again.
    const OutputMessage at10Hz { OutputType::Gsof, PortIndex::IpSocket1, Frequency::Hz10, 0, true, 2, 0 };
    const OutputMessage at1Hz { OutputType::Gsof, PortIndex::IpSocket1, Frequency::Hz1, 0, true, 2, 0 };
    const OutputMessage otherRecord { OutputType::Gsof, PortIndex::IpSocket1, Frequency::Hz10, 0, true, 8, 0 };
    const OutputMessage otherPort { OutputType::Gsof, PortIndex::IpSocket2, Frequency::Hz10, 0, true, 2, 0 };

    check(at10Hz.sameOutputAs(at1Hz), "the same GSOF record at a different rate is the same output");
    check(!at10Hz.sameOutputAs(otherRecord), "a different GSOF record is a different output");
    check(!at10Hz.sameOutputAs(otherPort), "and so is the same record on another port");

    // For a non-GSOF type the record number is meaningless and must not be
    // compared -- two NMEA GGA outputs on one port are the same output
    // whatever happens to be in the unused bytes.
    const OutputMessage gga1 { OutputType::NmeaGga, PortIndex::Serial1, Frequency::Hz1, 0, false, 0, 0 };
    const OutputMessage gga2 { OutputType::NmeaGga, PortIndex::Serial1, Frequency::Hz5, 0, false, 99, 0 };
    check(gga1.sameOutputAs(gga2), "non-GSOF outputs are identified by type and port alone");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    test_application_file_round_trip();
    test_a_configuration_too_large_for_one_page_is_refused();
    test_records_we_do_not_understand_are_counted_not_fatal();
    test_malformed_output_messages();
    test_same_output_identity();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all GSOF command checks passed");
    return 0;
}
