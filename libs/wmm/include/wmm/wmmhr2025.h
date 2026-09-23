// World Magnetic Model High Resolution 2025 (WMMHR2025).
//
// Link against the wmm_hr2025 target. The coefficient file is embedded and parsed at
// compile time inside that library's single translation unit
// (src/wmmhr2025.cpp), so including this header is cheap and needs only C++20.
#pragma once

#include "wmm/model.h"
#include "wmm/types.h"

namespace wmm::wmmhr2025 {

using Model = wmm::Model<133>;

// The parsed model: coefficients via model()(n, m), plus name(), epoch,
// release_date, valid_from(), valid_until().
const Model& model() noexcept;

// Field elements at a geodetic position (height above the WGS-84 ellipsoid) and
// decimal year. Grid variation is NaN for |latitude| < 55 degrees. The validity
// window is not enforced.
MagneticElements magnetic_field(const GeodeticCoord& position, double decimal_year) noexcept;

inline MagneticElements magnetic_field(const GeodeticCoord& position, const Date& date) noexcept {
    return magnetic_field(position, decimal_year(date));
}

// One-sigma model uncertainty for the given field (only X, Y, Z, H, F, I, D are set).
MagneticElements uncertainty(const MagneticElements& at) noexcept;

}  // namespace wmm::wmmhr2025
