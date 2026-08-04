// SPDX-License-Identifier: GPL-3.0-or-later
//
// The CAN FD length code, which is not a length.
//
// Classic CAN puts the payload length straight in the DLC field, 0 to 8. CAN FD
// reuses the same four bits for nine more sizes, and the mapping is not linear:
//
//     DLC   0 1 2 3 4 5 6 7 8  9 10 11 12 13 14 15
//     bytes 0 1 2 3 4 5 6 7 8 12 16 20 24 32 48 64
//
// So a payload of, say, 10 bytes cannot be sent as-is. It goes out as DLC 9 --
// twelve bytes -- with the last two undefined, and the receiver has no way to
// know the sender meant ten. Every FD driver has to round up somewhere, and
// getting this wrong shows up as trailing rubbish on the wire rather than as an
// error, which is why it is a named function with a test rather than a lookup
// inlined into a codec.
#ifndef CAN_DLC_H
#define CAN_DLC_H

#include <cstdint>

namespace can
{

// Payload bytes for a DLC. `fd` false clamps to 8, as classic CAN does with
// the DLC values above 8 that it permits but does not use.
uint8_t dlc_to_length(uint8_t dlc, bool fd);

// The DLC that carries at least `length` bytes. A length that is not one of
// the representable sizes rounds up to the next one; 65 or more clamps to 64.
uint8_t length_to_dlc(uint8_t length, bool fd);

// Whether a length is exactly representable, so a caller can refuse rather
// than silently padding.
bool is_valid_can_length(uint8_t length, bool fd);

// The next representable size at or above `length`. This is what a payload
// actually occupies on the wire.
uint8_t round_up_can_length(uint8_t length, bool fd);

} // namespace can

#endif // CAN_DLC_H
