// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <cstdio>
#include <filesystem>
#include <utility>
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

// A scalar key, applied only when present so an absent key keeps the default.
template <typename T>
void assignIfPresent(const YAML::Node& parent, const char* key, T& out)
{
    if (const YAML::Node node = parent[key])
    {
        out = node.as<T>();
    }
}

// A key whose value is one of a fixed set of names. Anything else is a typo
// worth stopping for rather than silently taking a default -- the difference
// between a diesel and an electric is not something to guess at.
template <typename T>
bool assignEnumIfPresent(const YAML::Node& parent, const char* key,
                         const std::vector<std::pair<const char*, T>>& names, T& out)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return true;
    }
    const std::string value = node.as<std::string>();
    for (const auto& [name, enumerator] : names)
    {
        if (value == name)
        {
            out = enumerator;
            return true;
        }
    }

    std::string allowed;
    for (const auto& [name, enumerator] : names)
    {
        allowed += allowed.empty() ? "" : ", ";
        allowed += name;
    }
    SPDLOG_ERROR("[node] {} must be one of [{}], not '{}'", key, allowed, value);
    return false;
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
        assignIfPresent(root, "night_mode", parsed.night_mode);
        assignIfPresent(root, "device_id", parsed.device_id);

        if (const YAML::Node vehicle = root["vehicle"]; vehicle && vehicle.IsMap())
        {
            VehicleIdentity& out_vehicle = parsed.vehicle;
            assignIfPresent(vehicle, "name", out_vehicle.name);
            assignIfPresent(vehicle, "model", out_vehicle.model);
            assignIfPresent(vehicle, "manufacturer", out_vehicle.manufacturer);
            assignIfPresent(vehicle, "serial_number", out_vehicle.serial_number);
            assignIfPresent(vehicle, "firmware_version", out_vehicle.firmware_version);
            assignIfPresent(vehicle, "hardware_version", out_vehicle.hardware_version);
            assignIfPresent(vehicle, "right_hand_drive", out_vehicle.right_hand_drive);
            assignIfPresent(vehicle, "language", out_vehicle.language);

            if (!assignEnumIfPresent<iap2::EngineType>(
                    vehicle, "engine_type",
                    {{"gas", iap2::EngineType::kGas},
                     {"diesel", iap2::EngineType::kDiesel},
                     {"electric", iap2::EngineType::kElectric},
                     {"cng", iap2::EngineType::kCng}},
                    out_vehicle.engine_type))
            {
                return false;
            }

            if (const YAML::Node languages = vehicle["supported_languages"];
                languages && languages.IsSequence())
            {
                out_vehicle.supported_languages.clear();
                for (const YAML::Node& language : languages)
                {
                    out_vehicle.supported_languages.push_back(language.as<std::string>());
                }
                if (out_vehicle.supported_languages.empty())
                {
                    SPDLOG_ERROR("[node] supported_languages is empty; the phone needs at least one");
                    return false;
                }
            }
        }

        if (const YAML::Node display = root["display"]; display && display.IsMap())
        {
            DisplayConfig& out_display = parsed.display;
            assignIfPresent(display, "width_px", out_display.width_px);
            assignIfPresent(display, "height_px", out_display.height_px);
            assignIfPresent(display, "fps", out_display.fps);
            assignIfPresent(display, "physical_width_mm", out_display.physical_width_mm);

            if (!assignEnumIfPresent<airplay::PrimaryInput>(
                    display, "primary_input",
                    {{"touch", airplay::PrimaryInput::Touch},
                     {"knob", airplay::PrimaryInput::Knob}},
                    out_display.primary_input))
            {
                return false;
            }

            // Zero would be advertised to the phone as a display it cannot draw
            // on, and the session would come up and produce nothing.
            if (out_display.width_px == 0 || out_display.height_px == 0 ||
                out_display.fps == 0 || out_display.physical_width_mm == 0)
            {
                SPDLOG_ERROR("[node] display width_px, height_px, fps and physical_width_mm "
                             "must all be non-zero");
                return false;
            }
        }
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
                    // Defaults true: false renders an empty tile on hardware.
                    // See airplay::OemIcon::prerendered.
                    const bool prerendered = entry["prerendered"]
                                                 ? entry["prerendered"].as<bool>()
                                                 : true;
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
