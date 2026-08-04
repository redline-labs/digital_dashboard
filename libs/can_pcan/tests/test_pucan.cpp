// SPDX-License-Identifier: GPL-3.0-or-later
//
// The PCAN-USB FD wire protocol, checked without a PCAN-USB FD.
//
// Everything here is byte-level: build a command and inspect what came out,
// hand-assemble a record and see what comes back. That covers the parts of a
// USB driver that are wrong silently -- a flag in the wrong bit, a length field
// counted from the wrong place, a DLC treated as a byte count -- and leaves only
// the transfers themselves needing hardware.
//
// A note on what a green run here does and does not mean. It means the codec
// does what this file says it should. It does not mean the opcodes match a real
// device: those come from the mainline Linux peak_usb driver's description of
// the protocol and have not been confirmed against hardware. If a dongle
// arrives and says nothing, suspect the constants before suspecting the
// framing.

#include "can_pcan/pucan.h"

#include "can/dlc.h"

#include <spdlog/spdlog.h>

#include <string>

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

std::string hex(std::span<const uint8_t> bytes)
{
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        out += fmt::format("{}{:02X}", i == 0 ? "" : " ", bytes[i]);
    }
    return out;
}

void put_u16(std::vector<uint8_t>& buffer, uint16_t value)
{
    buffer.push_back(static_cast<uint8_t>(value & 0xFF));
    buffer.push_back(static_cast<uint8_t>(value >> 8));
}

void put_u32(std::vector<uint8_t>& buffer, uint32_t value)
{
    put_u16(buffer, static_cast<uint16_t>(value & 0xFFFF));
    put_u16(buffer, static_cast<uint16_t>(value >> 16));
}

// ============================================================================
// Device identity
// ============================================================================

void test_product_table()
{
    check(can::pcan::channel_count_for_product(0x0012) == 1, "PCAN-USB FD has one channel");
    check(can::pcan::channel_count_for_product(0x0011) == 2, "PCAN-USB Pro FD has two");
    check(can::pcan::channel_count_for_product(0x0014) == 6, "PCAN-USB X6 has six");
    check(can::pcan::channel_count_for_product(0x000c) == 0,
          "the classic PCAN-USB is not one this backend drives, and says so rather than "
          "pretending it is a one-channel FD device");
    check(can::pcan::channel_count_for_product(0x9999) == 0, "an unknown product is not claimed");
}

// ============================================================================
// Commands
// ============================================================================

void test_opcode_channel_packing()
{
    using can::pcan::Opcode;

    // Opcode in the low ten bits, channel in bits 12..15. This is the field
    // that routes a command to one controller of a six-channel device, so a
    // shift in the wrong direction means every command lands on channel 0.
    check(can::pcan::opcode_channel(0, Opcode::NormalMode) == 0x0002,
          "channel 0 normal-mode is 0x0002");
    check(can::pcan::opcode_channel(1, Opcode::NormalMode) == 0x1002,
          "channel 1 normal-mode is 0x1002");
    check(can::pcan::opcode_channel(5, Opcode::ResetMode) == 0x5001,
          "channel 5 reset-mode is 0x5001");
    check(can::pcan::opcode_channel(0, Opcode::EndOfCollection) == 0x03FF,
          "end-of-collection is 0x03FF and does not overflow into the channel field");
}

void test_command_framing()
{
    using can::pcan::Opcode;

    std::vector<uint8_t> buffer;
    can::pcan::append_command(buffer, 1, Opcode::NormalMode);
    check(buffer.size() == can::pcan::kCommandSize, "a command is eight bytes");
    check(buffer[0] == 0x02 && buffer[1] == 0x10,
          fmt::format("little-endian opcode-and-channel: got {}", hex(buffer)));

    // Several commands pack into one buffer, then an end marker, then padding.
    std::vector<uint8_t> batch;
    can::pcan::append_command(batch, 0, Opcode::ResetMode);
    can::pcan::append_command(batch, 0, Opcode::NormalMode);
    check(batch.size() == 16, "two commands are sixteen bytes");

    can::pcan::finish_command_buffer(batch);
    check(batch.size() == can::pcan::kCommandBufferSize,
          "the buffer is padded to the size the device reads");
    check(batch[16] == 0xFF && batch[17] == 0x03,
          fmt::format("with the end-of-collection marker after the last command: {}",
                      hex(std::span(batch).subspan(16, 4))));
}

