// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MoTeC gateway codec, checked against bytes a real UTC produced.
//
// Two halves, and they carry different weight.
//
// The golden frames are evidence: eleven datagrams captured from hardware,
// every one of which has to decode and re-encode byte-identically. If the CRC
// parameters, the length arithmetic, the payload/data-block split or the
// record layout are wrong in any way, one of them stops round-tripping. This
// is the only part of this library that can be called confirmed.
//
// The rest is construction and malformed input, which nothing on a bus will
// ever hand over but a damaged stream will: bad CRCs, lengths that run off the
// end, blocks that arrive in pieces. The FrameReader is where those matter,
// because it is the only component here that has to make progress on a stream
// rather than judge a complete datagram.
#include "golden/utc_frames.h"

#include "can_motec/motec_gw.h"
#include "can_motec/utc_backend.h"

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace can::motec;

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

std::vector<uint8_t> from_hex(const std::string& text)
{
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < text.size();)
    {
        if (std::isspace(static_cast<unsigned char>(text[i])) != 0)
        {
            ++i;
            continue;
        }
        bytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
        i += 2;
    }
    return bytes;
}

std::string to_hex(std::span<const uint8_t> bytes)
{
    static const char* digits = "0123456789ABCDEF";
    std::string text;
    for (size_t i = 0; i < bytes.size(); ++i)
    {
        if (i != 0)
        {
            text += ' ';
        }
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 0x0F];
    }
    return text;
}

// ============================================================================
// Real hardware
// ============================================================================

// Decode, then re-encode, and require the bytes back exactly. Re-encoding is
// what makes this a test of the whole codec rather than of the decoder: a
// decoder that silently dropped the data block would still "decode" every
// frame below.
Frame round_trip(const golden::CapturedFrame& captured)
{
    const auto expected = from_hex(captured.hex);
    auto decoded = decode_frame(expected);
    if (!decoded.has_value())
    {
        check(false, fmt::format("{} did not decode: {}", captured.what,
                                 to_string(decoded.error())));
        return Frame {};
    }

    const auto reencoded = encode_frame(*decoded);
    check(reencoded == expected,
          fmt::format("{} re-encodes identically\n    got {}\n    expected {}", captured.what,
                      to_hex(reencoded), to_hex(expected)));
    return *decoded;
}

void test_every_captured_frame_round_trips()
{
    for (const auto& captured : golden::kHandshake)
    {
        round_trip(captured);
    }
    for (const auto& captured : golden::kRegRead)
    {
        round_trip(captured);
    }
    for (const auto& captured : golden::kTx)
    {
        round_trip(captured);
    }
    for (const auto& captured : golden::kRx)
    {
        round_trip(captured);
    }
}

void test_captured_handshake_fields()
{
    auto open = round_trip(golden::kHandshake[0]);
    check(open.code() == Code::Open, "the captured Open is an Open");
    check(!open.isReply(), "the captured Open is a request");
    check(open.tag() == 1, "the captured Open carries tag 1");
    check(open.field5 == 0xFF, "an Open goes out with field5 0xFF, before a handle exists");
    check(open.payload.size() == 2, "the captured Open asks for the V6 path with two version bytes");

    auto opened = round_trip(golden::kHandshake[1]);
    check(opened.isReply(), "the Open response has the reply bit set");
    check(opened.code() == Code::Open, "the Open response is still an Open");
    check(opened.field5 == 0x01, "the Open response carries the bus handle in field5");
    check(opened.status().value_or(0xFF) == 0, "the Open response reports success");

    auto version = round_trip(golden::kHandshake[2]);
    check(version.code() == Code::Version, "the captured Version response is a Version");
    check(version.payload.size() == 3, "a Version response is status, major, minor");
    if (version.payload.size() == 3)
    {
        check(version.payload[1] == 7 && version.payload[2] == 2,
              fmt::format("a real UTC reports 7.2, this capture says {}.{}", version.payload[1],
                          version.payload[2]));
    }

    auto filter = round_trip(golden::kHandshake[3]);
    check(filter.code() == Code::Filter, "the captured Filter write is a Filter");
    check(filter.payload.size() == 12, "a Filter payload is twelve bytes");
    if (filter.payload.size() == 12)
    {
        check(filter.payload[8] == 3, "and its index byte survives");
    }

    auto filtered = round_trip(golden::kHandshake[4]);
    // The distinguishing property: this reply looks like a data-path reply
    // until you notice it has no length and no block.
    check(filtered.data.empty(), "a Filter response carries no data block");
    check(filtered.status().value_or(0xFF) == 0, "the Filter response reports success");
}

