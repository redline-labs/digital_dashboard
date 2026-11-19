#include "e200/spe.h"

#include <bit>
#include <cfenv>
#include <cmath>

#include "e200/cpu_state.h"

namespace sim::e200::spe {

namespace {

constexpr uint32_t kSign = 0x80000000;
constexpr uint32_t kExp = 0x7F800000;
constexpr uint32_t kFrac = 0x007FFFFF;
constexpr uint32_t kMax = 0x7F7FFFFF;  // the largest finite magnitude

bool special(uint32_t f) {
    return (f & kExp) == kExp;  // infinity or NaN
}
bool denormal(uint32_t f) {
    return (f & kExp) == 0 && (f & kFrac) != 0;
}
bool is_nan(uint32_t f) {
    return (f & kExp) == kExp && (f & kFrac) != 0;
}

// A denormal operand is a zero of its sign.
float operand(uint32_t f) {
    return std::bit_cast<float>(denormal(f) ? (f & kSign) : f);
}

// Records this operation's status: the non-sticky bits are replaced, the sticky ones accumulate.
void status(uint32_t* spefscr, uint32_t bits) {
    constexpr uint32_t kNow = kSpefscrFinv | kSpefscrFdbz | kSpefscrFunf | kSpefscrFovf | kSpefscrFx;
    uint32_t sticky = 0;
    if (bits & kSpefscrFinv) {
        sticky |= kSpefscrFinvs;
    }
    if (bits & kSpefscrFdbz) {
        sticky |= kSpefscrFdbzs;
    }
    if (bits & kSpefscrFunf) {
        sticky |= kSpefscrFunfs;
    }
    if (bits & kSpefscrFovf) {
        sticky |= kSpefscrFovfs;
    }
    if (bits & kSpefscrFx) {
        sticky |= kSpefscrFxs;
    }
    *spefscr = (*spefscr & ~kNow) | bits | sticky;
}

// What the host's arithmetic reported about one operation.
struct HostFlags {
    bool inexact = false;
    bool underflow = false;
};

// Runs `op` under the rounding mode FRMC selects, and reports whether it was inexact or underflowed.
template <class Op>
auto rounded(uint32_t spefscr, HostFlags* flags, Op&& op) {
    static const int kModes[4] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
    const int mode = kModes[spefscr & kSpefscrFrmc];
    const int saved = std::fegetround();
    if (mode != saved) {
        std::fesetround(mode);
    }
    std::feclearexcept(FE_INEXACT | FE_UNDERFLOW);
    volatile auto result = op();  // volatile: computed here, under this mode
    flags->inexact = std::fetestexcept(FE_INEXACT) != 0;
    flags->underflow = std::fetestexcept(FE_UNDERFLOW) != 0;
    if (mode != saved) {
        std::fesetround(saved);
    }
    return result;
}

// The arithmetic's result fixed up to what the SPE gives: no infinities, NaNs or denormals out.
uint32_t finish(float value, bool invalid, bool divide_by_zero, HostFlags host, uint32_t* spefscr) {
    uint32_t bits = std::bit_cast<uint32_t>(value);
    uint32_t flags = 0;
    if (invalid) {
        bits = (bits & kSign) | kMax;
        flags |= kSpefscrFinv;
    } else if (divide_by_zero) {
        bits = (bits & kSign) | kMax;
        flags |= kSpefscrFdbz;
    } else if (special(bits)) {
        bits = (bits & kSign) | kMax;
        flags |= kSpefscrFovf | kSpefscrFx;
    } else if (host.underflow || ((bits & kExp) == 0 && (bits & kFrac) != 0)) {
        bits &= kSign;
        flags |= kSpefscrFunf | kSpefscrFx;
    } else if (host.inexact) {
        flags |= kSpefscrFx;
    }
    status(spefscr, flags);
    return bits;
}

template <class Op>
uint32_t arith(uint32_t a, uint32_t b, uint32_t* spefscr, Op&& op) {
    const bool invalid = special(a) || special(b);
    const float x = operand(a);
    const float y = operand(b);
    HostFlags host;
    const float r = rounded(*spefscr, &host, [&] { return op(x, y); });
    return finish(r, invalid, false, host, spefscr);
}

bool compare_invalid(uint32_t a, uint32_t b, uint32_t* spefscr) {
    const bool invalid = special(a) || special(b) || denormal(a) || denormal(b);
    status(spefscr, invalid ? kSpefscrFinv : 0);
    return invalid;
}

uint32_t from_double(double v, uint32_t* spefscr) {
    HostFlags host;
    const float r = rounded(*spefscr, &host, [&] { return float(v); });
    return finish(r, false, false, host, spefscr);
}

// Rounds `v` to an integer by FRMC (or toward zero), saturating to [lo, hi]. NaN is 0; out of range or an invalid
// operand sets FINV.
double to_integer(uint32_t f, bool toward_zero, double lo, double hi, double scale, uint32_t* spefscr) {
    if (is_nan(f)) {
        status(spefscr, kSpefscrFinv);
        return 0;
    }
    const double v = double(operand(f)) * scale;
    double r;
    bool inexact = false;
    if (toward_zero) {
        r = std::trunc(v);
        inexact = r != v;
    } else {
        HostFlags host;
        r = rounded(*spefscr, &host, [&] { return std::nearbyint(v); });
        inexact = r != v;
    }
    uint32_t flags = inexact ? kSpefscrFx : 0;
    if (special(f) || r < lo || r > hi) {
        flags = kSpefscrFinv;
        r = r < lo ? lo : hi;
        if (special(f)) {
            r = (f & kSign) ? lo : hi;
        }
    }
    status(spefscr, flags);
    return r;
}

}  // namespace

uint32_t add(uint32_t a, uint32_t b, uint32_t* spefscr) {
    return arith(a, b, spefscr, [](float x, float y) { return x + y; });
}
uint32_t sub(uint32_t a, uint32_t b, uint32_t* spefscr) {
    return arith(a, b, spefscr, [](float x, float y) { return x - y; });
}
uint32_t mul(uint32_t a, uint32_t b, uint32_t* spefscr) {
    return arith(a, b, spefscr, [](float x, float y) { return x * y; });
}

uint32_t div(uint32_t a, uint32_t b, uint32_t* spefscr) {
    const float x = operand(a);
    const float y = operand(b);
    const bool invalid = special(a) || special(b) || (x == 0 && y == 0);
    const bool by_zero = !invalid && y == 0;
    HostFlags host;
    const float r = rounded(*spefscr, &host, [&] { return x / y; });
    if (invalid && x == 0 && y == 0) {
        // 0/0: the sign is the exclusive or of the operands'
        return finish(std::bit_cast<float>((a ^ b) & kSign), true, false, HostFlags{}, spefscr);
    }
    return finish(r, invalid, by_zero, host, spefscr);
}

bool cmp_gt(uint32_t a, uint32_t b, uint32_t* spefscr) {
    compare_invalid(a, b, spefscr);
    return tst_gt(a, b);
}
bool cmp_lt(uint32_t a, uint32_t b, uint32_t* spefscr) {
    compare_invalid(a, b, spefscr);
    return tst_lt(a, b);
}
bool cmp_eq(uint32_t a, uint32_t b, uint32_t* spefscr) {
    compare_invalid(a, b, spefscr);
    return tst_eq(a, b);
}
bool tst_gt(uint32_t a, uint32_t b) {
    return operand(a) > operand(b);
}
bool tst_lt(uint32_t a, uint32_t b) {
    return operand(a) < operand(b);
}
bool tst_eq(uint32_t a, uint32_t b) {
    return operand(a) == operand(b);
}

uint32_t from_signed(int32_t v, uint32_t* spefscr) {
    return from_double(double(v), spefscr);
}
uint32_t from_unsigned(uint32_t v, uint32_t* spefscr) {
    return from_double(double(v), spefscr);
}
uint32_t from_signed_fraction(int32_t v, uint32_t* spefscr) {
    return from_double(std::ldexp(double(v), -31), spefscr);
}
uint32_t from_unsigned_fraction(uint32_t v, uint32_t* spefscr) {
    return from_double(std::ldexp(double(v), -32), spefscr);
}

uint32_t to_signed(uint32_t f, bool round_toward_zero, uint32_t* spefscr) {
    return uint32_t(int32_t(to_integer(f, round_toward_zero, -2147483648.0, 2147483647.0, 1.0, spefscr)));
}
uint32_t to_unsigned(uint32_t f, bool round_toward_zero, uint32_t* spefscr) {
    return uint32_t(to_integer(f, round_toward_zero, 0.0, 4294967295.0, 1.0, spefscr));
}
uint32_t to_signed_fraction(uint32_t f, uint32_t* spefscr) {
    return uint32_t(int32_t(to_integer(f, false, -2147483648.0, 2147483647.0, 2147483648.0, spefscr)));
}
uint32_t to_unsigned_fraction(uint32_t f, uint32_t* spefscr) {
    return uint32_t(to_integer(f, false, 0.0, 4294967295.0, 4294967296.0, spefscr));
}

}  // namespace sim::e200::spe
