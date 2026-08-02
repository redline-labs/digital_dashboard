// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <filesystem>
#include <vector>

namespace carplay
{
namespace
{

// PNG signature, then an IHDR chunk whose first eight payload bytes are the
// width and height as big-endian uint32. That layout is fixed by the spec, so
// reading the dimensions needs no decoder.
constexpr size_t kPngSignatureSize = 8;
constexpr size_t kIhdrTagOffset = 12;
constexpr size_t kIhdrWidthOffset = 16;
constexpr size_t kPngHeaderSize = 24;

uint32_t readBigEndian32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (file == nullptr)
    {
        return false;
    }
    std::vector<uint8_t> bytes;
    uint8_t chunk[4096];
    size_t read = 0;
    while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
    {
        bytes.insert(bytes.end(), chunk, chunk + read);
    }
    std::fclose(file);
    out = std::move(bytes);
    return true;
}

}  // namespace

bool loadOemIcon(const std::string& path, bool prerendered, airplay::OemIcon& out)
{
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes))
    {
        SPDLOG_ERROR("[node] cannot read manufacturer-button icon '{}'", path);
        return false;
    }

    static constexpr uint8_t kSignature[kPngSignatureSize] = {0x89, 'P', 'N', 'G',
                                                              '\r', '\n', 0x1A, '\n'};
    if (bytes.size() < kPngHeaderSize ||
        !std::equal(kSignature, kSignature + kPngSignatureSize, bytes.begin()) ||
        bytes[kIhdrTagOffset] != 'I' || bytes[kIhdrTagOffset + 1] != 'H' ||
        bytes[kIhdrTagOffset + 2] != 'D' || bytes[kIhdrTagOffset + 3] != 'R')
    {
        // CarPlay is handed the bytes verbatim; a JPEG here would be advertised
        // with nonsense dimensions and simply not draw, with nothing to see.
        SPDLOG_ERROR("[node] manufacturer-button icon '{}' is not a PNG", path);
        return false;
    }

    out.png = std::move(bytes);
    out.width_px = readBigEndian32(out.png, kIhdrWidthOffset);
    out.height_px = readBigEndian32(out.png, kIhdrWidthOffset + 4);
    out.prerendered = prerendered;

    if (out.width_px == 0 || out.height_px == 0)
    {
        SPDLOG_ERROR("[node] manufacturer-button icon '{}' declares a zero dimension", path);
        return false;
    }
    SPDLOG_DEBUG("[node] manufacturer-button icon '{}': {}x{}, {} bytes", path, out.width_px,
                 out.height_px, out.png.size());
    return true;
}

bool loadNodeConfig(const std::string& path, NodeConfig& out)
{
    YAML::Node root;
    try
    {
        root = YAML::LoadFile(path);
    }
    catch (const std::exception& error)
    {
        SPDLOG_ERROR("[node] cannot load config '{}': {}", path, error.what());
        return false;
    }

    // Parsed into a copy so a failure part-way through does not leave the caller
    // with half a config applied.
    NodeConfig parsed = out;
    const std::filesystem::path base = std::filesystem::path(path).parent_path();

    try
    {
        if (const YAML::Node button = root["oem_button"]; button && button.IsMap())
        {
            if (const YAML::Node enabled = button["enabled"])
            {
                parsed.oem_button.enabled = enabled.as<bool>();
            }
            if (const YAML::Node label = button["label"])
            {
                parsed.oem_button.label = label.as<std::string>();
            }
            if (const YAML::Node icons = button["icons"]; icons && icons.IsSequence())
            {
                // Replaces rather than appends: a config listing icons is
                // describing the whole set the phone gets to choose from.
                parsed.oem_button.icons.clear();
                for (const YAML::Node& entry : icons)
                {
                    const std::string icon_path = entry["path"].as<std::string>();
                    const bool prerendered = entry["prerendered"]
                                                 ? entry["prerendered"].as<bool>()
                                                 : false;
                    const std::filesystem::path resolved =
                        std::filesystem::path(icon_path).is_absolute()
                            ? std::filesystem::path(icon_path)
                            : base / icon_path;

                    airplay::OemIcon icon;
                    if (!loadOemIcon(resolved.string(), prerendered, icon))
                    {
                        return false;
                    }
                    parsed.oem_button.icons.push_back(std::move(icon));
                }
            }
        }
    }
    catch (const std::exception& error)
    {
        SPDLOG_ERROR("[node] config '{}' is malformed: {}", path, error.what());
        return false;
    }

    out = std::move(parsed);
    return true;
}

}  // namespace carplay