void test_captured_data_path()
{
    auto read = round_trip(golden::kRegRead[1]);
    check(read.code() == Code::RegRead, "the captured RegRead response is a RegRead");
    check(read.isReply(), "it has the reply bit");
    check(read.data.size() == 2, fmt::format("its block is two bytes, got {}", read.data.size()));
    check(read.data.size() == 2 && read.data[0] == 0xE2 && read.data[1] == 0x00,
          "the register read answered E2 00");
    check(read.payload.size() == 3, "its payload is [status][BE16 length]");

    auto tx = round_trip(golden::kTx[0]);
    check(tx.code() == Code::Tx && !tx.isReply(), "the captured Tx is a Tx request");
    check(tx.payload.size() == 4, "a Tx request payload is [BE16 bytes][BE16 records]");
    check(tx.data.size() == kRecordSize,
          fmt::format("its block holds one 17-byte record, got {}", tx.data.size()));

    auto records = unpack_records(tx.data);
    check(records.size() == 1, "one record decodes out of the captured Tx");
    if (records.size() == 1)
    {
        // The identifier is the thing worth asserting: 0x9CF8D281 on the wire
        // is extended 0x1CF8D281 on the bus, and reading bit 31 as part of the
        // identifier gives a number that looks perfectly reasonable.
        check(records[0].extended(), "the captured Tx record is extended");
        check(records[0].busId() == 0x1CF8D281u,
              fmt::format("its bus identifier is 0x1CF8D281, got 0x{:X}", records[0].busId()));
        check(records[0].dlc() == 7, "its DLC is 7");
    }

    // The trap this codec has to get right: a Tx acknowledgement carries a
    // BE16 in the same position an Rx response carries its block length, and
    // there is no block behind it.
    auto ack = round_trip(golden::kTx[1]);
    check(ack.code() == Code::Tx && ack.isReply(), "the captured Tx ack is a Tx reply");
    check(ack.data.empty(), "a Tx acknowledgement carries no data block");
    check(data_block_length(ack) == 0, "and its declared block length is zero, not its byte count");
    check(ack.payload.size() == 3, "its payload is [status][BE16 bytes accepted]");
    if (ack.payload.size() == 3)
    {
        check(ack.payload[1] == 0x00 && ack.payload[2] == 0x11,
              "it accepted 17 bytes, which is one record");
    }
}

void test_captured_rx_stream()
{
    auto idle = round_trip(golden::kRx[0]);
    check(idle.code() == Code::Rx && idle.isReply(), "the captured Rx keep-alive is an Rx reply");
    check(idle.data.empty(), "a keep-alive carries no records");
    check(data_block_length(idle) == 0, "and declares a zero-length block");

    auto one = round_trip(golden::kRx[1]);
    auto records = unpack_records(one.data);
    check(records.size() == 1, "one record decodes out of the captured Rx");
    if (records.size() == 1)
    {
        check(records[0].busId() == 0x1CF81BECu,
              fmt::format("its bus identifier is 0x1CF81BEC, got 0x{:X}", records[0].busId()));
        check(records[0].extended(), "it is extended");
        check(records[0].dlc() == 7, "its DLC is 7");
        check(records[0].timestampUs == 0x63E89F8Du, "its timestamp survives the round trip");
    }

    auto two = round_trip(golden::kRx[2]);
    // 34 and not 36: the absence of a trailing CRC on the data block is the
    // whole assertion here.
    check(two.data.size() == 2 * kRecordSize,
          fmt::format("a two-record block is exactly {} bytes, with no trailing CRC, got {}",
                      2 * kRecordSize, two.data.size()));
    check(unpack_records(two.data).size() == 2, "two records decode out of it");
}

