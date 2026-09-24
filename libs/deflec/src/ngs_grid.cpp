#include "deflec/ngs_grid.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>
#include <algorithm>
#include <cerrno>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace deflec {
namespace {

static_assert(std::endian::native == std::endian::little,
              "NGS grids are little-endian and are read in place; a big-endian host needs a byte swap here");

constexpr std::size_t kHeader = 44;

template <typename T>
T read(const unsigned char* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

LoadFailure fail(LoadError e, const std::filesystem::path& path, std::string_view why) {
    return {e, path.string() + ": " + std::string(why)};
}

// Catmull-Rom (Keys, a = -1/2) weights for nodes -1, 0, 1, 2 at offset t in [0, 1].
std::array<double, 4> weights(double t) {
    const double t2 = t * t, t3 = t2 * t;
    return {0.5 * (-t3 + 2.0 * t2 - t), 0.5 * (3.0 * t3 - 5.0 * t2 + 2.0), 0.5 * (-3.0 * t3 + 4.0 * t2 + t),
            0.5 * (t3 - t2)};
}

}  // namespace

std::expected<NgsGrid, LoadFailure> NgsGrid::open(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return std::unexpected(fail(LoadError::not_found, path, "no such file"));
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::unexpected(fail(LoadError::unreadable, path, std::strerror(errno)));
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
        const int err = errno;
        ::close(fd);
        return std::unexpected(fail(LoadError::unreadable, path, std::strerror(err)));
    }
    const auto size = static_cast<std::size_t>(st.st_size);

    // A checkout that never ran `git lfs pull` has a ~130-byte text file here.
    constexpr std::string_view kLfs = "version https://git-lfs";
    if (size >= kLfs.size()) {
        std::array<char, kLfs.size()> head{};
        if (::pread(fd, head.data(), head.size(), 0) == static_cast<ssize_t>(head.size()) &&
            std::string_view(head.data(), head.size()) == kLfs) {
            ::close(fd);
            return std::unexpected(fail(LoadError::lfs_pointer, path,
                                        "a Git LFS pointer, not the model: run `git lfs pull` in the checkout"));
        }
    }
    if (size < kHeader) {
        ::close(fd);
        return std::unexpected(fail(LoadError::too_short, path, "shorter than the 44-byte header"));
    }

    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);  // the mapping keeps the file
    if (map == MAP_FAILED) return std::unexpected(fail(LoadError::unreadable, path, std::strerror(errno)));

    NgsGrid g;
    g.map_ = static_cast<const unsigned char*>(map);
    g.size_ = size;
    g.south_ = read<double>(g.map_);
    g.west_ = read<double>(g.map_ + 8);
    g.dlat_ = read<double>(g.map_ + 16);
    g.dlon_ = read<double>(g.map_ + 24);
    g.rows_ = read<std::int32_t>(g.map_ + 32);
    g.cols_ = read<std::int32_t>(g.map_ + 36);

    const bool finite = std::isfinite(g.south_) && std::isfinite(g.west_) && std::isfinite(g.dlat_) && std::isfinite(g.dlon_);
    if (!finite || !(g.dlat_ > 0.0) || !(g.dlon_ > 0.0) || g.rows_ < 2 || g.cols_ < 2 || g.south_ < -90.0 ||
        g.south_ + g.dlat_ * (g.rows_ - 1) > 90.0 + 1e-9 || g.dlon_ * (g.cols_ - 1) > 360.0 + 1e-9)
        return std::unexpected(fail(LoadError::bad_header, path, "the header does not describe a grid on the globe"));
    const auto expected = kHeader + 4u * static_cast<std::size_t>(g.rows_) * static_cast<std::size_t>(g.cols_);
    if (size != expected)
        return std::unexpected(fail(LoadError::size_mismatch, path,
                                    "holds " + std::to_string(size) + " bytes; its header says " +
                                        std::to_string(expected)));
    return g;
}

