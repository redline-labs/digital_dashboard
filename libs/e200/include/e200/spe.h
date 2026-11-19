// The SPE's embedded scalar single-precision floating point (efs*), as the e200 computes it.
//
// It is not quite IEEE 754 (SPE PEM, "Embedded Floating-Point"):
//   - a denormalised operand is taken as a zero of its sign;
//   - an infinite or NaN operand is invalid (SPEFSCR[FINV]); the result is then the largest finite value of the IEEE
//     result's sign (pmax / nmax) -- there are no infinities or NaNs out of the arithmetic;
//   - a result that overflows is ±max (FOVF), one that underflows is ±0 (FUNF): no denormals out either;
//   - a finite non-zero value divided by zero is ±max (FDBZ); 0/0 is invalid.
// The rounding mode is SPEFSCR[FRMC]. Each operation sets SPEFSCR's status bits for itself (FINV, FDBZ, FOVF, FUNF,
// FX) and ORs them into the sticky ones. The floating-point exception interrupts the enable bits ask for are not
// taken (no firmware here enables them).
#pragma once

#include <cstdint>

namespace sim::e200::spe {

// rA op rB, each a float32 bit pattern; *spefscr is updated.
uint32_t add(uint32_t a, uint32_t b, uint32_t* spefscr);
uint32_t sub(uint32_t a, uint32_t b, uint32_t* spefscr);
uint32_t mul(uint32_t a, uint32_t b, uint32_t* spefscr);
uint32_t div(uint32_t a, uint32_t b, uint32_t* spefscr);

// The compares: true when the relation holds. cmp* set FINV for an invalid operand; tst* never touch SPEFSCR.
bool cmp_gt(uint32_t a, uint32_t b, uint32_t* spefscr);
bool cmp_lt(uint32_t a, uint32_t b, uint32_t* spefscr);
bool cmp_eq(uint32_t a, uint32_t b, uint32_t* spefscr);
bool tst_gt(uint32_t a, uint32_t b);
bool tst_lt(uint32_t a, uint32_t b);
bool tst_eq(uint32_t a, uint32_t b);

// Conversions. `round_toward_zero` selects the *z forms; the others round by FRMC.
uint32_t from_signed(int32_t v, uint32_t* spefscr);
uint32_t from_unsigned(uint32_t v, uint32_t* spefscr);
uint32_t from_signed_fraction(int32_t v, uint32_t* spefscr);   // v / 2^31
uint32_t from_unsigned_fraction(uint32_t v, uint32_t* spefscr);  // v / 2^32
uint32_t to_signed(uint32_t f, bool round_toward_zero, uint32_t* spefscr);
uint32_t to_unsigned(uint32_t f, bool round_toward_zero, uint32_t* spefscr);
uint32_t to_signed_fraction(uint32_t f, uint32_t* spefscr);
uint32_t to_unsigned_fraction(uint32_t f, uint32_t* spefscr);

}  // namespace sim::e200::spe
