// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two fixed-point formats, and the cross-check that catches the one bug in
// this library that nothing else would.
//
// XBus lets the same physical quantity be requested in four precisions,
// selected by two bits of its data identifier. Three of them are ordinary.
// The fourth, fixed point 16.32, sends the low six bytes of round(v * 2^32)
// in the order
//
//     b3 b2 b1 b0 b5 b4
//
// -- the 32-bit fractional part first, then the 16-bit integer part. Reading
// those six bytes as one big-endian integer compiles, runs, and returns a
// plausible number for every value: the vectors below show 9.81 m/s^2 reading
// back as -12451.84, which is wrong by enough to notice but not by enough to
// crash anything.
//
// WHAT MAKES THIS HARD TO TEST is that round-tripping does not catch it. An
// encoder and a decoder that share the same wrong byte order agree perfectly,
// and the test passes. So the assertion that matters is testDualEncoding()
// below: take one value, encode it BOTH as float32 -- a format with no room
// for interpretation -- and as fp16.32, decode both, and require them to
// agree. There is no byte order the two can be wrong in together.
//
// The float32 vector is not ours either: the LLCP prints 9.81 as 0x411CF5C3 in
// section 5.1.1, as its worked example of big-endian output.

#include "xbus/byte_order.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cmath>
#include <cstdint>
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

using namespace xbus;

template <std::size_t N>
constexpr std::span<const std::uint8_t> bytes(const std::array<std::uint8_t, N>& a)
{
    return std::span<const std::uint8_t>(a.data(), a.size());
}

constexpr bool near(double a, double b, double tolerance)
{
    const double d = a - b;
    return (d < 0 ? -d : d) <= tolerance;
}

// ============================================================================
// Compile-time: fixed point 12.20
//
// int32_t fixedPointValue12p20 = round(floatingPointValue * 2^20), big-endian.
// ============================================================================

static_assert(read_fp1220(std::array<std::uint8_t, 4> { 0x00, 0x10, 0x00, 0x00 }, 0) == 1.0);
static_assert(read_fp1220(std::array<std::uint8_t, 4> { 0xFF, 0xF0, 0x00, 0x00 }, 0) == -1.0);
static_assert(read_fp1220(std::array<std::uint8_t, 4> { 0x00, 0x04, 0x00, 0x00 }, 0) == 0.25);
static_assert(read_fp1220(std::array<std::uint8_t, 4> { 0x00, 0x00, 0x00, 0x00 }, 0) == 0.0);

// The sign has to come from the 32-bit integer, not from a zero-extension: an
// accelerometer that only ever reported positive values would pass every other
// check here.
static_assert(read_fp1220(std::array<std::uint8_t, 4> { 0xFF, 0xFF, 0xFF, 0xFF }, 0) < 0.0,
              "0xFFFFFFFF is -1 ulp, not +4095.999999");

// ============================================================================
// Compile-time: fixed point 16.32
// ============================================================================

// 1.0 is a clean separation of the two halves: fraction zero, integer one.
static_assert(read_fp1632(std::array<std::uint8_t, 6> { 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 }, 0) == 1.0,
              "the integer part is the LAST two bytes, not the first");

// -1.0. The integer half must be sign-extended from bit 15; zero-filling it
// would read +65535.0 here, which is exactly the kind of number that looks
// like a sensor fault rather than a parser bug.
static_assert(read_fp1632(std::array<std::uint8_t, 6> { 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF }, 0) == -1.0,
              "the 48-bit value is signed");