// The DLC says 7, so the eighth byte is not part of the frame. Real hardware
// leaves stale buffer content there rather than zero, so a converter that
// copied all eight would put a junk byte into a seven-byte payload.
void test_bytes_past_the_dlc_are_dropped()
{
    auto one = round_trip(golden::kRx[1]);
    auto records = unpack_records(one.data);
    check(records.size() == 1, "the capture holds one record");
    if (records.empty())
    {
        return;
    }

    check(records[0].data[7] == 0xCD, "the capture really does carry junk past the DLC");

    const auto frame = to_can_frame(records[0]);
    check(frame.len == 7, "the converted frame is seven bytes long");
    check(frame.id == 0x1CF81BECu && frame.isExtended, "and keeps its extended identifier");
    check(frame.data[7] == 0, "the byte past the DLC does not reach the payload");
    check(frame.timestampUs == 0x63E89F8Du, "and the device timestamp is carried through");
}

// ============================================================================
// The codec on its own
// ============================================================================

void test_crc_matches_the_captures()
{
    // Taken from the Open request: covered bytes 06 20 ff 01 00 0a, CRC 569C.
    const std::vector<uint8_t> covered { 0x06, 0x20, 0xFF, 0x01, 0x00, 0x0A };
    check(crc16_ccitt(covered) == 0x569C,
          fmt::format("CRC-16-CCITT of the captured Open header is 0x569C, got 0x{:04X}",
                      crc16_ccitt(covered)));
    // A different initial value or a reflected algorithm would still produce a
    // stable, plausible number, so pin the empty case too.
    check(crc16_ccitt({}) == 0xFFFF, "the initial value is 0xFFFF");
}

void test_builders_produce_decodable_frames()
{
    const struct
    {
        const char* what;
        Frame frame;
    } built[] = {
        { "Open", make_open(0x01) },
        { "Version", make_version(0x01, 0x02) },
        { "Poll", make_poll(0x01, 0x03) },
        { "accept-all Filter", make_accept_all_filter(0x01, 0x04) },
        { "Rx subscribe", make_rx_subscribe(0x01, 0x05) },
    };

    for (const auto& entry : built)
    {
        const auto bytes = encode_frame(entry.frame);
        auto decoded = decode_frame(bytes);
        check(decoded.has_value(), fmt::format("a built {} decodes", entry.what));
        if (decoded.has_value())
        {
            check(encode_frame(*decoded) == bytes,
                  fmt::format("a built {} round-trips", entry.what));
            check(!decoded->isReply(), fmt::format("a built {} is a request", entry.what));
        }
    }

    // The filter mask is "don't care" bits, so all ones has to mean everything
    // gets through. Inverting that sense is a silent way to receive nothing.
    const auto filter = make_accept_all_filter(0x01, 0x04);
    check(filter.payload.size() == 12, "an accept-all filter is still twelve bytes");
    check(filter.payload[4] == 0xFF && filter.payload[5] == 0xFF && filter.payload[6] == 0xFF
              && filter.payload[7] == 0xFF,
          "an accept-all filter sets every mask bit");
}

void test_tx_builder_matches_the_capture()
{
    // Rebuild the captured Tx request from a record and require the same
    // bytes. This is the strongest statement available about the builder:
    // it has to agree with hardware, not merely with the decoder.
    const auto expected = from_hex(golden::kTx[0].hex);
    auto captured = decode_frame(expected);
    check(captured.has_value(), "the captured Tx decodes");
    if (!captured.has_value())
    {
        return;
    }

    const auto records = unpack_records(captured->data);
    const auto rebuilt = encode_frame(
        make_tx(captured->field5, captured->reqid, records, captured->tag()));
    check(rebuilt == expected,
          fmt::format("make_tx rebuilds the captured request\n    got {}\n    expected {}",
                      to_hex(rebuilt), to_hex(expected)));
}

void test_can_frame_conversion()
{
    helpers::CanFrame frame;
    frame.id = 0x1CF8D281;
    frame.isExtended = true;
    frame.len = 7;
    for (uint8_t i = 0; i < 7; ++i)
    {
        frame.data[i] = static_cast<uint8_t>(0xA0 + i);
    }

    auto record = from_can_frame(frame);
    check(record.has_value(), "an extended classic frame converts to a record");
    if (record.has_value())
    {
        check(record->wireId == 0x9CF8D281u,
              fmt::format("the extended bit goes into bit 31, got 0x{:08X}", record->wireId));
        check(record->dlc() == 7, "the DLC carries the length");

        const auto back = to_can_frame(*record);
        check(back.id == frame.id && back.isExtended && back.len == frame.len,
              "and converts back unchanged");
    }

    helpers::CanFrame standard;
    standard.id = 0x7E0;
    standard.len = 8;
    auto standardRecord = from_can_frame(standard);
    check(standardRecord.has_value(), "a standard frame converts");
    check(standardRecord.has_value() && !standardRecord->extended(),
          "and does not read back as extended");
}

