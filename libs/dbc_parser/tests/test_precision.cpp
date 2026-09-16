// Every raw value of the type-boundary signals, through decode and back.
//
// The golden replay samples random frames; this walks each float and small
// integer field exhaustively, because the claim the generator's type rule rests
// on is "every raw value round-trips", and a claim about every value is only
// tested by every value. Each float value is also recomputed with an explicit
// fused multiply-add, which is what a compiler contracting raw * scale + offset
// produces on a target that has one, so the result does not depend on the host.
//
// The second half pins what cantools cannot vouch for: it rounds half to even
// and raises outside the field, where the generated code rounds half away from
// zero and saturates. Every other hand-written expectation below was confirmed
// against cantools 44.0.0 when it was written.

#include "dbc_test_precision.h"

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <type_traits>

using namespace dbc_test_precision;

namespace
{

int failures = 0;

void check(bool condition, const char *what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what);
        failures += 1;
    }
}

// ---------------------------------------------------------------- the types

using P0 = Precision0_t;
using P1 = Precision1_t;
using P2 = Precision2_t;
using P3 = Precision3_t;
using P4 = Precision4_t;
using P5 = Precision5_t;
using P6 = Precision6_t;

static_assert(std::is_same_v<P0::sig_F20U_Tenth_t::Type, float>);
static_assert(std::is_same_v<P0::sig_F12U_QuarterOffset_t::Type, float>);
static_assert(std::is_same_v<P0::sig_F16S_Milli_t::Type, float>);
static_assert(std::is_same_v<P1::sig_F16U_NegHalf_t::Type, float>);
static_assert(std::is_same_v<P1::sig_F8U_Kelvin_t::Type, float>);
static_assert(std::is_same_v<P1::sig_D21S_Tenth_t::Type, double>);
static_assert(std::is_same_v<P2::sig_D20U_HalfHalf_t::Type, double>);
static_assert(std::is_same_v<P2::sig_D12U_QuarterOffset_t::Type, double>);
static_assert(std::is_same_v<P3::sig_D24U_Centi_t::Type, double>);
static_assert(std::is_same_v<P3::sig_D32S_CentiOffset_t::Type, double>);
static_assert(std::is_same_v<P4::sig_I32U_Times4_t::Type, uint64_t>);
static_assert(std::is_same_v<P5::sig_I32S_NegThreeOffset_t::Type, int64_t>);
static_assert(std::is_same_v<P5::sig_I16U_Thousand_t::Type, uint32_t>);
static_assert(std::is_same_v<P5::sig_I8S_NegTwo_t::Type, int16_t>);
static_assert(std::is_same_v<P5::sig_I3S_FiveOffset_t::Type, int8_t>);
static_assert(std::is_same_v<P5::sig_B1U_Flag_t::Type, bool>);
static_assert(std::is_same_v<P5::sig_I1U_Offset_t::Type, uint8_t>);
static_assert(std::is_same_v<P6::sig_IeeeScaled_t::Type, double>);

// The round trips are constant expressions too.
static_assert(to_raw(P1::sig_F16U_NegHalf_t{}, from_raw(P1::sig_F16U_NegHalf_t{}, 12345u)) == 12345u);
static_assert(to_raw(P5::sig_I8S_NegTwo_t{}, from_raw(P5::sig_I8S_NegTwo_t{}, 0x80u)) == 0x80u);

// ------------------------------------------------------------- round trips

template <typename Sig>
constexpr int64_t rawLo()
{
    return Sig::is_signed ? -(int64_t{1} << (Sig::length - 1u)) : 0;
}

template <typename Sig>
constexpr int64_t rawHi()
{
    return Sig::is_signed ? ((int64_t{1} << (Sig::length - 1u)) - 1) : ((int64_t{1} << Sig::length) - 1);
}

template <typename Sig>
typename Sig::Raw bitsOf(int64_t raw)
{
    const uint64_t mask = (uint64_t{1} << Sig::length) - 1u;
    return static_cast<typename Sig::Raw>(static_cast<uint64_t>(raw) & mask);
}

