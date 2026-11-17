// SPDX-License-Identifier: GPL-3.0-or-later
//
// Deliberately destroying frames on the bus, for testing how the other nodes
// cope with errors.
//
// The adapter does it by driving six consecutive dominant bits onto the bus at
// a chosen bit of a frame. Six of the same level is a stuff error to every
// node listening, so each of them flags an error frame, bumps its error
// counters, and the transmitter retries. Destroying enough frames walks the
// nodes up through error-warning and error-passive to bus-off, which is what
// this is for -- and is exactly why it must never be pointed at a bus you do
// not own. Every node on the bus sees it, not just the one being tested.
//
// Few adapters can do this at all. A channel that cannot answers Unsupported;
// see Channel::set_error_injection().
#ifndef CAN_ERROR_INJECTION_H
#define CAN_ERROR_INJECTION_H

#include "can/error.h"

#include <cstdint>

namespace can
{

struct ErrorInjection
{
    enum class Mode
    {
        // Stop injecting. Always accepted by a channel that supports injection
        // at all, including one already off.
        Off,
        // Destroy the next frame on the bus, whatever its identifier. Once.
        NextFrame,
        // Destroy frames with one identifier, in a cycle: `passCount` let
        // through, then `destroyCount` destroyed, repeated `repeatCount` times
        // (0 = until turned off). A retransmission counts as a frame, and
        // with `passCount` 0 every attempt is destroyed: the sender goes
        // bus-off.
        Targeted,
    };

    Mode mode { Mode::Off };

    // Where in the frame the dominant bits start, counted in bits. The PCAN
    // field is ten bits wide. Whether it counts from start-of-frame, and
    // whether stuff bits count, has not been checked against a scope.
    uint16_t bitPosition { 0 };

    // Targeted only. 29 bits are carried; an 11-bit identifier is given as
    // is (0x456 matched 0x456 on a PCAN).
    uint32_t identifier { 0 };
    uint16_t destroyCount { 1 };
    uint16_t passCount { 0 };
    uint8_t repeatCount { 0 };

    static ErrorInjection off() { return ErrorInjection {}; }
    static ErrorInjection next_frame(uint16_t bitPosition);
    static ErrorInjection targeted(uint32_t identifier, uint16_t bitPosition,
                                   uint16_t destroyCount, uint16_t passCount = 0,
                                   uint8_t repeatCount = 0);
};

// The widest field any adapter here carries. Checked before a backend sees
// the request, so an out-of-range value is refused rather than masked into a
// different, plausible one.
inline constexpr uint16_t kMaxErrorInjectionBitPosition = 0x3FF;

// InvalidArgument when the request cannot mean what it says: a bit position or
// identifier wider than the field, or a targeted cycle that destroys nothing.
Result<void> validate(const ErrorInjection& injection);

const char* to_string(ErrorInjection::Mode mode);

} // namespace can

#endif // CAN_ERROR_INJECTION_H
