// The PDM's CRC: CRC-16/CCITT-FALSE (polynomial 0x1021, initial 0xFFFF, not reflected, no final xor). The
// configuration and current-calibration blocks carry it big-endian in their last two bytes, over every byte before
// them.
#pragma once

#include <cstdint>
#include <span>

namespace pdm::config {

uint16_t crc16_ccitt(std::span<const uint8_t> bytes, uint16_t crc = 0xFFFF);

}  // namespace pdm::config
