// Firmware images: Motorola S-records (the PDM's pdm.hex is S19 despite the name) and raw binary blocks.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hc08/memmap.h"

namespace sim {

struct ImageSegment {
    uint32_t address = 0;
    std::vector<uint8_t> bytes;
};

struct Image {
    std::string name;  // the S0 header's text
    std::vector<ImageSegment> segments;
    uint32_t entry = 0;  // the S7/S8/S9 record's address

    // [lowest, highest] address covered, for reports
    uint32_t lowest() const;
    uint32_t highest() const;
};

// Parses S-record text. Throws std::runtime_error naming the line on a malformed record or a bad checksum.
Image parse_srec(const std::string& text, const std::string& origin = "<text>");
Image load_srec_file(const std::string& path);

// Copies every segment into storage (flash or ROM bytes; device windows are refused).
void load_image(MemoryMap& mem, const Image& image);

}  // namespace sim
