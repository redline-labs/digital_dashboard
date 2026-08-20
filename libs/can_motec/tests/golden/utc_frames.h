// SPDX-License-Identifier: GPL-3.0-or-later
//
// Frames captured from a genuine MoTeC UTC dongle.
//
// These are the reason this backend is worth trusting at all. The protocol is
// unpublished, so the alternative to real bytes is a vector written from the
// same reading of the specification as the parser -- which agrees with the
// parser precisely where both are wrong. That failure mode is not theoretical
// here: the captures are what established that the extended-identifier bit
// lives in bit 31 of the identifier rather than in the flags byte, that the
// data block has no CRC of its own, and that a Tx acknowledgement carries a
// byte count with nothing behind it.
//
// Provenance: https://github.com/ryandavid/motec-gw-sim,
// tests/test_real_utc_frames.cpp, taken from pcapng captures of CAN Inspector
// v1.19 talking to real hardware. Reproduced here as hex so this library's
// tests do not depend on that repository being checked out.
//
// Every frame below must decode, and must re-encode byte-identically.
#ifndef CAN_MOTEC_GOLDEN_UTC_FRAMES_H
#define CAN_MOTEC_GOLDEN_UTC_FRAMES_H

#include <cstdint>

namespace can::motec::golden
{

struct CapturedFrame
{
    const char* what;
    const char* hex;
};

// The session bring-up, in the order a real client performs it.
inline constexpr CapturedFrame kHandshake[] = {
    { "Open request", "80 81 86 06 20 ff 01 00 0a 56 9c" },
    // The response's field5 is the bus handle -- 0x01 here -- and every later
    // request has to echo it.
    { "Open response", "80 81 86 05 30 01 01 00 1a b3" },
    // A real UTC reports 7.2.
    { "Version response", "80 81 86 07 3f 01 02 00 07 02 fc e9" },
    { "Filter write", "80 81 86 10 26 01 03 9c f8 00 00 00 00 ff ff 03 00 00 00 38 4c" },
    // A bare status and NO data block, which is what distinguishes a Filter
    // reply from the data-path replies.
    { "Filter response", "80 81 86 05 36 01 03 00 5b 48" },
};

// The register read, which is a data-path response: status, a BE16 length, and
// that many raw bytes after the header CRC with no trailing CRC.
inline constexpr CapturedFrame kRegRead[] = {
    { "RegRead request", "80 81 86 0c 04 63 03 00 00 00 00 00 02 02 01 0c 7d" },
    { "RegRead response", "80 81 86 07 14 63 03 00 00 02 b8 3f e2 00" },
};

// Transmit: the records live outside the CRC-covered payload.
inline constexpr CapturedFrame kTx[] = {
    { "Tx request", "80 81 86 08 28 01 d6 00 11 00 01 09 0a 9c f8 d2 81 07 07 00 00 00 36 37 84 "
                    "00 00 00 00 01" },
    // The acknowledgement's BE16 is bytes accepted, and nothing follows it.
    { "Tx acknowledgement", "80 81 86 07 38 01 d6 00 00 11 ed 9f" },
};

// The receive stream. The device pushes one of these roughly every 255 ms on
// its own request-id counter, empty ones included.
inline constexpr CapturedFrame kRx[] = {
    { "Rx keep-alive", "80 81 86 07 39 01 b1 00 00 00 a2 d0" },
    { "Rx one record", "80 81 86 07 39 01 bc 00 00 11 99 46 9c f8 1b ec 07 aa 55 aa 55 aa 55 aa "
                       "cd 63 e8 9f 8d" },
    { "Rx two records", "80 81 86 07 39 01 bd 00 00 22 e9 c2 "
                        "9c f8 1b ec 07 aa 55 aa 55 aa 55 aa cd 63 ea 24 a7 "
                        "9c f8 1b ec 07 aa 55 aa 55 aa 55 aa cd 63 eb ab 24" },
};

// --- identifier layouts, measured on the wire -------------------------------
//
// The captured frames above are ALL extended, so they say nothing about how a
// standard identifier is carried -- and it is not carried the same way. These
// pairs were measured: each identifier was transmitted from a PCAN-USB Pro FD
// on the same bus and the raw wire word read back off a real UTC.
//
// Extended: bit 31 set, 29-bit identifier in bits 0..28.
// Standard: bit 30 set, 11-bit identifier in bits 18..28 -- left-aligned,
// because those are the top eleven bits of the same arbitration field.
struct IdentifierCase
{
    uint32_t busId;
    bool extended;
    uint32_t wireId;
};

inline constexpr IdentifierCase kIdentifiers[] = {
    { 0x001, false, 0x40040000 },
    { 0x100, false, 0x44000000 },
    { 0x200, false, 0x48000000 },
    { 0x7FF, false, 0x5FFC0000 },
    { 0x1ABCDEF, true, 0x81ABCDEF },
    { 0x1FFFFFFF, true, 0x9FFFFFFF },
    { 0x0001800, true, 0x80001800 },
    { 0x1CF81BEC, true, 0x9CF81BEC }, // the one the captures also contain
};

} // namespace can::motec::golden

#endif // CAN_MOTEC_GOLDEN_UTC_FRAMES_H
