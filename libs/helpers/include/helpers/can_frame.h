#ifndef HELPERS_CAN_FRAME_H
#define HELPERS_CAN_FRAME_H

#include <array>
#include <cstdint>
#include <span>

namespace helpers
{

// Generic CAN/CAN-FD frame (up to 64 bytes)
struct CanFrame
{
    uint32_t id;          // 11-bit or 29-bit identifier (SFF/EFF)
    uint8_t len;          // number of bytes in payload (0..64)
    bool isExtended;  // EFF vs SFF
    bool isFD;        // CAN-FD frame
    std::array<uint8_t, 64> data;

    CanFrame() :
      id{0u},
      len{0u},
      isExtended{false},
      isFD{false},
      data{{0}}
    {
    }

    std::span<const uint8_t> data_span() const
    {
        size_t span_size = std::min(static_cast<size_t>(len), data.size());
        return std::span<const uint8_t>(data.begin(), span_size);
    }
};

} // namespace helpers

#endif // HELPERS_CAN_FRAME_H