void test_timing_slow_encoding()
{
    // 500 kbit/s at 80 MHz: brp 1, 160 quanta, sample point 87.5% -> tseg1 139,
    // tseg2 20. That does not fit the nominal segment limits, so the solver
    // will have picked a longer prescaler; take whatever it gives and check the
    // encoding round-trips the numbers rather than assuming particular ones.
    auto timing = can::solve_bit_timing(500000, 875, can::pcan::nominal_bit_timing_limits());
    check(timing.has_value(), "500 kbit/s solves for the PCAN FD controller");
    if (!timing.has_value())
    {
        return;
    }

    std::vector<uint8_t> buffer;
    can::pcan::append_timing_slow(buffer, 1, *timing, 96, false);
    check(buffer.size() == 8, "a timing command is eight bytes");

    check(buffer[0] == 0x04 && buffer[1] == 0x10, "addressed to channel 1, opcode TimingSlow");
    check(buffer[2] == 96, "the error warning limit is carried verbatim");
    check((buffer[3] & 0x0F) == (timing->sjw & 0x0F), "the jump width is in the low nibble");
    check((buffer[3] & 0x80) == 0, "and triple sampling is off");
    check(buffer[4] == (timing->tseg2 & 0x0F), "tseg2");
    check(buffer[5] == (timing->tseg1 & 0x3F), "tseg1");

    // The device counts the prescaler from zero, so a brp of 1 goes out as 0.
    // Getting this backwards halves or doubles the bit rate, which is the kind
    // of bug that looks like a broken cable.
    const uint16_t encodedBrp = static_cast<uint16_t>(buffer[6] | (buffer[7] << 8));
    check(encodedBrp == timing->brp - 1,
          fmt::format("the prescaler is encoded one less than it is: brp {} -> {}", timing->brp,
                      encodedBrp));

    std::vector<uint8_t> sampled;
    can::pcan::append_timing_slow(sampled, 0, *timing, 96, true);
    check((sampled[3] & 0x80) != 0, "triple sampling sets the top bit of the jump-width byte");
}

void test_timing_fast_encoding()
{
    auto timing = can::solve_bit_timing(2000000, 0, can::pcan::data_bit_timing_limits());
    check(timing.has_value(), "2 Mbit/s solves for the FD data phase");
    if (!timing.has_value())
    {
        return;
    }

    std::vector<uint8_t> buffer;
    can::pcan::append_timing_fast(buffer, 0, *timing);
    check(buffer.size() == 8, "a fast timing command is eight bytes");
    check(buffer[0] == 0x05 && buffer[1] == 0x00, "opcode TimingFast on channel 0");
    check(buffer[2] == 0, "the byte the nominal phase uses for the warning limit is unused");

    // The data phase's fields are narrower. A jump width that fitted the
    // nominal phase would be truncated here, which is why the solver is given
    // different limits rather than one set scaled.
    check((buffer[3] & 0x03) == (timing->sjw & 0x03), "the jump width fits two bits");
    check(timing->sjw <= 4, "and the solver kept it inside what the data phase allows");
    check(buffer[4] == (timing->tseg2 & 0x07), "tseg2 fits three bits");
    check(buffer[5] == (timing->tseg1 & 0x0F), "tseg1 fits four bits");
}