// The two identifier layouts, against words measured on the wire.
//
// This is the case the captures could not cover -- they are extended-only --
// and both directions were wrong because of it. Encoding a standard frame with
// neither format bit set made the device transmit it as a 29-bit frame, which
// a PCAN dongle on the same bus duly reported as extended; decoding one from
// the low bits gave a large, stable, entirely wrong identifier.
void test_identifier_layouts_against_the_wire()
{
    for (const auto& entry : golden::kIdentifiers)
    {
        // Decode: the wire word the device produced has to read back as the
        // identifier that was actually on the bus.
        Record record;
        record.wireId = entry.wireId;
        record.flags = 1;
        check(record.extended() == entry.extended,
              fmt::format("0x{:08X} reads as {}", entry.wireId,
                          entry.extended ? "extended" : "standard"));
        check(record.busId() == entry.busId,
              fmt::format("0x{:08X} carries bus identifier 0x{:X}, got 0x{:X}", entry.wireId,
                          entry.busId, record.busId()));

        // Encode: and building that frame has to produce the same word.
        helpers::CanFrame frame;
        frame.id = entry.busId;
        frame.isExtended = entry.extended;
        frame.len = 1;
        auto built = from_can_frame(frame);
        check(built.has_value(), fmt::format("0x{:X} converts to a record", entry.busId));
        check(built.has_value() && built->wireId == entry.wireId,
              fmt::format("0x{:X} ({}) encodes to 0x{:08X}, got 0x{:08X}", entry.busId,
                          entry.extended ? "extended" : "standard", entry.wireId,
                          built.has_value() ? built->wireId : 0));
    }

    // The format bits are distinct: a standard frame must not set the extended
    // bit, and vice versa. Setting neither is what the device read as
    // "extended", so this is the assertion that would have caught it.
    helpers::CanFrame standard;
    standard.id = 0x201;
    standard.len = 1;
    auto record = from_can_frame(standard);
    check(record.has_value() && (record->wireId & kExtendedIdBit) == 0,
          "a standard frame does not set the extended bit");
    check(record.has_value() && (record->wireId & kStandardIdBit) != 0,
          "a standard frame DOES set the standard bit -- setting neither made the device "
          "transmit it as a 29-bit frame");
}

void test_conversion_rejections()
{
    helpers::CanFrame fd;
    fd.isFD = true;
    fd.len = 16;
    auto rejected = from_can_frame(fd);
    check(!rejected.has_value(), "an FD frame is refused");
    check(!rejected.has_value() && rejected.error().kind == can::Error::Kind::Unsupported,
          "and refused as unsupported rather than as a bad argument");

    helpers::CanFrame tooLong;
    tooLong.len = 9;
    check(!from_can_frame(tooLong).has_value(), "a nine-byte classic frame is refused");

    helpers::CanFrame tooBig;
    tooBig.id = 0x800; // 12 bits, in a standard frame
    tooBig.len = 1;
    check(!from_can_frame(tooBig).has_value(),
          "an identifier too large for a standard frame is refused");

    // The RTR bit's position is unknown, so sending one would be a guess that
    // puts a data frame on the bus where a request was meant.
    helpers::CanFrame remote;
    remote.id = 0x100;
    remote.isRTR = true;
    check(!from_can_frame(remote).has_value(), "a remote frame is refused rather than guessed at");
}

