#include "pdm32/config/crc16.h"

namespace pdm::config {

uint16_t crc16_ccitt(std::span<const uint8_t> bytes, uint16_t crc) {
    for (uint8_t byte : bytes) {
        crc = uint16_t(crc ^ (uint16_t(byte) << 8));
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000) ? uint16_t((crc << 1) ^ 0x1021) : uint16_t(crc << 1);
        }
    }
    return crc;
}

}  // namespace pdm::config