struct Tally
{
    const char *name;
    uint64_t checked{};
    uint64_t failed{};
    int64_t firstBad{};
    const char *firstPath{};
};

void record(Tally &tally, int64_t raw, const char *path)
{
    if (tally.failed == 0)
    {
        tally.firstBad = raw;
        tally.firstPath = path;
    }
    tally.failed += 1;
}

template <typename Sig>
void roundTrip(Tally &tally, int64_t raw)
{
    const auto bits = bitsOf<Sig>(raw);
    const auto value = from_raw(Sig{}, bits);
    tally.checked += 1;
    if (to_raw(Sig{}, value) != bits)
    {
        record(tally, raw, "decode");
    }

    // The same value as a contracting compiler computes it. Deterministic on
    // every host, so the tolerance is proven rather than hoped for.
    if constexpr (Sig::domain == value_domain::Float)
    {
        const float contracted = std::fma(static_cast<float>(raw), Sig::scale, Sig::offset);
        tally.checked += 1;
        if (to_raw(Sig{}, contracted) != bits)
        {
            record(tally, raw, "fused multiply-add");
        }
    }
    else if constexpr (Sig::domain == value_domain::Double)
    {
        const double contracted = std::fma(static_cast<double>(raw), Sig::scale, Sig::offset);
        tally.checked += 1;
        if (to_raw(Sig{}, contracted) != bits)
        {
            record(tally, raw, "fused multiply-add");
        }
    }
}

// Every raw value up to 2^21 of them; beyond that both ends of the field,
// where the precision runs out, and a fixed-seed sample of the middle.
template <typename Sig>
void sweep(const char *name)
{
    Tally tally{name};
    const int64_t lo = rawLo<Sig>();
    const int64_t hi = rawHi<Sig>();
    const uint64_t count = static_cast<uint64_t>(hi - lo) + 1u;

    if (count <= (uint64_t{1} << 21))
    {
        for (int64_t raw = lo; raw <= hi; ++raw)
        {
            roundTrip<Sig>(tally, raw);
        }
    }
    else
    {
        constexpr int64_t kEnd = 65536;
        for (int64_t raw = lo; raw < lo + kEnd; ++raw)
        {
            roundTrip<Sig>(tally, raw);
        }
        for (int64_t raw = hi - kEnd + 1; raw <= hi; ++raw)
        {
            roundTrip<Sig>(tally, raw);
        }
        std::mt19937_64 rng(Sig::start_bit + Sig::length);
        std::uniform_int_distribution<int64_t> pick(lo, hi);
        for (int i = 0; i < 65536; ++i)
        {
            roundTrip<Sig>(tally, pick(rng));
        }
    }

    if (tally.failed != 0)
    {
        std::fprintf(stderr, "FAIL: %s: %llu of %llu round trips lost their raw value, first raw %lld via %s\n",
                     name, static_cast<unsigned long long>(tally.failed),
                     static_cast<unsigned long long>(tally.checked), static_cast<long long>(tally.firstBad),
                     tally.firstPath);
        failures += 1;
    }
    else
    {
        std::printf("%-24s %9llu round trips\n", name, static_cast<unsigned long long>(tally.checked));
    }
}

// --------------------------------------------------------------- encodings

template <typename Sig>
void expectRaw(const char *what, typename Sig::Type value, uint64_t expected)
{
    const auto bits = static_cast<uint64_t>(to_raw(Sig{}, value));
    if (bits != expected)
    {
        std::fprintf(stderr, "FAIL: %s: encoded raw 0x%llx, expected 0x%llx\n", what,
                     static_cast<unsigned long long>(bits), static_cast<unsigned long long>(expected));
        failures += 1;
    }
}