void test_option_and_filter_commands()
{
    std::vector<uint8_t> buffer;
    can::pcan::append_options(buffer, 0, false, 0, can::pcan::kOptionCalibrationMessages);
    check(buffer.size() == 8, "an option command is eight bytes");
    check(buffer[0] == 0x0C && buffer[1] == 0x00, "clearing options uses ClearDisableOption");
    check(buffer[6] == 0x00 && buffer[7] == 0x80,
          fmt::format("with the calibration bit in the USB mask: {}", hex(buffer)));

    std::vector<uint8_t> enabling;
    can::pcan::append_options(enabling, 1, true, 0, can::pcan::kOptionCalibrationMessages);
    check(enabling[0] == 0x0B && enabling[1] == 0x10, "enabling uses SetEnableOption on channel 1");

    std::vector<uint8_t> filter;
    can::pcan::append_std_filter_pass_all(filter, 0);
    check(filter.size() == 8, "a filter command is eight bytes");
    check(filter[4] == 0xFF && filter[5] == 0xFF && filter[6] == 0xFF && filter[7] == 0xFF,
          "and passing everything sets every bit of the acceptance bitmap");
}

// ============================================================================
// Firmware info
// ============================================================================

void test_firmware_info_decoding()
{
    std::vector<uint8_t> bytes;
    put_u16(bytes, 36);   // size
    put_u16(bytes, 2);    // extended record
    bytes.push_back(9);   // hardware type
    bytes.push_back(1);   // bootloader major
    bytes.push_back(2);
    bytes.push_back(3);
    bytes.push_back(4);   // hardware version
    bytes.push_back(3);   // firmware major
    bytes.push_back(2);
    bytes.push_back(1);
    put_u32(bytes, 0xAABBCCDD); // device id 0
    put_u32(bytes, 0x11223344); // device id 1
    put_u32(bytes, 123456);     // serial
    put_u32(bytes, 0);          // flags
    bytes.push_back(0x01);      // cmd out
    bytes.push_back(0x81);      // cmd in
    bytes.push_back(0x02);      // data out 0
    bytes.push_back(0x03);      // data out 1
    bytes.push_back(0x82);      // data in
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(0);

    auto info = can::pcan::decode_firmware_info(bytes);
    check(info.has_value(), "the firmware info record decodes");
    if (!info.has_value())
    {
        SPDLOG_ERROR("  {}", can::to_string(info.error()));
        return;
    }

    check(info->serialNumber == 123456, "the serial number is read");
    check(info->firmwareVersionString() == "3.2.1", "so is the firmware version");
    check(info->hasEndpoints, "an extended record carries endpoint numbers");
    check(info->dataOutEndpoint[0] == 0x02 && info->dataOutEndpoint[1] == 0x03,
          "including one data-out endpoint per channel, which is how a two-channel dongle "
          "keeps its transmit queues apart");
    check(info->dataInEndpoint == 0x82, "and a shared data-in endpoint");

    // A base record has no endpoints, and the defaults have to stand.
    std::vector<uint8_t> base(bytes.begin(), bytes.begin() + 36);
    base[2] = 1;
    base[3] = 0;
    auto baseInfo = can::pcan::decode_firmware_info(base);
    check(baseInfo.has_value() && !baseInfo->hasEndpoints,
          "a base record reports no endpoints of its own");
    check(baseInfo.has_value()
              && baseInfo->dataInEndpoint == can::pcan::kDefaultDataInEndpoint,
          "so the family defaults stand");

    // A short transfer is a protocol error, not something to read past.
    auto truncated = can::pcan::decode_firmware_info(std::span(bytes).subspan(0, 20));
    check(!truncated.has_value(), "a short firmware info record is rejected");
    check(!truncated.has_value() && truncated.error().kind == can::Error::Kind::Protocol,
          "as a protocol error");
}

// ============================================================================
// Received records
// ============================================================================

