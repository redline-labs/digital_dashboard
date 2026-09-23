// The one translation unit that embeds and parses WMMHR.COF. Both the text and
// the parsed model have internal linkage; only the model's coefficient table
// ends up in the binary (the text is used during constant evaluation only).
#include "wmm/wmmhr2025.h"

#include <type_traits>

#include "wmm/cof.h"
#include "wmm/field.h"

namespace wmm::wmmhr2025 {
namespace {

// #embed in C++ is an extension before C++26 (P1967); clang says so, GCC does not.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
#endif
constexpr char cof_text[] = {
#embed "data/WMMHR.COF"
};
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

constexpr auto parsed = load_cof<cof_text>();

// The public header promises Model<133>; catch a replacement file of a different degree here.
static_assert(std::is_same_v<decltype(parsed), const Model>, "WMMHR.COF degree differs from wmmhr2025::Model");
static_assert(parsed.name() == "WMMHR-2025");

}  // namespace

const Model& model() noexcept { return parsed; }

MagneticElements magnetic_field(const GeodeticCoord& position, double decimal_year) noexcept {
    return wmm::magnetic_field(parsed, position, decimal_year);
}

MagneticElements uncertainty(const MagneticElements& at) noexcept { return wmm::wmmhr_uncertainty(at); }

}  // namespace wmm::wmmhr2025