// In range and nowhere near a tie, so cantools agrees; checked against
// cantools 44.0.0 on dbc_test_precision.dbc.
void testEncodingsCantoolsAgreesWith()
{
    expectRaw<P1::sig_F16U_NegHalf_t>("negative scale, one step", 99.5f, 1u);
    expectRaw<P1::sig_F16U_NegHalf_t>("negative scale, far end", -32667.5f, 65535u);
    expectRaw<P1::sig_F16U_NegHalf_t>("negative scale, zero", 100.0f, 0u);
    expectRaw<P0::sig_F16S_Milli_t>("signed float, most negative", -32.768f, 0x8000u);
    expectRaw<P0::sig_F16S_Milli_t>("signed float, rounds down", 1.2344f, 1234u);
    expectRaw<P1::sig_F8U_Kelvin_t>("fractional offset, top", -145.65f, 255u);
    expectRaw<P0::sig_F20U_Tenth_t>("20 bit float, top", 104857.5f, 1048575u);
    expectRaw<P5::sig_I8S_NegTwo_t>("negative even scale, top of range", 256, 0x80u);
    expectRaw<P5::sig_I8S_NegTwo_t>("negative even scale, bottom of range", -254, 0x7Fu);
    expectRaw<P5::sig_I3S_FiveOffset_t>("3 bit signed, bottom", -21, 0x4u);
    expectRaw<P5::sig_I3S_FiveOffset_t>("3 bit signed, top", 14, 0x3u);
    expectRaw<P5::sig_I16U_Thousand_t>("scale 1000, top", 65535000u, 0xFFFFu);
    expectRaw<P5::sig_I16U_Thousand_t>("scale 1000, rounds down", 1400u, 1u);
    expectRaw<P4::sig_I32U_Times4_t>("scale 4, top", 17179869180u, 0xFFFFFFFFu);
    expectRaw<P4::sig_I32U_Times4_t>("scale 4, rounds up", 7u, 2u);
    expectRaw<P5::sig_I1U_Offset_t>("one bit with an offset", 6u, 1u);
    expectRaw<P3::sig_D32S_CentiOffset_t>("32 bit double, bottom", -21474841.48, 0x80000000u);
    expectRaw<P3::sig_D24U_Centi_t>("24 bit double, top", 167772.15, 0xFFFFFFu);
    expectRaw<P5::sig_I32S_NegThreeOffset_t>("negative odd scale, zero", 7, 0u);
    expectRaw<P5::sig_I32S_NegThreeOffset_t>("negative odd scale, top", -6442450934, 0x7FFFFFFFu);
}

// A hand-built signal with a scale of -4, so a negative even scale can be
// pinned at an exact tie. Only the traits to_raw() reads for an integer.
struct NegativeFour
{
    static constexpr uint32_t length = 8u;
    static constexpr value_domain domain = value_domain::Integer;
    static constexpr bool identity = false;
    using Raw = uint8_t;
    using Type = int16_t;
    using Work = int32_t;
    static constexpr int32_t scale = -4;
    static constexpr int32_t offset = 0;
    static constexpr int16_t phys_min = -508;
    static constexpr int16_t phys_max = 512;

    // This fixture measures rounding, not railing, so the declared range is
    // inert: rail() needs these members to exist, and has_range keeps it a
    // no-op so every value below still reaches the arithmetic under test.
    static constexpr int16_t minimum = 0;
    static constexpr int16_t maximum = 0;
    static constexpr bool has_range = false;
};

