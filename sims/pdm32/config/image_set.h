// The flash contents the PDM's application expects beyond its own image: the blocks that live in the bootloader's,
// the factory's and PDM Manager's part of flash. A set is either synthetic (config_builder) or read from files dumped
// from a real unit, block by block, so a dump replaces one piece at a time.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "pdm32/config/config_builder.h"

namespace pdm::config {

struct FlashBlock {
    std::string name;  // "config", "current_cal", "factory"
    uint16_t address = 0;
    std::vector<uint8_t> bytes;
};

struct ImageSet {
    std::vector<FlashBlock> blocks;

    const FlashBlock* find(const std::string& name) const;
};

// config + current cal + factory, all synthetic
ImageSet synthetic_images(const ConfigSpec& spec);

// The scenario's "config" member: a builder spec ({"builder": ...} / {"rules": [...]}) for the synthetic blocks, and
// optionally "files": {"config": "dump.bin", "current_cal": "...", "factory": "..."} replacing blocks with dumps
// (raw bytes, loaded at the block's address). Paths are relative to `base_dir`.
ImageSet images_from_json(const nlohmann::json& j, const std::string& base_dir);

}  // namespace pdm::config