// Builds a CanRx record the way the device would.
std::vector<uint8_t> make_rx_record(uint8_t channel, uint32_t id, std::vector<uint8_t> payload,
                                    uint16_t flags, uint64_t timestamp, bool fd)
{
    const uint8_t dlc = can::length_to_dlc(static_cast<uint8_t>(payload.size()), fd);
    const uint8_t onWire = can::dlc_to_length(dlc, fd);
    payload.resize(onWire, 0);

    std::vector<uint8_t> record;
    const size_t padded = (onWire + 3) & ~size_t { 3 };
    put_u16(record, static_cast<uint16_t>(28 + padded));
    put_u16(record, static_cast<uint16_t>(can::pcan::RecordType::CanRx));
    put_u32(record, static_cast<uint32_t>(timestamp & 0xFFFFFFFF));
    put_u32(record, static_cast<uint32_t>(timestamp >> 32));
    put_u32(record, 0); // tag low
    put_u32(record, 0); // tag high
    record.push_back(static_cast<uint8_t>((channel & 0x0F) | (dlc << 4)));
    record.push_back(0); // client
    put_u16(record, flags);
    put_u32(record, id);
    for (size_t i = 0; i < padded; ++i)
    {
        record.push_back(i < payload.size() ? payload[i] : 0);
    }
    return record;
}

void test_rx_decoding()
{
    // A plain 11-bit data frame.
    auto record = make_rx_record(0, 0x123, { 0xDE, 0xAD, 0xBE, 0xEF }, 0, 0x0000000012345678ull,
                                 false);
    auto decoded = can::pcan::decode_records(record);
    check(!decoded.error.has_value(), "a well-formed record decodes without error");
    check(decoded.records.size() == 1, "and yields one record");
    if (decoded.records.size() != 1)
    {
        return;
    }

    const auto& r = decoded.records[0];
    check(r.type == can::pcan::RecordType::CanRx, "typed as a received frame");
    check(r.channel == 0, "on channel 0");
    check(r.frame.id == 0x123, "with the right identifier");
    check(!r.frame.isExtended, "as a standard frame");
    check(r.frame.len == 4, "and four bytes");
    check(r.frame.data[0] == 0xDE && r.frame.data[3] == 0xEF, "of the right payload");
    check(r.timestampUs == 0x12345678ull, "the device timestamp is reassembled from both halves");
    check(r.frame.timestampUs == r.timestampUs, "and reaches the frame");

    // A 29-bit frame on channel 1. The identifier must not be masked to 11
    // bits, and the channel must come from the record rather than be assumed.
    auto extended = make_rx_record(1, 0x1ABCDEF, { 0x01 }, can::pcan::kFlagExtendedId, 0, false);
    auto extendedDecoded = can::pcan::decode_records(extended);
    check(extendedDecoded.records.size() == 1, "an extended frame decodes");
    if (!extendedDecoded.records.empty())
    {
        check(extendedDecoded.records[0].frame.isExtended, "flagged extended");
        check(extendedDecoded.records[0].frame.id == 0x1ABCDEF,
              "with all 29 bits of the identifier");
        check(extendedDecoded.records[0].channel == 1, "on channel 1");
    }

    // An FD frame with bit-rate switching and a length only FD can express.
    auto fd = make_rx_record(
        0, 0x7FF, std::vector<uint8_t>(48, 0xA5),
        can::pcan::kFlagExtendedDataLength | can::pcan::kFlagBitrateSwitch, 0, true);
    auto fdDecoded = can::pcan::decode_records(fd);
    check(fdDecoded.records.size() == 1, "an FD frame decodes");
    if (!fdDecoded.records.empty())
    {
        const auto& f = fdDecoded.records[0].frame;
        check(f.isFD, "flagged as FD");
        check(f.isBRS, "with the bit-rate switch");
        check(f.len == 48, "and 48 bytes, which only the FD length table can express");
        check(f.data[47] == 0xA5, "all of which arrive");
    }

    // A remote frame has a length but no payload.
    auto rtr = make_rx_record(0, 0x100, {}, can::pcan::kFlagRtr, 0, false);
    auto rtrDecoded = can::pcan::decode_records(rtr);
    check(rtrDecoded.records.size() == 1, "a remote frame decodes");
    if (!rtrDecoded.records.empty())
    {
        check(rtrDecoded.records[0].frame.isRTR, "flagged RTR");
    }
}