// Exact ties. cantools would round these half to even.
void testTiesRoundAwayFromZero()
{
    expectRaw<P5::sig_I8S_NegTwo_t>("3 / -2 = -1.5 rounds to -2", 3, 0xFEu);
    expectRaw<P5::sig_I8S_NegTwo_t>("-3 / -2 = 1.5 rounds to 2", -3, 0x02u);
    expectRaw<P5::sig_I8S_NegTwo_t>("1 / -2 = -0.5 rounds to -1", 1, 0xFFu);
    expectRaw<P5::sig_I16U_Thousand_t>("1.5 rounds to 2", 1500u, 2u);
    expectRaw<P5::sig_I16U_Thousand_t>("2.5 rounds to 3", 2500u, 3u);
    expectRaw<P4::sig_I32U_Times4_t>("0.5 rounds to 1", 2u, 1u);
    expectRaw<P4::sig_I32U_Times4_t>("2.5 rounds to 3", 10u, 3u);

    check(dbc_test_precision::to_raw(NegativeFour{}, int16_t{2}) == 0xFFu, "2 / -4 = -0.5 rounds to -1");
    check(dbc_test_precision::to_raw(NegativeFour{}, int16_t{6}) == 0xFEu, "6 / -4 = -1.5 rounds to -2");
    check(dbc_test_precision::to_raw(NegativeFour{}, int16_t{-2}) == 0x01u, "-2 / -4 = 0.5 rounds to 1");
    check(dbc_test_precision::to_raw(NegativeFour{}, int16_t{-10}) == 0x03u, "-10 / -4 = 2.5 rounds to 3");
    check(dbc_test_precision::to_raw(NegativeFour{}, int16_t{5}) == 0xFFu, "5 / -4 = -1.25 rounds to -1");

    using dbc_test_precision::dbc_detail::round_saturate;
    check(round_saturate<float, int32_t>(2.5f, -100, 100) == 3, "float 2.5 rounds to 3");
    check(round_saturate<float, int32_t>(-2.5f, -100, 100) == -3, "float -2.5 rounds to -3");
    check(round_saturate<float, int32_t>(1.5f, -100, 100) == 2, "float 1.5 rounds to 2");
    check(round_saturate<double, int64_t>(4503599627370495.5, 0, std::numeric_limits<int64_t>::max()) ==
              4503599627370496,
          "double 2^52 - 0.5 rounds to 2^52");

    // The helper this replaced added 0.5 before truncating, and the sum rounds
    // up to 1.0 for the float just below a half.
    check(round_saturate<float, int32_t>(0.49999997f, -100, 100) == 0, "float just below 0.5 rounds to 0");
    check(round_saturate<float, int32_t>(-0.49999997f, -100, 100) == 0, "float just above -0.5 rounds to 0");
    check(round_saturate<double, int32_t>(0.49999999999999994, -100, 100) == 0,
          "double just below 0.5 rounds to 0");
}

