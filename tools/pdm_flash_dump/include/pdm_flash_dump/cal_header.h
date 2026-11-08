// A current-calibration block read off a unit (current_cal.bin), as the C++ header sims/pdm32 compiles in
// (sims/pdm32/config/current_cal_dumped.h). Throws when the block is not a format-1 calibration with a good CRC.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace pdm_flash_dump {

// `name` becomes the constant's suffix: "dumped" -> kCurrentCalDumped, kCurrentCalDumpedCrc.
std::string current_cal_header(std::span<const uint8_t> block, std::string_view source_name, std::string_view name);

}  // namespace pdm_flash_dump