void test_multiple_records_in_one_transfer()
{
    // A bulk transfer holds however many records fit, which is the normal case
    // on a busy bus -- decoding only the first would drop most of the traffic.
    std::vector<uint8_t> buffer;
    for (uint32_t i = 0; i < 5; ++i)
    {
        auto record = make_rx_record(static_cast<uint8_t>(i % 2), 0x200 + i,
                                     { static_cast<uint8_t>(i) }, 0, i * 1000, false);
        buffer.insert(buffer.end(), record.begin(), record.end());
    }

    auto decoded = can::pcan::decode_records(buffer);
    check(!decoded.error.has_value(), "a multi-record transfer decodes cleanly");
    check(decoded.records.size() == 5, "with every record found");
    for (size_t i = 0; i < decoded.records.size(); ++i)
    {
        check(decoded.records[i].frame.id == 0x200 + i,
              fmt::format("record {} has the right identifier", i));
        check(decoded.records[i].channel == (i % 2),
              fmt::format("record {} is routed to the right channel", i));
    }

    // Trailing zero padding is how the device fills out a transfer, and it
    // ends the walk rather than producing a phantom record.
    buffer.resize(buffer.size() + 16, 0);
    auto padded = can::pcan::decode_records(buffer);
    check(!padded.error.has_value() && padded.records.size() == 5,
          "trailing zero padding ends the walk without an error");
}

void test_malformed_records_stop_the_walk()
{
    // Once the length field is wrong there is no way to find the next record,
    // so the walk has to stop rather than resynchronise on a guess. Everything
    // decoded before the bad record is still good and still returned.
    auto good = make_rx_record(0, 0x111, { 0x01 }, 0, 0, false);
    std::vector<uint8_t> buffer = good;

    // A record claiming more bytes than the transfer holds.
    put_u16(buffer, 200);
    put_u16(buffer, static_cast<uint16_t>(can::pcan::RecordType::CanRx));
    buffer.resize(buffer.size() + 8, 0);

    auto decoded = can::pcan::decode_records(buffer);
    check(decoded.records.size() == 1, "the records before the bad one survive");
    check(decoded.error.has_value(), "and the truncated record is reported");
    check(decoded.error.has_value() && decoded.error->kind == can::Error::Kind::Protocol,
          "as a protocol error");

    // A length smaller than the header cannot be advanced past either.
    std::vector<uint8_t> tiny;
    put_u16(tiny, 2);
    put_u16(tiny, 1);
    auto tinyDecoded = can::pcan::decode_records(tiny);
    check(tinyDecoded.error.has_value(), "a record shorter than its own header is rejected");
    check(tinyDecoded.records.empty(), "and yields nothing");
}

