#include "hc08/image.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace sim {

namespace {

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

}  // namespace

uint32_t Image::lowest() const {
    uint32_t low = ~0u;
    for (const ImageSegment& s : segments) {
        low = std::min(low, s.address);
    }
    return low;
}

uint32_t Image::highest() const {
    uint32_t high = 0;
    for (const ImageSegment& s : segments) {
        high = std::max(high, uint32_t(s.address + s.bytes.size() - 1));
    }
    return high;
}

Image parse_srec(const std::string& text, const std::string& origin) {
    Image image;
    std::istringstream in(text);
    std::string line;
    int number = 0;

    while (std::getline(in, line)) {
        number++;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const auto fail = [&](const std::string& why) {
            return std::runtime_error(origin + ":" + std::to_string(number) + ": " + why);
        };

        if (line.size() < 4 || line[0] != 'S' || line.size() % 2 != 0) {
            throw fail("not an S-record");
        }

        std::vector<uint8_t> bytes;
        for (size_t k = 2; k < line.size(); k += 2) {
            const int hi = hex_digit(line[k]);
            const int lo = hex_digit(line[k + 1]);
            if (hi < 0 || lo < 0) {
                throw fail("bad hex digit");
            }
            bytes.push_back(uint8_t(hi * 16 + lo));
        }

        if (bytes.empty() || bytes[0] != bytes.size() - 1) {
            throw fail("byte count does not match the record length");
        }

        uint8_t sum = 0;
        for (size_t k = 0; k + 1 < bytes.size(); k++) {
            sum = uint8_t(sum + bytes[k]);
        }
        if (uint8_t(~sum) != bytes.back()) {
            throw fail("checksum mismatch");
        }

        const char type = line[1];
        const size_t address_bytes = (type == '0' || type == '1' || type == '5' || type == '9') ? 2
                                     : (type == '2' || type == '6' || type == '8')              ? 3
                                     : (type == '3' || type == '7')                             ? 4
                                                                                                : 0;
        if (address_bytes == 0) {
            throw fail(std::string("unknown record type S") + type);
        }
        // The count byte, the address and the checksum, at least: a record shorter than its own address would read
        // past its end, and an empty data range would start after it finishes.
        if (bytes.size() < 1 + address_bytes + 1) {
            throw fail("record too short for its address");
        }

        uint32_t address = 0;
        for (size_t k = 0; k < address_bytes; k++) {
            address = (address << 8) | bytes[1 + k];
        }
        const auto data_begin = bytes.begin() + static_cast<std::ptrdiff_t>(1 + address_bytes);
        const auto data_end = bytes.end() - 1;

        switch (type) {
            case '0':
                image.name.assign(data_begin, data_end);
                break;
            case '1':
            case '2':
            case '3': {
                // merge with the previous segment when contiguous
                if (!image.segments.empty()) {
                    ImageSegment& last = image.segments.back();
                    if (last.address + last.bytes.size() == address) {
                        last.bytes.insert(last.bytes.end(), data_begin, data_end);
                        break;
                    }
                }
                image.segments.push_back(ImageSegment{address, std::vector<uint8_t>(data_begin, data_end)});
                break;
            }
            case '7':
            case '8':
            case '9':
                image.entry = address;
                break;
            default:
                break;  // S5/S6 counts
        }
    }

    return image;
}

Image load_srec_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("cannot open " + path);
    }
    std::ostringstream text;
    text << file.rdbuf();
    return parse_srec(text.str(), path);
}

void load_image(MemoryMap& mem, const Image& image) {
    for (const ImageSegment& s : image.segments) {
        for (size_t k = 0; k < s.bytes.size(); k++) {
            const uint32_t addr = s.address + uint32_t(k);
            if (addr > 0xFFFF) {
                throw std::runtime_error("image byte beyond $FFFF at $" + std::to_string(addr));
            }
            if (mem.region(uint16_t(addr)) == Region::Device) {
                throw std::runtime_error("image writes a device register at $" + std::to_string(addr));
            }
            mem.bytes[addr] = s.bytes[k];
        }
    }
}

}  // namespace sim
