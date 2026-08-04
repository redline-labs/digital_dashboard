// SPDX-License-Identifier: GPL-3.0-or-later
//
// SocketCAN's two frame layouts, converted by hand rather than by struct cast.
//
// A CAN_RAW socket carries either `struct can_frame` (16 bytes) or
// `struct canfd_frame` (72 bytes), and which one you get is decided by how many
// bytes the read returned. Both put the identifier in a 32-bit word with three
// flag bits above it:
//
//     bit 31  EFF   the identifier is 29 bits, not 11
//     bit 30  RTR   remote transmission request
//     bit 29  ERR   this is an error report, not a message
//
// Those three bits are the reason this is a hand-written conversion tested on
// macOS rather than a memcpy inside a Linux-only file. Masking the identifier
// with the wrong constant turns a 29-bit frame into a different 11-bit one, and
// missing the error bit turns a bus-off report into traffic from a device that
// does not exist. Neither shows up as a failure; both show up as a decoder
// downstream producing plausible nonsense.
#ifndef CAN_SOCKETCAN_FRAME_H
#define CAN_SOCKETCAN_FRAME_H

#include "can/error.h"

#include "helpers/can_frame.h"

#include <cstdint>
#include <span>

namespace can::socketcan
{

// Sizes of the kernel's two structures. A read returns exactly one of these,
// and the size is the only thing that says which.
inline constexpr size_t kClassicFrameSize = 16;
inline constexpr size_t kFdFrameSize = 72;

inline constexpr uint32_t kEffFlag = 0x80000000u;
inline constexpr uint32_t kRtrFlag = 0x40000000u;
inline constexpr uint32_t kErrFlag = 0x20000000u;
inline constexpr uint32_t kSffMask = 0x000007FFu;
inline constexpr uint32_t kEffMask = 0x1FFFFFFFu;

// canfd_frame::flags
inline constexpr uint8_t kFdFlagBrs = 0x01;
inline constexpr uint8_t kFdFlagEsi = 0x02;

// Writes the frame into `out`, which must be at least kFdFrameSize when the
// frame is FD and kClassicFrameSize otherwise. Returns how many bytes were
// written, which is what the caller passes to write().
Result<size_t> encode_frame(const helpers::CanFrame& frame, std::span<uint8_t> out);

// Reads whichever layout `bytes` holds, deciding by its length.
Result<helpers::CanFrame> decode_frame(std::span<const uint8_t> bytes);

} // namespace can::socketcan

#endif // CAN_SOCKETCAN_FRAME_H