void test_decode_rejections()
{
    check(!decode_frame({}).has_value(), "an empty buffer is refused");

    auto valid = from_hex(golden::kHandshake[1].hex);

    auto shortened = valid;
    shortened.resize(valid.size() - 1);
    check(!decode_frame(shortened).has_value(), "a truncated frame is refused");

    auto badPreamble = valid;
    badPreamble[1] = 0x00;
    check(!decode_frame(badPreamble).has_value(), "a frame without the preamble is refused");

    auto badCrc = valid;
    badCrc.back() ^= 0xFF;
    check(!decode_frame(badCrc).has_value(), "a frame with a bad CRC is refused");

    auto shortLength = valid;
    shortLength[3] = 3; // below the four header bytes it has to cover
    check(!decode_frame(shortLength).has_value(), "a covered length below four is refused");

    // A data-path response whose block never arrived. Accepting this would
    // hand a caller a short block and call it complete.
    auto truncatedBlock = from_hex(golden::kRx[1].hex);
    truncatedBlock.resize(truncatedBlock.size() - 4);
    check(!decode_frame(truncatedBlock).has_value(), "a truncated data block is refused");
}

// ============================================================================
// Reassembling a stream
// ============================================================================

void test_reader_reassembles_split_frames()
{
    // Everything concatenated, then fed one byte at a time. A reader that
    // depended on frame boundaries lining up with reads fails immediately.
    std::vector<uint8_t> stream;
    std::vector<std::vector<uint8_t>> expected;
    for (const auto& captured : golden::kRx)
    {
        auto bytes = from_hex(captured.hex);
        expected.push_back(bytes);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }

    FrameReader reader;
    std::vector<std::vector<uint8_t>> got;
    for (const uint8_t byte : stream)
    {
        reader.push(std::span(&byte, 1));
        while (auto frame = reader.next())
        {
            got.push_back(encode_frame(*frame));
        }
    }

    check(got.size() == expected.size(),
          fmt::format("byte-at-a-time delivery yields {} frames, got {}", expected.size(),
                      got.size()));
    check(got == expected, "and they are the frames that went in");
    check(reader.resyncBytes() == 0, "with nothing discarded");
    check(reader.buffered() == 0, "and nothing left buffered");
}

void test_reader_handles_several_frames_in_one_push()
{
    std::vector<uint8_t> stream;
    for (const auto& captured : golden::kRx)
    {
        auto bytes = from_hex(captured.hex);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
    }

    FrameReader reader;
    reader.push(stream);

    int count = 0;
    while (auto frame = reader.next())
    {
        ++count;
    }
    check(count == 3, fmt::format("three frames come out of one push, got {}", count));
    check(reader.buffered() == 0, "leaving nothing buffered");
}

// The case that motivates the whole class: a record block split across USB
// packet boundaries. The reader has to wait for the rest rather than treat the
// short block as a damaged frame and resynchronise past it.
void test_reader_waits_for_a_split_data_block()
{
    const auto whole = from_hex(golden::kRx[2].hex);

    FrameReader reader;
    // Everything except the last record's final four bytes.
    reader.push(std::span(whole).first(whole.size() - 4));
    check(!reader.next().has_value(), "a frame whose block is incomplete is not yet returned");
    check(reader.resyncBytes() == 0, "and nothing is discarded while waiting");

    reader.push(std::span(whole).last(4));
    auto frame = reader.next();
    check(frame.has_value(), "the frame arrives once the rest of its block does");
    check(frame.has_value() && frame->data.size() == 2 * kRecordSize,
          "with the whole block intact");
    check(reader.resyncBytes() == 0, "and still nothing discarded");
}

// A Tx acknowledgement carries a BE16 byte count in the same position an Rx
// response carries its block length, and NOTHING follows it. A reader that
// treated every data-path reply alike would wait for seventeen bytes that are
// never sent -- and then find them in the next frame, consuming it. So the
// assertion is not just that the ack decodes, but that the frame after it
// still arrives.
void test_tx_ack_does_not_swallow_the_next_frame()
{
    const auto ack = from_hex(golden::kTx[1].hex);
    const auto next = from_hex(golden::kRx[1].hex);

    std::vector<uint8_t> stream(ack.begin(), ack.end());
    stream.insert(stream.end(), next.begin(), next.end());

    FrameReader reader;
    reader.push(stream);

    auto first = reader.next();
    check(first.has_value(), "the Tx acknowledgement is read");
    check(first.has_value() && first->code() == Code::Tx && first->isReply(),
          "and it is a Tx reply");
    check(first.has_value() && first->data.empty(), "carrying no data block");

    auto second = reader.next();
    check(second.has_value(), "the frame after a Tx acknowledgement is NOT swallowed by it");
    check(second.has_value() && encode_frame(*second) == next,
          "and arrives intact");
    check(reader.buffered() == 0, "with nothing left over");
    check(reader.resyncBytes() == 0, "and nothing discarded");
}

