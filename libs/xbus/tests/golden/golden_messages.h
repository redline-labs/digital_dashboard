// SPDX-License-Identifier: GPL-3.0-or-later
//
// Byte vectors for the XBus framing tests.
//
// PROVENANCE MATTERS HERE MORE THAN USUAL, because no MTi has been on the
// bench. libs/gsof's golden README states the problem: a byte vector authored
// from the same reading of the spec as the parser agrees with the parser by
// construction, including everywhere both are wrong. With no capture to fall
// back on, the vectors below are split by how much they are worth, and each
// one says which it is.
//
// VENDOR: printed complete, with its checksum, in the LLCP (document MT0101P
// rev 2019.C, section 5.2 and section 5.3.6). These came out of Xsens'
// encoder, not out of anybody's reading of the spec, and they are the only
// bytes in this library with that property. They pin the checksum rule and the
// header layout outright.
//
// SYNTHETIC: constructed here to exercise a case the LLCP does not print --
// extended length, a payload containing a preamble byte, a truncated header.
// These check that the parser is self-consistent and that it survives
// malformed input. They CANNOT confirm the wire format, and must not be read
// as if they could.
//
// See README.md for how to promote the synthetic ones once hardware exists.

#ifndef XBUS_GOLDEN_MESSAGES_H
#define XBUS_GOLDEN_MESSAGES_H

#include <array>
#include <cstdint>

namespace xbus::golden
{

// VENDOR. LLCP 5.2: "Requesting the device ID of an MT".
//   preamble  bid   mid(ReqDID)  len   checksum
//   FA        FF    00           00    01
inline constexpr std::array<std::uint8_t, 5> kReqDeviceId {
    0xFA, 0xFF, 0x00, 0x00, 0x01
};

// VENDOR. LLCP 5.2: "Request current baud rate". A request is distinguished
// from the set that shares its message id by its zero length, and nothing
// else -- this vector is what that claim rests on.
inline constexpr std::array<std::uint8_t, 5> kReqBaudrate {
    0xFA, 0xFF, 0x18, 0x00, 0xE9
};

// VENDOR. LLCP 5.2: the acknowledgement of SetBaudrate. Message id 0x19 is
// 0x18 + 1, which is the whole of the acknowledgement rule.
inline constexpr std::array<std::uint8_t, 5> kSetBaudrateAck {
    0xFA, 0xFF, 0x19, 0x00, 0xE8
};

// VENDOR. LLCP 5.3.6, "Switching from NMEA to MTData2", printed inline as
// 0x FA FF 8E 02 00 00 71. Note that the prose calls this "an empty data
// field" while the bytes carry a length of 2 -- the bytes are what is
// asserted, because the bytes are what the device was sent.
inline constexpr std::array<std::uint8_t, 7> kSetStringOutputTypeNone {
    0xFA, 0xFF, 0x8E, 0x02, 0x00, 0x00, 0x71
};

// VENDOR. LLCP 5.3.3, SetOptionFlags: "message for enabling AHS:
// FA FF 48 08 00 00 00 10 00 00 00 00 A1". The only vendor vector here with a
// non-trivial payload, and therefore the only evidence that the checksum rule
// holds across one -- the other four carry zero or two bytes.
//
// AHS itself is an MTi 1/10/100-series flag and means nothing on a 610; what
// is being asserted is the framing, not the flag.
inline constexpr std::array<std::uint8_t, 13> kSetOptionFlagsEnableAhs {
    0xFA, 0xFF, 0x48, 0x08, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0xA1
};

// VENDOR, and from a different source than the five above: these are complete
// messages lifted out of `bodypack_0_0.mtb`, a sample log shipped in the
// xsens-xme-sdk package. Unlike the LLCP's printed examples, nobody typeset
// these -- they are bytes an Xsens device actually put on a wire and Xsens'
// own software wrote to a file.
//
// The device is a Bodypack rather than an MTi, so its DATA is not something an
// MTi-610 would send. What they pin is the part that is common to both: the
// frame, the checksum, and the two reply layouts below.
//
// See tools/verify_sdk_corpus.py for the aggregate check over the whole file.

// MID 0x13, FirmwareRev. Eleven payload bytes, laid out per LLCP Table 4:
// major=1, minor=2, revision=0, build=2243, then the SCM reference. This is
// the only evidence that Table 4's offsets are right.
inline constexpr std::array<std::uint8_t, 16> kFirmwareRevReply {
    0xFA, 0xFF, 0x13, 0x0B, 0x01, 0x02, 0x00, 0x00, 0x00, 0x08, 0xC3, 0x00,
    0x01, 0x65, 0x6E, 0x41,
};

// MID 0xC1, the acknowledgement of an output configuration, carrying exactly
// one four-byte entry: data identifier 0x10A0 (XDI_SampleTime64) at 1 Hz.
//
// THIS IS THE ONLY VENDOR EVIDENCE FOR THE OUTPUT-CONFIGURATION ENTRY LAYOUT.
// Everything else about SetOutputConfiguration rests on the LLCP's tables, and
// docs/nodes/mti610_bridge.md lists it as deferred to hardware. This much -- that an entry
// is a big-endian identifier followed by a big-endian frequency, four bytes,
// and that a reply's length is a whole number of them -- is now settled.
inline constexpr std::array<std::uint8_t, 9> kOutputConfigAckReply {
    0xFA, 0xFF, 0xC1, 0x04, 0x10, 0xA0, 0x00, 0x01, 0x8B,
};

// SYNTHETIC. GoToConfig, which carries no payload.
inline constexpr std::array<std::uint8_t, 5> kGoToConfig {
    0xFA, 0xFF, 0x30, 0x00, 0xD1
};

// SYNTHETIC. An MTData2 carrying one item: PacketCounter (0x1020), two bytes,
// value 0x04D2. Built to the LLCP's item grammar, not captured.
//   FA FF 36 05 | 10 20 02 04 D2 | cs
// sum(FF 36 05 10 20 02 04 D2) = 0x0342 -> 0x42, checksum = 0xBE
inline constexpr std::array<std::uint8_t, 10> kMtData2PacketCounter {
    0xFA, 0xFF, 0x36, 0x05, 0x10, 0x20, 0x02, 0x04, 0xD2, 0xBE
};

// SYNTHETIC, and the one that matters most for the framer: an MTData2 whose
// PAYLOAD CONTAINS 0xFA. A preamble byte inside a payload is not rare -- it is
// one value out of 256 in every byte of every accelerometer reading -- and a
// framer that resynchronised by scanning for the next preamble would mis-frame
// here. One item: StatusWord (0xE020), four bytes, value 0xFA00FA01.
//   FA FF 36 07 | E0 20 04 FA 00 FA 01 | cs
// sum(FF 36 07 E0 20 04 FA 00 FA 01) = 0x0435 -> 0x35, checksum = 0xCB
inline constexpr std::array<std::uint8_t, 12> kMtData2WithPreambleInPayload {
    0xFA, 0xFF, 0x36, 0x07, 0xE0, 0x20, 0x04, 0xFA, 0x00, 0xFA, 0x01, 0xCB
};

} // namespace xbus::golden

#endif // XBUS_GOLDEN_MESSAGES_H