NgsGrid::NgsGrid(NgsGrid&& o) noexcept
    : map_(std::exchange(o.map_, nullptr)), size_(std::exchange(o.size_, 0)), south_(o.south_), west_(o.west_),
      dlat_(o.dlat_), dlon_(o.dlon_), rows_(o.rows_), cols_(o.cols_) {}

NgsGrid& NgsGrid::operator=(NgsGrid&& o) noexcept {
    if (this != &o) {
        if (map_) ::munmap(const_cast<unsigned char*>(map_), size_);
        map_ = std::exchange(o.map_, nullptr);
        size_ = std::exchange(o.size_, 0);
        south_ = o.south_;
        west_ = o.west_;
        dlat_ = o.dlat_;
        dlon_ = o.dlon_;
        rows_ = o.rows_;
        cols_ = o.cols_;
    }
    return *this;
}

NgsGrid::~NgsGrid() {
    if (map_) ::munmap(const_cast<unsigned char*>(map_), size_);
}

bool NgsGrid::sameGridAs(const NgsGrid& o) const {
    return south_ == o.south_ && west_ == o.west_ && dlat_ == o.dlat_ && dlon_ == o.dlon_ && rows_ == o.rows_ &&
           cols_ == o.cols_;
}

double NgsGrid::value(std::int64_t i, std::int64_t j) const {
    const auto at = kHeader + 4u * (static_cast<std::size_t>(i) * static_cast<std::size_t>(cols_) + static_cast<std::size_t>(j));
    return static_cast<double>(read<float>(map_ + at));
}

// A node of the window; off-grid ones by linear extrapolation from the two
// nearest along each axis that leaves the grid -- NGS's padding.
double NgsGrid::padded(std::int64_t i, std::int64_t j) const {
    const std::int64_t last_i = rows_ - 1, last_j = cols_ - 1;
    const auto along_j = [&](std::int64_t ii) {
        if (j < 0) return 2.0 * value(ii, 0) - value(ii, 1);
        if (j > last_j) return 2.0 * value(ii, last_j) - value(ii, last_j - 1);
        return value(ii, j);
    };
    if (i < 0) return 2.0 * along_j(0) - along_j(1);
    if (i > last_i) return 2.0 * along_j(last_i) - along_j(last_i - 1);
    return along_j(i);
}

std::optional<double> NgsGrid::at(double lat_deg, double lon_deg) const {
    if (!map_) return std::nullopt;
    double y = (lat_deg - south_) / dlat_;
    const double east = std::fmod(std::fmod(lon_deg - west_, 360.0) + 360.0, 360.0);
    double x = east / dlon_;
    const double last_row = rows_ - 1, last_col = cols_ - 1;
    // A billionth of a node of slack, so a point ON an edge -- whose degrees
    // are not exact in binary -- is inside; NaN fails every comparison.
    constexpr double kSlack = 1e-9;
    if (!(y >= -kSlack && y <= last_row + kSlack && x >= -kSlack && x <= last_col + kSlack)) return std::nullopt;
    y = std::clamp(y, 0.0, last_row);
    x = std::clamp(x, 0.0, last_col);
    // The window's corner node, one short of the last so its second node
    // exists: y = last_row interpolates at t = 1.
    const auto i = std::min<std::int64_t>(static_cast<std::int64_t>(y), rows_ - 2);
    const auto j = std::min<std::int64_t>(static_cast<std::int64_t>(x), cols_ - 2);
    const auto wy = weights(y - static_cast<double>(i));
    const auto wx = weights(x - static_cast<double>(j));
    double v = 0.0;
    for (std::size_t a = 0; a < 4; ++a) {
        double row = 0.0;
        for (std::size_t b = 0; b < 4; ++b)
            row += wx[b] * padded(i - 1 + static_cast<std::int64_t>(a), j - 1 + static_cast<std::int64_t>(b));
        v += wy[a] * row;
    }
    return v;
}

}  // namespace deflec