void test_reader_resynchronises_after_damage()
{
    const auto good = from_hex(golden::kRx[1].hex);

    std::vector<uint8_t> stream { 0x11, 0x22, 0x33 };
    stream.insert(stream.end(), good.begin(), good.end());

    FrameReader reader;
    reader.push(stream);

    auto frame = reader.next();
    check(frame.has_value(), "a frame after leading junk is still found");
    check(reader.resyncBytes() == 3, fmt::format("three junk bytes are counted as discarded, got {}",
                                                 reader.resyncBytes()));

    // A corrupted frame followed by a good one: the good one must survive.
    auto damaged = good;
    damaged[9] ^= 0xFF; // inside the CRC-covered header
    std::vector<uint8_t> mixed(damaged.begin(), damaged.end());
    mixed.insert(mixed.end(), good.begin(), good.end());

    FrameReader second;
    second.push(mixed);
    int recovered = 0;
    while (auto next = second.next())
    {
        ++recovered;
    }
    check(recovered >= 1, "the intact frame after a corrupted one is still recovered");
    check(second.badCrcFrames() >= 1, "and the corruption is counted");
}

// A length byte that cannot be right must not make the reader wait forever for
// bytes that are never coming.
void test_reader_rejects_an_impossible_length()
{
    const auto good = from_hex(golden::kRx[1].hex);

    std::vector<uint8_t> stream { kPreamble0, kPreamble1, kPreamble2, 0x00, 0x00, 0x00 };
    stream.insert(stream.end(), good.begin(), good.end());

    FrameReader reader;
    reader.push(stream);
    auto frame = reader.next();
    check(frame.has_value(), "a frame following an impossible length byte is still found");
    check(reader.resyncBytes() > 0, "and the bad header is discarded rather than waited on");
}

void test_ftdi_status_stripping()
{
    // Two full packets: two status bytes then 62 bytes of stream, twice.
    std::vector<uint8_t> transfer;
    for (int packet = 0; packet < 2; ++packet)
    {
        transfer.push_back(0x31);
        transfer.push_back(0x60);
        for (int i = 0; i < 62; ++i)
        {
            transfer.push_back(static_cast<uint8_t>(packet * 62 + i));
        }
    }

    auto stripped = strip_ftdi_status(transfer);
    check(stripped.size() == 124, fmt::format("two full packets yield 124 bytes, got {}",
                                              stripped.size()));
    check(!stripped.empty() && stripped[0] == 0, "the first payload byte survives");
    check(stripped.size() == 124 && stripped[62] == 62,
          "and the second packet's payload follows the first with no status between");

    // A short final packet is normal, not an error.
    std::vector<uint8_t> shortPacket { 0x31, 0x60, 0xAA, 0xBB };
    auto shortStripped = strip_ftdi_status(shortPacket);
    check(shortStripped.size() == 2 && shortStripped[0] == 0xAA && shortStripped[1] == 0xBB,
          "a short packet keeps only its payload");

    // An idle poll: status bytes and nothing else.
    std::vector<uint8_t> idle { 0x31, 0x60 };
    check(strip_ftdi_status(idle).empty(), "a status-only packet contributes nothing");
    check(strip_ftdi_status({}).empty(), "an empty transfer contributes nothing");
}

// The status bytes are stripped per packet, so a frame that straddles a packet
// boundary only reassembles if the stripping happens before the reader sees
// it. Feeding the raw transfer straight in would put 31 60 in the middle of a
// record.
void test_stripping_then_reading_a_straddling_frame()
{
    const auto frame = from_hex(golden::kRx[2].hex);

    // Chop the frame into 62-byte pieces and give each a status prefix, as the
    // endpoint would.
    std::vector<uint8_t> transfer;
    for (size_t offset = 0; offset < frame.size(); offset += 62)
    {
        const size_t chunk = std::min<size_t>(62, frame.size() - offset);
        transfer.push_back(0x31);
        transfer.push_back(0x60);
        transfer.insert(transfer.end(), frame.begin() + static_cast<ptrdiff_t>(offset),
                        frame.begin() + static_cast<ptrdiff_t>(offset + chunk));
    }

    FrameReader reader;
    reader.push(strip_ftdi_status(transfer));
    auto decoded = reader.next();
    check(decoded.has_value(), "a frame split across USB packets reassembles");
    check(decoded.has_value() && encode_frame(*decoded) == frame, "byte for byte");
}

