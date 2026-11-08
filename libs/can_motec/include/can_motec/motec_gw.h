// SPDX-License-Identifier: GPL-3.0-or-later
//
// The MoTeC CAN gateway protocol, as a pure codec.
//
// A MoTeC UTC is an FTDI FT245BM in front of a CAN controller. It does not
// speak a register protocol the way the PCAN family does; it speaks a small
// command envelope with a CRC, and CAN frames travel as 17-byte records in a
// block that hangs off the end of that envelope. The same envelope is used
// over USB bulk endpoints (the dongle) and over UDP (MoTeC's network
// gateways), which is why the codec is separate from any transport.
//
// PROVENANCE, which matters here more than usual: this protocol is not
// published. Everything below comes from
// https://github.com/ryandavid/motec-gw-sim, whose author reverse-engineered
// it from CAN Inspector v1.19 and from packet captures of a genuine UTC. The
// captures are the important half -- tests/golden/utc_frames.h holds eleven
// frames taken off real hardware, and every one of them round-trips through
// this file. What is NOT confirmed is called out at its definition, and the
// list is short but real: the Rx subscribe payload (its period byte is a
// strong inference), the RTR bit, the upper bits of the flags byte, what the
// Set command does, and whether the non-zero bit-rate indices of the Mode
// command behave as CAN Inspector's table says. See docs/libs/can_motec.md.
//
// Envelope, indexed from the start of the datagram:
//
//     [0..2]     80 81 86            preamble, NOT covered by the CRC
//     [3]        N                   count of CRC-covered bytes, starting here
//     [4]        CMD                 (tag << 5) | code; bit 0x10 set on replies
//     [5]        field5              bus handle from Open
//     [6]        REQID               rolling request id
//     [7..]      payload             command-specific, N-4 bytes
//     [3+N]      CRC-16-CCITT        big-endian, over the N bytes from [3]
//     [3+N+2..]  data block          Tx/Rx/RegRead only, and NOT checksummed
//
// So a frame is 3 + N + 2 + dataLength bytes, and the data block's length is
// not in the envelope -- it has to be derived from the command. That derivation
// is the single most delicate thing in this file, because a stream transport
// cannot fall back on "the rest of the datagram" the way a UDP one can. See
// data_block_length().
#ifndef CAN_MOTEC_GW_H
#define CAN_MOTEC_GW_H

#include "can/error.h"

