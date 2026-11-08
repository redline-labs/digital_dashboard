#include "pdm_flash_dump/dump.h"

#include <algorithm>
#include <fstream>
#include <set>

#include <spdlog/fmt/fmt.h>

#include "pdm32/config/crc16.h"
#include "pdm32/config/layout.h"

namespace pdm_flash_dump {

namespace {

bool has_all(const Image& image, uint32_t from, uint32_t to) {
    for (uint32_t a = from; a < to; ++a) {
        if (!image.contains(a)) {
            return false;
        }
    }
    return true;
}

std::vector<uint8_t> bytes_of(const Image& image, uint32_t from, uint32_t to) {
    std::vector<uint8_t> out;
    for (uint32_t a = from; a < to; ++a) {
        out.push_back(image.at(a));
    }
    return out;
}

std::string record(char type, std::span<const uint8_t> body) {
    uint32_t sum = 0;
    std::string line = fmt::format("S{}", type);
    for (const uint8_t b : body) {
        line += fmt::format("{:02X}", b);
        sum += b;
    }
    return line + fmt::format("{:02X}\n", static_cast<uint8_t>(~sum));
}

void write_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        throw std::runtime_error("cannot write " + path.string());
    }
}

}  // namespace

Image read_plan(Session& session, const std::vector<Range>& plan, const std::function<void(const Range&)>& progress) {
    Image image;
    for (const Range& range : plan) {
        if (progress) {
            progress(range);
        }
        for (uint32_t address = range.from; address < range.to; address += kReadChunk) {
            uint32_t at = address;
            uint32_t length = std::min(kReadChunk, range.to - address);
            if (length < 3) {
                // The unit returns no data for 1- and 2-byte reads: take a 3-byte window ending at the same place.
                at = range.to - 3;
                length = 3;
            }
            // Within a 16-bit address space and at most kReadChunk long, so both fit.
            const auto bytes = session.read(static_cast<uint16_t>(at), static_cast<uint16_t>(length));
            for (uint32_t k = 0; k < bytes.size(); ++k) {
                image[at + k] = bytes[k];
            }
        }
    }
    return image;
}

std::string to_srec(const Image& image, const std::string& header) {
    std::string out;
    {
        std::vector<uint8_t> body{0, 0, 0};
        body.insert(body.end(), header.begin(), header.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(header.size(), 60)));
        body[0] = static_cast<uint8_t>(body.size());  // count: address, header and checksum, at most 63
        out += record('0', body);
    }
    auto it = image.begin();
    while (it != image.end()) {
        const uint32_t start = it->first;
        std::vector<uint8_t> chunk;
        while (it != image.end() && chunk.size() < 32 && it->first == start + chunk.size()) {
            chunk.push_back(it->second);
            ++it;
        }
        std::vector<uint8_t> body{static_cast<uint8_t>(chunk.size() + 3), static_cast<uint8_t>(start >> 8),
                                  static_cast<uint8_t>(start)};
        body.insert(body.end(), chunk.begin(), chunk.end());
        out += record('1', body);
    }
    out += record('9', std::vector<uint8_t>{3, 0, 0});
    return out;
}

BlockCheck check_block(const Image& image, uint32_t base, uint8_t format) {
    BlockCheck check;
    if (!has_all(image, base, base + 4)) {
        return check;
    }
    check.present = true;
    check.length = (uint32_t{image.at(base + 2)} << 8) | image.at(base + 3);
    check.header_ok = image.at(base) == 0 && image.at(base + 1) == format && check.length >= 6 &&
                      check.length <= 0x8000 - base && has_all(image, base, base + check.length);
    if (!check.header_ok) {
        return check;
    }
    check.bytes = bytes_of(image, base, base + check.length);
    const auto body = std::span<const uint8_t>(check.bytes).first(check.length - 2);
    const uint16_t stored = static_cast<uint16_t>((check.bytes[check.length - 2] << 8) | check.bytes[check.length - 1]);
    check.crc_ok = pdm::config::crc16_ccitt(body) == stored;
    return check;
}

nlohmann::json write_dump(const Session& session, const Image& image, const std::vector<Range>& plan,
                          double seconds, const std::filesystem::path& out) {
    namespace layout = pdm::layout;
    std::filesystem::create_directories(out);
    const std::string serial = session.serial() ? std::to_string(*session.serial()) : "unknown";
    {
        std::ofstream srec(out / "image.s19");
        srec << to_srec(image, "pdm32 serial " + serial);
    }

    nlohmann::json manifest;
    manifest["serial"] = session.serial() ? nlohmann::json(*session.serial()) : nlohmann::json(nullptr);
    manifest["seconds"] = seconds;
    manifest["ranges"] = nlohmann::json::array();
    for (const Range& range : plan) {
        manifest["ranges"].push_back(
            {{"from", fmt::format("${:04X}", range.from)}, {"to", fmt::format("${:04X}", range.to - 1)},
             {"name", std::string(range.name)}});
    }
    std::set<uint32_t> types;
    for (const auto& frame : session.sent()) {
        types.insert(frame.id & 0x0F);
    }
    manifest["requests_sent"] = types;
    manifest["reads_retried"] = session.retries();

    const auto blocks = {std::make_tuple("config", uint32_t{layout::kConfig}, layout::kConfigFormat),
                         std::make_tuple("current_cal", uint32_t{layout::kCurrentCal}, uint8_t{1})};
    for (const auto& [name, base, format] : blocks) {
        const BlockCheck check = check_block(image, base, format);
        nlohmann::json entry{{"present", check.present}};
        if (check.present) {
            entry["header_ok"] = check.header_ok;
            entry["length"] = check.length;
        }
        if (check.header_ok) {
            entry["crc_ok"] = check.crc_ok;
            write_file(out / (std::string(name) + ".bin"), check.bytes);
        }
        manifest[name] = entry;
    }
    if (has_all(image, layout::kFactory, 0x0500)) {
        write_file(out / "factory.bin", bytes_of(image, layout::kFactory, 0x0500));
        manifest["factory"] = {{"present", true}, {"model_byte", image.at(layout::kFactoryModel)}};
    }
    if (has_all(image, 0xCF6B, 0xFE00)) {
        write_file(out / "bootloader.bin", bytes_of(image, 0xCF6B, 0xFE00));
    }
    std::ofstream(out / "manifest.json") << manifest.dump(2) << "\n";
    return manifest;
}

}  // namespace pdm_flash_dump
