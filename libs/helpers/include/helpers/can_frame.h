#ifndef HELPERS_CAN_FRAME_H
#define HELPERS_CAN_FRAME_H

#include <array>
#include <cstdint>

namespace helpers
{

// Generic CAN/CAN-FD frame (up to 64 bytes)
struct CanFrame
{
    uint32_t id { 0 };          // 11-bit or 29-bit identifier (SFF/EFF)
    uint8_t len { 0 };          // number of bytes in payload (0..64)
    bool isExtended { false };  // EFF vs SFF
    bool isFD { false };        // CAN-FD frame
    std::array<uint8_t, 64> data { {0} };
};

} // namespace helpers

#endif // HELPERS_CAN_FRAME_H