// 0.5 is the vector that proves the fractional half is read big-endian and is
// read FIRST. Under a naive six-byte big-endian read this same input decodes
// to -32768.0.
static_assert(read_fp1632(std::array<std::uint8_t, 6> { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0) == 0.5);

static_assert(read_fp1632(std::array<std::uint8_t, 6> { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0) == 0.0);

// 9.81 m/s^2, the acceleration the LLCP itself uses as an example.
static_assert(near(read_fp1632(std::array<std::uint8_t, 6> { 0xCF, 0x5C, 0x28, 0xF6, 0x00, 0x09 }, 0),
                   9.81, 1e-9),
              "9.81 must decode as 9.81");

// And the statement that gives this file its teeth: those same six bytes read
// as one big-endian 48-bit quantity are nowhere near 9.81.
constexpr double naiveBigEndian48(std::span<const std::uint8_t> b, std::size_t offset)
{
    std::int64_t value = 0;
    for (std::size_t i = 0; i < 6; ++i)
    {
        value = (value << 8) | static_cast<std::int64_t>(b[offset + i]);
    }
    if ((value & (std::int64_t { 1 } << 47)) != 0)
    {
        value -= (std::int64_t { 1 } << 48);
    }
    return static_cast<double>(value) / 4294967296.0;
}

static_assert(
    !near(naiveBigEndian48(std::array<std::uint8_t, 6> { 0xCF, 0x5C, 0x28, 0xF6, 0x00, 0x09 }, 0),
          9.81, 1.0),
    "if this ever becomes true the swizzle stopped mattering and this file is pointless");

// ============================================================================
// Compile-time: float32, against the LLCP's own printed bytes
// ============================================================================

// LLCP 5.1.1: "Calibrated accelerometer output (float, 4 bytes) 9.81 (decimal)
// = 0x411CF5C3 (hexadecimal). Transmission order of bytes = 0x41 0x1C 0xF5
// 0xC3." Vendor bytes, and the only thing here that did not come from a
// reading of the spec.
static_assert(near(static_cast<double>(read_f32(std::array<std::uint8_t, 4> { 0x41, 0x1C, 0xF5, 0xC3 }, 0)),
                   9.81, 1e-6),
              "the LLCP's own float32 example must decode to 9.81");

// And the 16-bit example from the same section: 1275 decimal = 0x04FB.
static_assert(read_u16(std::array<std::uint8_t, 2> { 0x04, 0xFB }, 0) == 1275,
              "the LLCP's own uint16 example");

// ============================================================================
// Run time: the dual-encoding cross-check
// ============================================================================

void testDualEncoding()
{
    // The whole argument of this file. For each value, encode it in every
    // format the protocol offers and require the decodes to agree. A byte
    // order that is wrong in one format cannot also be wrong in the same way
    // in float32, which has exactly one legal layout and a vendor vector
    // behind it.
    //
    // Tolerances are each format's resolution, not a fudge: fp12.20 resolves
    // to 2^-20 and fp16.32 to 2^-32, and float32 carries about seven
    // significant digits.
    const double values[] = { 0.0,   1.0,    -1.0,     0.5,    -0.5,   9.81,
                              -9.80665, 0.001, -0.001, 123.456, -123.456,
                              2000.0, -2000.0 };

    for (double v : values)
    {
        std::array<std::uint8_t, 4> asFloat32 {};
        std::array<std::uint8_t, 4> asFp1220 {};
        std::array<std::uint8_t, 6> asFp1632 {};

        write_f32(asFloat32, 0, static_cast<float>(v));
        write_fp1220(asFp1220, 0, v);
        write_fp1632(asFp1632, 0, v);

        const double f32 = static_cast<double>(read_f32(asFloat32, 0));
        const double fp1220 = read_fp1220(asFp1220, 0);
        const double fp1632 = read_fp1632(asFp1632, 0);

        const double scale = std::abs(v) < 1.0 ? 1.0 : std::abs(v);

        check(near(fp1632, f32, scale * 1e-6),
              "fp16.32 and float32 must decode the same value: " + std::to_string(v));
        check(near(fp1220, f32, scale * 1e-5),
              "fp12.20 and float32 must decode the same value: " + std::to_string(v));

        // The negative control. If the swizzle were dropped, this is the
        // comparison that would start passing.
        if (std::abs(v) > 1.0)
        {
            check(!near(naiveBigEndian48(asFp1632, 0), f32, scale * 1e-6),
                  "a naive big-endian read must NOT agree: " + std::to_string(v));
        }
    }
}

void testRangeLimits()
{
    // The LLCP's stated ranges. Values outside them are not representable and
    // the device will not send them, but the boundaries are worth pinning: a
    // sign-extension bug shows up at exactly one end.
    std::array<std::uint8_t, 6> buffer {};

    write_fp1632(buffer, 0, 32767.5);
    check(near(read_fp1632(buffer, 0), 32767.5, 1e-6), "fp16.32 near its positive limit");

    write_fp1632(buffer, 0, -32768.0);
    check(near(read_fp1632(buffer, 0), -32768.0, 1e-6), "fp16.32 at its negative limit");

    std::array<std::uint8_t, 4> small {};
    write_fp1220(small, 0, 2047.5);
    check(near(read_fp1220(small, 0), 2047.5, 1e-5), "fp12.20 near its positive limit");

    write_fp1220(small, 0, -2048.0);
    check(near(read_fp1220(small, 0), -2048.0, 1e-5), "fp12.20 at its negative limit");
}

void testResolution()
{
    // One ulp of each format, which is what the tolerances above are chosen
    // against. fp16.32 resolving to 2^-32 is the reason it exists: it is finer
    // than float32 anywhere above 1.0, which is where an accelerometer lives.
    std::array<std::uint8_t, 6> buffer {};
    write_fp1632(buffer, 0, 9.81 + 1e-8);
    const double a = read_fp1632(buffer, 0);
    write_fp1632(buffer, 0, 9.81);
    const double b = read_fp1632(buffer, 0);

    check(a != b, "fp16.32 resolves a difference float32 would lose at this magnitude");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testDualEncoding();
    testRangeLimits();
    testResolution();

    return failures == 0 ? 0 : 1;
}
