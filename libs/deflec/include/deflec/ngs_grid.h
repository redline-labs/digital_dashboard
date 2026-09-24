#pragma once

// One NGS gridded model file (.bin), exactly as NGS distributes it, mapped
// into memory: only the pages for where the car is are ever read, so a
// 233 MB North America grid costs a few hundred kilobytes of RAM.
//
// The format (NGS, NAPGD2022; little-endian): a 44-byte header -- four
// float64, the south edge, the west edge (east positive, degrees), the
// latitude and longitude spacing (degrees); three int32, rows, columns and a
// kind -- then float32 values, row 0 at the south edge, west to east.
//
// Interpolation is NGS's rule for NAPGD2022 grids: a local 4 x 4 bicubic --
// Catmull-Rom, the one bicubic that reproduces NGS's published test values to
// their last digit -- with linear extrapolation padding the window within a
// node of an edge.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <optional>
#include <string>

namespace deflec {

enum class LoadError {
    not_found,      // no file at the path
    lfs_pointer,    // a Git LFS pointer: the checkout never fetched the model
    unreadable,     // exists, cannot be opened or mapped
    too_short,      // shorter than the 44-byte header
    bad_header,     // spacing, shape or extent that is not a grid on the globe
    size_mismatch,  // the values are not rows x columns float32
    extent_differs, // two components of one model on different grids
};

struct LoadFailure {
    LoadError error;
    std::string message;  // names the file and says what to do about it
};

class NgsGrid {
  public:
    static std::expected<NgsGrid, LoadFailure> open(const std::filesystem::path& path);

    NgsGrid(NgsGrid&&) noexcept;
    NgsGrid& operator=(NgsGrid&&) noexcept;
    NgsGrid(const NgsGrid&) = delete;
    NgsGrid& operator=(const NgsGrid&) = delete;
    ~NgsGrid();

    // The value at geodetic latitude and longitude, degrees (longitude in any
    // turn), or nothing outside the grid.
    std::optional<double> at(double lat_deg, double lon_deg) const;

    double south_deg() const { return south_; }
    double west_deg() const { return west_; }
    double spacing_lat_deg() const { return dlat_; }
    double spacing_lon_deg() const { return dlon_; }
    std::int32_t rows() const { return rows_; }
    std::int32_t cols() const { return cols_; }
    bool sameGridAs(const NgsGrid& other) const;

  private:
    NgsGrid() = default;
    double value(std::int64_t i, std::int64_t j) const;
    double padded(std::int64_t i, std::int64_t j) const;

    const unsigned char* map_ = nullptr;
    std::size_t size_ = 0;
    double south_ = 0.0, west_ = 0.0, dlat_ = 0.0, dlon_ = 0.0;
    std::int32_t rows_ = 0, cols_ = 0;
};

}  // namespace deflec