void test_status_and_error_records()
{
    // Bus state. The device reports the bits cumulatively, so a bus-off
    // controller also has the passive and warning bits set; reporting the
    // mildest of those would say the bus is fine when it is not.
    auto status = [](uint8_t channel, uint8_t bits)
    {
        std::vector<uint8_t> record;
        put_u16(record, 16);
        put_u16(record, static_cast<uint16_t>(can::pcan::RecordType::Status));
        put_u32(record, 0);
        put_u32(record, 0);
        record.push_back(static_cast<uint8_t>(channel | bits));
        record.push_back(0);
        record.push_back(0);
        record.push_back(0);
        return record;
    };

    auto active = can::pcan::decode_records(status(0, 0));
    check(active.records.size() == 1 && active.records[0].state == can::BusState::ErrorActive,
          "no status bits means error-active");

    auto warning = can::pcan::decode_records(status(0, can::pcan::kStatusErrorWarning));
    check(warning.records.size() == 1 && warning.records[0].state == can::BusState::ErrorWarning,
          "the warning bit means error-warning");

    auto passive = can::pcan::decode_records(
        status(1, can::pcan::kStatusErrorWarning | can::pcan::kStatusErrorPassive));
    check(passive.records.size() == 1 && passive.records[0].state == can::BusState::ErrorPassive,
          "warning plus passive is error-passive, not warning");
    check(passive.records.size() == 1 && passive.records[0].channel == 1,
          "and the channel is read out of the same byte as the bits");

    auto off = can::pcan::decode_records(status(0, can::pcan::kStatusErrorWarning
                                                    | can::pcan::kStatusErrorPassive
                                                    | can::pcan::kStatusBusOff));
    check(off.records.size() == 1 && off.records[0].state == can::BusState::BusOff,
          "all three bits is bus-off, the most severe of them");

    // Error counters.
    std::vector<uint8_t> errorRecord;
    put_u16(errorRecord, 16);
    put_u16(errorRecord, static_cast<uint16_t>(can::pcan::RecordType::Error));
    put_u32(errorRecord, 0);
    put_u32(errorRecord, 0);
    errorRecord.push_back(1); // channel
    errorRecord.push_back(0);
    errorRecord.push_back(120); // tx errors
    errorRecord.push_back(64);  // rx errors
    auto errors = can::pcan::decode_records(errorRecord);
    check(errors.records.size() == 1, "an error record decodes");
    if (!errors.records.empty())
    {
        check(errors.records[0].txErrorCounter == 120 && errors.records[0].rxErrorCounter == 64,
              "with both counters, which is what says whether a bus is degrading");
        check(errors.records[0].channel == 1, "on the right channel");
    }

    // Overrun: the device dropped frames. A bridge that ignored this would
    // hand out a trace with silent holes in it.
    std::vector<uint8_t> overrun;
    put_u16(overrun, 16);
    put_u16(overrun, static_cast<uint16_t>(can::pcan::RecordType::Overrun));
    put_u32(overrun, 0);
    put_u32(overrun, 0);
    overrun.push_back(0);
    overrun.push_back(0);
    overrun.push_back(0);
    overrun.push_back(0);
    auto overrunDecoded = can::pcan::decode_records(overrun);
    check(overrunDecoded.records.size() == 1 && overrunDecoded.records[0].overrun,
          "an overrun record is decoded as one");
}

// ============================================================================
// Transmitted frames
// ============================================================================

void test_tx_encoding()
{
    helpers::CanFrame frame {};
    frame.id = 0x1AB;
    frame.len = 3;
    frame.data[0] = 0x11;
    frame.data[1] = 0x22;
    frame.data[2] = 0x33;

    std::vector<uint8_t> buffer;
    auto result = can::pcan::append_tx_frame(buffer, 1, frame, false);
    check(result.has_value(), "a classic frame encodes");
    if (!result.has_value())
    {
        SPDLOG_ERROR("  {}", can::to_string(result.error()));
        return;
    }

    // 20-byte header plus a payload padded to a multiple of four.
    check(buffer.size() == 24, fmt::format("the record is 24 bytes: {}", hex(buffer)));
    const uint16_t size = static_cast<uint16_t>(buffer[0] | (buffer[1] << 8));
    check(size == 24, "and says so in its length field");
    check(buffer[2] == 0x00 && buffer[3] == 0x10, "typed as a transmit record (0x1000)");
    // A transmit record has no timestamp, so the channel/DLC byte sits eight
    // bytes earlier than in a received one: size, type, tag(8), then it.
    check((buffer[12] & 0x0F) == 1, "addressed to channel 1");
    check(((buffer[12] >> 4) & 0x0F) == 3, "with DLC 3");
    check(buffer[20] == 0x11 && buffer[22] == 0x33, "and the payload follows the header");

    const uint32_t encodedId = static_cast<uint32_t>(buffer[16]) | (buffer[17] << 8)
        | (buffer[18] << 16) | (buffer[19] << 24);
    check(encodedId == 0x1AB, "with the identifier little-endian at offset 16");
}