// Outside the field. cantools raises here.
void testSaturation()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    expectRaw<P0::sig_F16S_Milli_t>("float above the field", 40.0f, 0x7FFFu);
    expectRaw<P0::sig_F16S_Milli_t>("float below the field", -40.0f, 0x8000u);
    expectRaw<P0::sig_F16S_Milli_t>("NaN goes to the bottom", nan, 0x8000u);
    expectRaw<P0::sig_F16S_Milli_t>("+inf goes to the top", inf, 0x7FFFu);
    expectRaw<P0::sig_F16S_Milli_t>("-inf goes to the bottom", -inf, 0x8000u);
    expectRaw<P0::sig_F16S_Milli_t>("FLT_MAX goes to the top", FLT_MAX, 0x7FFFu);
    expectRaw<P0::sig_F20U_Tenth_t>("negative into an unsigned field", -1.0f, 0u);
    expectRaw<P0::sig_F20U_Tenth_t>("huge into a 20 bit field", 1e30f, 0xFFFFFu);

    // A negative scale saturates in raw space, so a physical value above the
    // top of the physical range lands at raw 0.
    expectRaw<P1::sig_F16U_NegHalf_t>("negative scale, above the physical range", 200.0f, 0u);
    expectRaw<P1::sig_F16U_NegHalf_t>("negative scale, below the physical range", -40000.0f, 0xFFFFu);

    expectRaw<P5::sig_I8S_NegTwo_t>("integer above the physical range", 1000, 0x80u);
    expectRaw<P5::sig_I8S_NegTwo_t>("integer below the physical range", -1000, 0x7Fu);
    expectRaw<P5::sig_I16U_Thousand_t>("integer above the physical range", 70000000u, 0xFFFFu);
    expectRaw<P5::sig_I1U_Offset_t>("below a range that starts at 5", 0u, 0u);
    expectRaw<P5::sig_I1U_Offset_t>("above a range that ends at 6", 200u, 1u);

    expectRaw<P3::sig_D24U_Centi_t>("double above the field", 1e12, 0xFFFFFFu);
    expectRaw<P3::sig_D24U_Centi_t>("double NaN", std::numeric_limits<double>::quiet_NaN(), 0u);

    // Less than a step above the top of the field. The old encoder only
    // saturated at 2^length, so these rounded to one past the top and the mask
    // wrapped them to the bottom: 0 for the unsigned field, INT32_MIN for the
    // signed one.
    expectRaw<P3::sig_D24U_Centi_t>("0.8 of a step above the top stays at the top", 167772.158, 0xFFFFFFu);
    expectRaw<P3::sig_D32S_CentiOffset_t>("0.8 of a step above a signed top stays at the top", 21474831.478,
                                          0x7FFFFFFFu);

    using dbc_test_precision::dbc_detail::round_saturate;
    check(round_saturate<float, int32_t>(99.6f, -100, 100) == 100, "rounds up to the top");
    check(round_saturate<float, int32_t>(100.0f, -100, 100) == 100, "at the top");
    check(round_saturate<double, uint64_t>(1.8446744073709552e19, 0u, std::numeric_limits<uint64_t>::max()) ==
              std::numeric_limits<uint64_t>::max(),
          "2^64 saturates a uint64_t rather than converting out of range");
    check(round_saturate<double, uint64_t>(-1.0, 0u, std::numeric_limits<uint64_t>::max()) == 0u,
          "negative saturates a uint64_t at zero");
}

void testBool()
{
    check(to_raw(P5::sig_B1U_Flag_t{}, true) == 1u, "true encodes as 1");
    check(to_raw(P5::sig_B1U_Flag_t{}, false) == 0u, "false encodes as 0");
    check(from_raw(P5::sig_B1U_Flag_t{}, 1u), "1 decodes as true");
}

} // namespace

int main()
{
    sweep<P0::sig_F20U_Tenth_t>("F20U_Tenth");
    sweep<P0::sig_F12U_QuarterOffset_t>("F12U_QuarterOffset");
    sweep<P0::sig_F16S_Milli_t>("F16S_Milli");
    sweep<P1::sig_F16U_NegHalf_t>("F16U_NegHalf");
    sweep<P1::sig_F8U_Kelvin_t>("F8U_Kelvin");

    sweep<P1::sig_D21S_Tenth_t>("D21S_Tenth");
    sweep<P2::sig_D20U_HalfHalf_t>("D20U_HalfHalf");
    sweep<P2::sig_D12U_QuarterOffset_t>("D12U_QuarterOffset");
    sweep<P3::sig_D24U_Centi_t>("D24U_Centi");
    sweep<P3::sig_D32S_CentiOffset_t>("D32S_CentiOffset");

    sweep<P4::sig_I32U_Times4_t>("I32U_Times4");
    sweep<P5::sig_I32S_NegThreeOffset_t>("I32S_NegThreeOffset");
    sweep<P5::sig_I16U_Thousand_t>("I16U_Thousand");
    sweep<P5::sig_I8S_NegTwo_t>("I8S_NegTwo");
    sweep<P5::sig_I3S_FiveOffset_t>("I3S_FiveOffset");
    sweep<P5::sig_B1U_Flag_t>("B1U_Flag");
    sweep<P5::sig_I1U_Offset_t>("I1U_Offset");

    testEncodingsCantoolsAgreesWith();
    testTiesRoundAwayFromZero();
    testSaturation();
    testBool();

    if (failures != 0)
    {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }

    std::printf("precision: all checks passed\n");
    return 0;
}