#include "helpers/can_frame.h"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace can::motec
{

// --- USB identity ----------------------------------------------------------

inline constexpr uint16_t kFtdiVendorId = 0x0403;
// Not an FTDI stock product id: MoTeC programmes it into the FT245BM's EEPROM,
// which is why ftdi_sio does not bind to it without a new_id write.
inline constexpr uint16_t kUtcProductId = 0xDCD8;

inline constexpr uint8_t kBulkOutEndpoint = 0x02;
inline constexpr uint8_t kBulkInEndpoint = 0x81;

// The FT245BM is a fixed 64-byte FIFO part. There is no baud rate, no latency
// timer and no flow control to configure -- and issuing those control requests
// is not merely unnecessary, it is what a driver written against the more
// common FT232 would wrongly do first.
inline constexpr size_t kUsbPacketSize = 64;

// Every inbound 64-byte packet begins with two FTDI modem-status bytes that
// are not part of the stream. Outbound has no such prefix.
inline constexpr size_t kFtdiStatusPrefix = 2;

// --- the envelope ----------------------------------------------------------

inline constexpr uint8_t kPreamble0 = 0x80;
inline constexpr uint8_t kPreamble1 = 0x81;
inline constexpr uint8_t kPreamble2 = 0x86;

// Set in CMD on every response. A reply without it is not a reply.
inline constexpr uint8_t kReplyBit = 0x10;

// Preamble + length + CRC. The smallest legal frame carries no payload, so
// N is 4 and the whole thing is 9 bytes.
inline constexpr size_t kMinFrameSize = 3 + 4 + 2;
inline constexpr uint8_t kMinCoveredLength = 4;

// One CAN record on the wire.
inline constexpr size_t kRecordSize = 17;

// The low 4 bits of CMD. Note this is 0x0F and not 0x1F: the reply bit 0x10
// sits immediately above the code, so a 5-bit read would fold every response
// into a different command.
enum class Code : uint8_t
{
    // Opens a session. The response's field5 is the bus handle every later
    // request has to echo.
    Open = 0x00,
    // Ends the session (CAN Inspector's CMinReqCmd). Answered with a bare
    // status. Not a keep-alive, which it was once taken for.
    Close = 0x01,
    // One-way. Must NOT be answered -- replying to it desynchronises the
    // request-id matching at the other end.
    Ack = 0x02,
    // Register/memory read. Answers on the data path.
    RegRead = 0x04,
    // Mode: the receive mode, one id/mask acceptance filter, AND the CAN bit
    // rate. Once per session, after Version. Long read as a filter write
    // only; see make_mode().
    Mode = 0x06,
    // Transmit. Records ride in the data block.
    Tx = 0x08,
    // Subscribe to received frames. Sent ONCE; the device then free-runs.
    Rx = 0x09,
    // A value write used only by MoTeC's ADL protocol; payload
    // [BE32 key][len][8 bytes]. NOT the bit rate (that is Mode's), and what
    // it does on a UTC is unknown -- a guessed write was refused with 0x22.
    Set = 0x0A,
    // Version negotiation. A real UTC answers 7.2.
    Version = 0x0F,
};

const char* to_string(Code code);

// CRC-16-CCITT: polynomial 0x1021, initial value 0xFFFF, non-reflected, no
// final XOR, stored big-endian. Every one of those five choices has a
// plausible alternative that produces a different value, which is why the
// golden frames are the thing that pins it down rather than the name.
uint16_t crc16_ccitt(std::span<const uint8_t> bytes);

struct Frame
{
    uint8_t cmd { 0 };
    // The bus handle on a request; on an Open response, the handle itself.
    uint8_t field5 { 0 };
    uint8_t reqid { 0 };
    // The CRC-covered bytes after reqid.
    std::vector<uint8_t> payload;
    // The block after the header CRC. Empty for everything except a Tx
    // request, an Rx response and a RegRead response.
    std::vector<uint8_t> data;

    Code code() const { return static_cast<Code>(cmd & 0x0F); }
    uint8_t tag() const { return static_cast<uint8_t>(cmd >> 5); }
    bool isReply() const { return (cmd & kReplyBit) != 0; }

    // Payload byte 0 on any response that carries one. Non-zero is the
    // device refusing, and every caller has to check it: a refused command
    // still arrives with a valid CRC and the right request id.
    std::optional<uint8_t> status() const;
};

// Builds the complete datagram, recomputing N and the CRC. The data block is
// appended after the CRC and is deliberately not covered by it.
std::vector<uint8_t> encode_frame(const Frame& frame);

// How many bytes of data block follow the header CRC, given the command and
// its payload.
//
// This cannot be answered by "whatever is left in the buffer" on a stream
// transport, and getting it wrong does not desynchronise loudly -- it consumes
// the next frame's preamble as payload and then rejects a frame that was
// perfectly good. Three commands carry a block and the rest carry none:
//
//     Tx  request   payload = [BE16 byte count][BE16 record count]  -> [0..1]
//     Rx  response  payload = [status][BE16 byte count]             -> [1..2]
//     RegRead resp  payload = [status][BE16 byte count]             -> [1..2]
//
// A Tx *response* is the trap. It carries a BE16 in the same place the Rx
// response carries one -- the count of bytes the device accepted -- but no
// block follows it. Reading it as a length waits forever for 17 bytes that
// are never sent.
size_t data_block_length(const Frame& frame);

// Decodes exactly one frame from the front of `bytes`. Requires the whole
// frame, data block included, to be present.
Result<Frame> decode_frame(std::span<const uint8_t> bytes);

// --- CAN records -----------------------------------------------------------

// The identifier word carries the frame format in its top two bits, and the
// two formats are NOT laid out the same way:
//
//     bit 31 set   extended: the 29-bit identifier sits in bits 0..28
//     bit 30 set   standard: the 11-bit identifier sits in bits 18..28
//
// The standard case is left-aligned because bits 18..28 are the top eleven
// bits of the 29-bit arbitration field -- it is the same field, just shorter.
//
// Both halves of this were wrong before and neither was detectable from the
// captures, which contain extended frames only. On receive, reading a standard
// frame's identifier from the low bits yields a large, stable, entirely wrong
// number. On transmit, setting neither bit made the device send every standard
// frame as a 29-bit frame -- confirmed against a PCAN dongle on the same bus,
// which reported them as extended.
//
// The values below were measured, not inferred: identifiers 0x001, 0x100,
// 0x200 and 0x7FF transmitted from a PCAN and read back off a real UTC as
// 0x40040000, 0x44000000, 0x48000000 and 0x5FFC0000.
inline constexpr uint32_t kExtendedIdBit = 0x80000000u;
inline constexpr uint32_t kStandardIdBit = 0x40000000u;
inline constexpr uint32_t kExtendedIdMask = 0x1FFFFFFFu;
inline constexpr uint32_t kStandardIdMask = 0x7FFu;
// How far a standard identifier is shifted up in the word.
inline constexpr unsigned kStandardIdShift = 18;

struct Record
{
    // As it appears on the wire, extended bit included.
    uint32_t wireId { 0 };
    // Low nibble is the DLC. The upper four bits are not understood; they are
    // preserved on the way through rather than masked off, so a capture taken
    // through this library still shows what the device sent.
    uint8_t flags { 0 };
    std::array<uint8_t, 8> data { {} };
    // Free-running microseconds from the device. Roughly 1,001,800 ticks per
    // second, so it drifts against wall-clock by about 0.18%.
    uint32_t timestampUs { 0 };

    uint8_t dlc() const { return static_cast<uint8_t>(flags & 0x0F); }
    bool extended() const { return (wireId & kExtendedIdBit) != 0; }
    // Every frame a UTC receives carries one of the two format bits. Under
    // sustained overrun (past about 8,100 frames a second) a real UTC sent
    // every 128th record with the identifier word zeroed and the data intact:
    // that is damage, not a frame with identifier 0.
    bool hasFormat() const { return (wireId & (kExtendedIdBit | kStandardIdBit)) != 0; }
    // The identifier as the bus sees it, undoing whichever layout applies.
    uint32_t busId() const
    {
        return extended() ? (wireId & kExtendedIdMask)
                          : ((wireId >> kStandardIdShift) & kStandardIdMask);
    }
};

void pack_record(std::span<uint8_t, kRecordSize> out, const Record& record);
Record unpack_record(std::span<const uint8_t, kRecordSize> in);

// Walks a block of records. A block whose length is not a whole multiple of 17
// is truncated to the records that are complete rather than rejected: the
// device has been observed to pad, and throwing away frames that decoded
// cleanly because of a trailing byte would be worse than ignoring it.
std::vector<Record> unpack_records(std::span<const uint8_t> block);
std::vector<uint8_t> pack_records(std::span<const Record> records);

// Record to frame. `len` comes from the DLC and the bytes past it are dropped:
// the device leaves stale buffer content there rather than zeroes, so copying
// all eight would put junk into the payload of a short frame.
helpers::CanFrame to_can_frame(const Record& record);

// Frame to record. Fails for anything classic CAN cannot carry: an FD frame, a
// payload over 8 bytes, or an identifier too big for its format.
Result<Record> from_can_frame(const helpers::CanFrame& frame);

// --- command builders ------------------------------------------------------
//
// `tag` is echoed by the device and its origin is not understood; the captures
// show 1 on the client's requests. It is a parameter rather than a constant so
// that a device which turns out to care can be driven without editing this.

inline constexpr uint8_t kDefaultTag = 1;

// The V6 Open's payload is [bus][timeout] -- long misread as a two-byte
// protocol version. CAN Inspector sends 00 05; the captured UTC client sent
// 00 05 and 00 0A. The timeout's unit on the device is not confirmed (the
// client derives its own Rx timeout as timeout * 1000 / 3 ms). A one-byte
// payload is the V3 Open; a device answering 0x23 wants that instead.
inline constexpr uint8_t kOpenTimeout = 0x0A;
Frame make_open(uint8_t reqid, uint8_t bus = 0, uint8_t timeout = kOpenTimeout,
                uint8_t tag = kDefaultTag);

// --- getting a latched device back ------------------------------------------
//
// The device can end up refusing EVERY command on the normal tag with status
// 0x21 -- Open included, so there is no way back in by the front door. It is
// not dead while it does this: the replies are well formed and the request id
// is echoed. Nothing at the USB level clears it. Re-opening the device, both
// bulk endpoints cleared, a full USBDEVFS_RESET and even a host reboot all
// leave it latched, because the FT245BM in front is only a FIFO bridge and
// none of that removes power from the microcontroller behind it.
//
// What does clear it is `tag`. The command byte is (tag << 5) | code, and tag
// selects an endpoint: tag 0 is a separate one that stays responsive while the
// rest are latched, answering with its own per-command statuses rather than a
// blanket 0x21. An Open addressed there brings the device back, after which
// the normal tag works again -- verified against a UTC that had survived a
// reboot still latched.
inline constexpr uint8_t kStatusLatched = 0x21;
inline constexpr uint8_t kManagementTag = 0;

// An Open on the management tag. The payload matters: with none at all the
// device answers 0x23 (the "use a V3 Open" status) and stays latched.
//
// There is a worse latch this does NOT clear: writing transmits faster than
// the device drains them left a USB write stuck part way through a frame and
// the device answering 0x21 on the management tag too, until power-cycled.
// That one is prevented, not recovered -- see kTxWindow in utc_backend.cpp.
Frame make_unlock(uint8_t reqid, uint8_t bus = 0, uint8_t timeout = kOpenTimeout);
Frame make_version(uint8_t busHandle, uint8_t reqid, uint8_t tag = kDefaultTag);
Frame make_close(uint8_t busHandle, uint8_t reqid, uint8_t tag = kDefaultTag);

// --- Mode: receive mode, acceptance filter and bit rate ----------------------
//
// Payload, twelve bytes:
//
//     +0   BE32  id pattern   (bit 31 = extended, bit 30 = standard, as in a record)
//     +4   BE32  mask         1 = don't care, so all ones accepts everything
//     +8   u8    mode         1 ADL, 2 message/polled, 3 message-stream
//     +9   u8    0
//     +10  u8    0
//     +11  u8    bit-rate index (kBitrateTable)
//
// This was read as a filter write for a long time, with +8 as a "filter index"
// and +11 as padding -- which meant every session set the bus to index 0,
// 1 Mbit/s, while the backend said the rate could not be set at all. The
// layout comes from CAN Inspector's serialisers and all three captures.
//
// Learned on a real UTC: mode 0 and 4 and up are refused with 0x22, and 1 was
// refused with 0x40 in one session (a captured client did use 1). ONE Mode is
// accepted per session -- a second is refused, and an Open starts a new
// session -- so changing the rate means Open, Version, Mode again.
inline constexpr uint8_t kModeAdl = 1;
inline constexpr uint8_t kModeMessage = 2;
inline constexpr uint8_t kModeStream = 3;

// CAN Inspector's table: the index is what goes on the wire, and the client
// clamps anything above 5 to 0. Index 0 is the only one a capture shows; the
// others are its UI labels, NOT yet confirmed against hardware.
inline constexpr uint32_t kBitrateTable[] = { 1000000, 500000, 250000, 125000, 66667, 33333 };

// The index for a rate within 0.5% of a table entry (the tolerance CiA 301
// allows across a bus), or nullopt for a rate the device cannot run.
std::optional<uint8_t> bitrate_index(uint32_t bitrateBps);

Frame make_mode(uint8_t busHandle, uint8_t reqid, uint32_t pattern, uint32_t mask, uint8_t mode,
                uint8_t bitrateIndex, uint8_t tag = kDefaultTag);

// Stream mode, accepting every identifier, at `bitrateIndex`.
Frame make_accept_all_mode(uint8_t busHandle, uint8_t reqid, uint8_t bitrateIndex,
                           uint8_t tag = kDefaultTag);

// Sent once. The device then pushes data frames on its own request-id counter
// roughly every 255 ms, empty ones included, and never asks for anything back.
//
// Payload FF FF <period> <flags>. The third byte is most likely the push
// period in ms: this sends 0xFF and a UTC pushed every ~255 ms; CAN Inspector
// sends 0x64. One capture, so inference rather than proof. Bit 0 of flags is
// a session capability bit.
Frame make_rx_subscribe(uint8_t busHandle, uint8_t reqid, uint8_t tag = kDefaultTag);

Frame make_tx(uint8_t busHandle, uint8_t reqid, std::span<const Record> records,
              uint8_t tag = kDefaultTag);

// --- reading a stream ------------------------------------------------------

// Reassembles frames from a byte stream.
//
// USB delivers 64-byte packets whose boundaries have nothing to do with frame
// boundaries: a frame straddles packets, and several small frames arrive in
// one. So the transport strips the FTDI status bytes, pushes what is left in
// here, and takes whole frames out.
//
// Resynchronisation is the reason this is a class rather than a loop over
// decode_frame(). When bytes are lost -- and on a FIFO part with no flow
// control they can be -- the parser has to find the next preamble rather than
// reject everything after the damage. It does that by discarding one byte at a
// time until a preamble appears, which is slow and correct; the alternative,
// clearing the buffer, throws away frames that had already arrived intact.
class FrameReader
{
public:
    // Feeds bytes with the FTDI status prefixes already removed.
    void push(std::span<const uint8_t> bytes);

    // Takes the next complete frame, or nullopt when more bytes are needed.
    // Call until it returns nullopt.
    std::optional<Frame> next();

    // Bytes thrown away resynchronising. Non-zero means the stream was
    // damaged, which is worth reporting rather than silently absorbing.
    uint64_t resyncBytes() const { return resyncBytes_; }
    uint64_t badCrcFrames() const { return badCrcFrames_; }

    size_t buffered() const { return buffer_.size() - consumed_; }
    void reset();

private:
    void compact();

    std::vector<uint8_t> buffer_;
    size_t consumed_ { 0 };
    uint64_t resyncBytes_ { 0 };
    uint64_t badCrcFrames_ { 0 };
};

// Removes the two-byte modem status that heads every inbound USB packet.
//
// `packetSize` is the endpoint's maximum, and a short final packet is normal:
// a transfer that returns 34 bytes is one packet holding 32 bytes of stream. A
// packet holding only the two status bytes is the device saying nothing, which
// is what most of them are on an idle bus.
std::vector<uint8_t> strip_ftdi_status(std::span<const uint8_t> transfer,
                                       size_t packetSize = kUsbPacketSize);

} // namespace can::motec

#endif // CAN_MOTEC_GW_H
