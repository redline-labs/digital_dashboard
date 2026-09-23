// Test-only: the WMMHR2025 model as a constexpr variable, for static_asserts.
// (The library's own copy is internal to src/wmmhr2025.cpp and so is not usable
// in other translation units' constant expressions.) This is also what any
// consumer wanting compile-time evaluation would write.
#pragma once

#include "wmm/cof.h"
#include "wmm/field.h"

namespace test {

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
inline constexpr char wmmhr2025_cof[] = {
#embed "../src/data/WMMHR.COF"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

inline constexpr auto wmmhr2025 = wmm::load_cof<wmmhr2025_cof>();

}  // namespace test
