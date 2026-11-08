// A whole dump: the plan's ranges read through a Session, written as an S-record image, the unit's blocks pulled out
// and CRC-checked, and a manifest of what was done.
//
//   image.s19        every byte read (gaps not read are absent)
//   manifest.json    the unit's serial, the ranges read, the request types sent, the blocks' checks
//   config.bin       the configuration block (length from its header), as sims/pdm32 scenarios load it
//   current_cal.bin  the current calibration
//   factory.bin      the factory block
//   bootloader.bin   the bootloader
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pdm_flash_dump/session.h"

namespace pdm_flash_dump {

using Image = std::map<uint32_t, uint8_t>;

// Bytes per read request (the unit takes up to 65535).
inline constexpr uint32_t kReadChunk = 0x0800;

Image read_plan(Session& session, const std::vector<Range>& plan, const std::function<void(const Range&)>& progress);

// S1 records of up to 32 bytes over the contiguous runs, an S0 header and an S9.
std::string to_srec(const Image& image, const std::string& header);

// A [0]=0 [1]=format [2..3]=length ... CRC16 block (the configuration is format 9, the current calibration format 1):
// present, header_ok, length, crc_ok, and the bytes when the header is good.
struct BlockCheck {
    bool present = false;
    bool header_ok = false;
    uint32_t length = 0;
    bool crc_ok = false;
    std::vector<uint8_t> bytes;
};
BlockCheck check_block(const Image& image, uint32_t base, uint8_t format);

// Writes everything above into `out` and returns the manifest.
nlohmann::json write_dump(const Session& session, const Image& image, const std::vector<Range>& plan,
                          double seconds, const std::filesystem::path& out);

}  // namespace pdm_flash_dump