void test_tx_flags_and_lengths()
{
    // Extended identifier.
    helpers::CanFrame extended {};
    extended.id = 0x1ABCDEF;
    extended.isExtended = true;
    extended.len = 1;
    std::vector<uint8_t> buffer;
    check(can::pcan::append_tx_frame(buffer, 0, extended, false).has_value(),
          "an extended frame encodes");
    const uint16_t flags = static_cast<uint16_t>(buffer[14] | (buffer[15] << 8));
    check((flags & can::pcan::kFlagExtendedId) != 0, "with the extended-id flag set");

    // FD with bit-rate switch.
    helpers::CanFrame fd {};
    fd.isFD = true;
    fd.isBRS = true;
    fd.id = 0x100;
    fd.len = 64;
    std::vector<uint8_t> fdBuffer;
    check(can::pcan::append_tx_frame(fdBuffer, 0, fd, true).has_value(), "a 64-byte FD frame encodes");
    check(fdBuffer.size() == 84, "as a 20-byte header plus 64 bytes");
    const uint16_t fdFlags = static_cast<uint16_t>(fdBuffer[14] | (fdBuffer[15] << 8));
    check((fdFlags & can::pcan::kFlagExtendedDataLength) != 0, "with the FD flag");
    check((fdFlags & can::pcan::kFlagBitrateSwitch) != 0, "and the bit-rate switch flag");
    check(((fdBuffer[12] >> 4) & 0x0F) == 15, "and DLC 15, which is what 64 bytes encodes as");
}

void test_tx_rejections()
{
    std::vector<uint8_t> buffer;

    // An identifier too wide for its format would go out as a different
    // message entirely.
    helpers::CanFrame tooWide {};
    tooWide.id = 0x800;
    tooWide.isExtended = false;
    auto wide = can::pcan::append_tx_frame(buffer, 0, tooWide, false);
    check(!wide.has_value(), "an 11-bit frame cannot carry a 12-bit identifier");
    check(!wide.has_value() && wide.error().kind == can::Error::Kind::InvalidArgument,
          "and the refusal is an invalid argument");

    // An FD frame on a channel opened without a data bit rate.
    helpers::CanFrame fd {};
    fd.isFD = true;
    fd.len = 8;
    auto notEnabled = can::pcan::append_tx_frame(buffer, 0, fd, false);
    check(!notEnabled.has_value(), "an FD frame on a classic channel is refused");

    // A length CAN FD cannot express. Padding it silently would put two bytes
    // of rubbish on the bus that the receiver has no way to identify.
    helpers::CanFrame odd {};
    odd.isFD = true;
    odd.len = 10;
    auto oddResult = can::pcan::append_tx_frame(buffer, 0, odd, true);
    check(!oddResult.has_value(), "a 10-byte FD payload is refused rather than padded silently");
    check(!oddResult.has_value() && oddResult.error().message.find("12") != std::string::npos,
          "and the message says what the next representable size is");

    // Classic CAN stops at eight.
    helpers::CanFrame tooLong {};
    tooLong.len = 9;
    check(!can::pcan::append_tx_frame(buffer, 0, tooLong, false).has_value(),
          "a nine-byte classic frame is refused");

    // FD has no remote frames.
    helpers::CanFrame fdRtr {};
    fdRtr.isFD = true;
    fdRtr.isRTR = true;
    check(!can::pcan::append_tx_frame(buffer, 0, fdRtr, true).has_value(),
          "CAN FD has no remote transmission request");

    check(buffer.empty(), "and nothing was written to the buffer by any of the refusals");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_product_table();
    test_opcode_channel_packing();
    test_command_framing();
    test_timing_slow_encoding();
    test_timing_fast_encoding();
    test_option_and_filter_commands();
    test_firmware_info_decoding();
    test_rx_decoding();
    test_multiple_records_in_one_transfer();
    test_malformed_records_stop_the_walk();
    test_status_and_error_records();
    test_tx_encoding();
    test_tx_flags_and_lengths();
    test_tx_rejections();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all PCAN uCAN codec checks passed");
    return 0;
}