// The session keep-alive, which is a device requirement rather than a choice.
//
// A real UTC stops sending about ten seconds after the Rx subscribe unless the
// client keeps talking to it -- data frames stop, idle keep-alives stop, and a
// fresh subscribe is answered with status 0x04. Nothing warns first. This
// cannot be tested without the dongle, so what is guarded here is the two
// things that would silently reintroduce it: an interval that drifts up past
// the device's window, and the command being changed to the one that makes it
// worse.
// The command that gets a latched device back.
//
// A UTC can end up refusing every command on the normal tag with 0x21, Open
// included, so there is no way in by the front door. `tag` is the way out: it
// selects an endpoint, and tag 0 keeps answering while the others are latched.
// Verified on hardware against a device that had stayed latched through a full
// host reboot -- an Open there cleared it and the normal tag worked again.
void test_unlock_addresses_the_management_tag()
{
    const auto unlock = make_unlock(9);
    check(unlock.tag() == kManagementTag,
          fmt::format("the unlock goes to tag {}, got {}", kManagementTag, unlock.tag()));
    check(unlock.code() == Code::Open, "and it is an Open");
    check(!unlock.isReply(), "sent as a request");

    // Not a detail: an Open on tag 0 with NO payload is answered 0x23 and the
    // device stays latched. The version bytes are what makes it work.
    check(!unlock.payload.empty(), "an unlock carries a version payload; without one it is refused");
    check(unlock.field5 == 0xFF, "and goes out with no bus handle, like any other Open");

    check(decode_frame(encode_frame(unlock)).has_value(), "the unlock encodes to a valid frame");

    // The ordinary Open must NOT be on the management tag, or the two would be
    // the same command and the recovery would be a no-op.
    check(make_open(9).tag() != kManagementTag,
          "an ordinary Open is on a different tag from the unlock");
}

void test_keepalive_defaults_stay_inside_the_device_window()
{
    // Measured: the stream survived indefinitely at a 2 s interval and died at
    // about 10 s with none at all. Half the observed window is the most that
    // could be called safe, and the default should be well under it.
    constexpr unsigned kObservedTimeoutMs = 10000;
    MotecOptions options;
    check(options.keepAliveIntervalMs * 2 < kObservedTimeoutMs,
          fmt::format("the keep-alive interval ({} ms) leaves margin against the ~{} ms the "
                      "device allows",
                      options.keepAliveIntervalMs, kObservedTimeoutMs));
    check(options.keepAliveIntervalMs > 0, "and it is actually enabled");

    // Version is the command that works. Ack is the one that does not: sending
    // it stops the stream immediately, which is worth an assertion because the
    // two sit next to each other in the command table.
    const auto keepAlive = make_version(1, 7);
    check(keepAlive.code() == Code::Version, "the keep-alive is a Version command");
    check(keepAlive.code() != Code::Ack, "and specifically not an Ack");
    check(keepAlive.payload.empty(), "a Version request carries no payload");
    check(decode_frame(encode_frame(keepAlive)).has_value(), "and encodes to a valid frame");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_every_captured_frame_round_trips();
    test_captured_handshake_fields();
    test_captured_data_path();
    test_captured_rx_stream();
    test_bytes_past_the_dlc_are_dropped();

    test_crc_matches_the_captures();
    test_builders_produce_decodable_frames();
    test_tx_builder_matches_the_capture();
    test_can_frame_conversion();
    test_identifier_layouts_against_the_wire();
    test_conversion_rejections();
    test_decode_rejections();

    test_reader_reassembles_split_frames();
    test_reader_handles_several_frames_in_one_push();
    test_reader_waits_for_a_split_data_block();
    test_tx_ack_does_not_swallow_the_next_frame();
    test_reader_resynchronises_after_damage();
    test_reader_rejects_an_impossible_length();
    test_unlock_addresses_the_management_tag();
    test_keepalive_defaults_stay_inside_the_device_window();
    test_ftdi_status_stripping();
    test_stripping_then_reading_a_straddling_frame();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all MoTeC gateway codec checks passed");
    return 0;
}
