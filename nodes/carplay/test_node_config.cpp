// SPDX-License-Identifier: GPL-3.0-or-later
//
// The node's config file, and the PNG header reader behind the manufacturer
// button's artwork.
//
// Two things worth holding down here. The PNG reader is hand-rolled -- it takes
// the icon's dimensions straight out of the IHDR chunk rather than decoding --
// so it has to reject anything that is not a PNG instead of reporting whatever
// those bytes happen to say; CarPlay is handed the file verbatim, and an icon
// advertised at the wrong size simply does not draw. And a config that fails
// half way through must leave the caller's defaults untouched, because the node
// refuses to start on a bad config and would otherwise be reasoning about a
// half-applied one on the way out.
#include "node_config.h"

#include <spdlog/spdlog.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

namespace fs = std::filesystem;

fs::path scratchDir()
{
    const fs::path dir = fs::temp_directory_path() / "carplay_node_config_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void writeFile(const fs::path& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void writeBytes(const fs::path& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

// A PNG header only: signature, then an IHDR chunk whose payload starts with
// width and height as big-endian uint32. Enough for the reader under test,
// which never decodes the image.
std::vector<uint8_t> pngHeader(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> out{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    const auto be32 = [&out](uint32_t value) {
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(value & 0xFF));
    };
    be32(13);  // IHDR payload length
    out.insert(out.end(), {'I', 'H', 'D', 'R'});
    be32(width);
    be32(height);
    out.insert(out.end(), {8, 6, 0, 0, 0});  // bit depth, colour type, ...
    return out;
}

}  // namespace

int main()
{
    using carplay::loadNodeConfig;
    using carplay::loadOemIcon;
    using carplay::NodeConfig;

    const fs::path dir = scratchDir();

    // The defaults a node starts from, before any file is read.
    {
        const NodeConfig defaults;
        expect(defaults.oem_button.enabled, "the manufacturer button is on by default");
        expect(!defaults.oem_button.label.empty(), "and carries a default label");
        expect(defaults.oem_button.icons.empty(), "but no artwork -- that only comes from a file");
        expect(!defaults.night_mode, "day theme by default");
        expect(defaults.display.primary_input == airplay::PrimaryInput::Touch,
               "touch-primary by default");
        expect(!defaults.vehicle.right_hand_drive, "left-hand drive by default");
        expect(defaults.display.width_px > 0 && defaults.display.height_px > 0,
               "the display has a usable default size");
        expect(defaults.max_stage == 7, "the full pipeline by default");
    }

    // --- The PNG header reader ---------------------------------------------
    {
        const fs::path good = dir / "good.png";
        writeBytes(good, pngHeader(120, 90));

        airplay::OemIcon icon;
        expect(loadOemIcon(good.string(), false, icon), "a PNG header is accepted");
        expect(icon.width_px == 120 && icon.height_px == 90,
               "dimensions come from IHDR, big endian");
        expect(!icon.prerendered, "prerendered follows the argument");
        expect(icon.png.size() == pngHeader(120, 90).size(),
               "the file is carried verbatim, not re-encoded");

        airplay::OemIcon pre;
        expect(loadOemIcon(good.string(), true, pre) && pre.prerendered,
               "prerendered can be set");
    }

    // Dimensions above 32767, which a naive signed read would mangle.
    {
        const fs::path big = dir / "big.png";
        writeBytes(big, pngHeader(70000, 40000));
        airplay::OemIcon icon;
        expect(loadOemIcon(big.string(), false, icon), "a large PNG header is accepted");
        expect(icon.width_px == 70000 && icon.height_px == 40000,
               "dimensions past 16 bits survive");
    }

    // Everything that is not a usable PNG must be refused rather than guessed
    // at: CarPlay gets these bytes as-is.
    {
        airplay::OemIcon icon;
        expect(!loadOemIcon((dir / "missing.png").string(), false, icon), "a missing file fails");

        const fs::path jpeg = dir / "actually.jpg";
        writeBytes(jpeg, {0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J', 'F', 'I', 'F', 0,
                          0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
        expect(!loadOemIcon(jpeg.string(), false, icon), "a JPEG is refused");

        const fs::path truncated = dir / "truncated.png";
        std::vector<uint8_t> head = pngHeader(60, 60);
        head.resize(12);
        writeBytes(truncated, head);
        expect(!loadOemIcon(truncated.string(), false, icon), "a truncated header is refused");

        const fs::path empty = dir / "empty.png";
        writeBytes(empty, {});
        expect(!loadOemIcon(empty.string(), false, icon), "an empty file is refused");

        // Right signature, wrong first chunk.
        const fs::path no_ihdr = dir / "no_ihdr.png";
        std::vector<uint8_t> bad = pngHeader(60, 60);
        bad[12] = 'X';
        writeBytes(no_ihdr, bad);
        expect(!loadOemIcon(no_ihdr.string(), false, icon), "a first chunk that is not IHDR fails");

        // A zero dimension would be advertised to CarPlay as a zero-sized icon.
        const fs::path zero = dir / "zero.png";
        writeBytes(zero, pngHeader(0, 60));
        expect(!loadOemIcon(zero.string(), false, icon), "a zero dimension is refused");
    }

    // --- The YAML surface ---------------------------------------------------

    // The config that ships with the repo must load, artwork and all. This is
    // the case a synthetic fixture cannot cover.
    {
        NodeConfig config;
        const std::string shipped = std::string(CARPLAY_CONFIG_DIR) + "/carplay.yaml";
        expect(loadNodeConfig(shipped, config), "the committed config loads");
        expect(config.oem_button.icons.size() == 3, "with its three icon renditions");
        bool sized = true;
        for (const airplay::OemIcon& icon : config.oem_button.icons)
        {
            sized = sized && icon.width_px > 0 && icon.width_px == icon.height_px;
        }
        expect(sized, "each committed icon is square and non-empty");
    }

    // Absent keys keep the caller's values rather than resetting them.
    {
        const fs::path partial = dir / "partial.yaml";
        writeFile(partial, "night_mode: true\n");

        NodeConfig config;
        config.oem_button.label = "Preexisting";
        expect(loadNodeConfig(partial.string(), config), "a partial config loads");
        expect(config.night_mode, "the key it sets is applied");
        expect(config.oem_button.label == "Preexisting", "and absent keys are left alone");
        expect(config.oem_button.enabled, "including the defaults");
    }

    // Icon paths resolve against the config file, not the working directory --
    // otherwise a config is only usable from one place.
    {
        const fs::path sub = dir / "vehicle";
        fs::create_directories(sub);
        writeBytes(sub / "icon.png", pngHeader(64, 64));
        writeFile(sub / "config.yaml",
                  "oem_button:\n"
                  "  enabled: true\n"
                  "  label: \"Rover\"\n"
                  "  icons:\n"
                  "    - path: icon.png\n"
                  "      prerendered: true\n");

        NodeConfig config;
        expect(loadNodeConfig((sub / "config.yaml").string(), config),
               "a config with a relative icon path loads");
        expect(config.oem_button.label == "Rover", "label is read");
        expect(config.oem_button.icons.size() == 1, "one icon");
        if (config.oem_button.icons.size() == 1)
        {
            expect(config.oem_button.icons[0].width_px == 64, "resolved against the config's dir");
            expect(config.oem_button.icons[0].prerendered, "per-icon prerendered is honoured");
        }
    }

    // An icons list replaces rather than appends: a config describes the whole
    // set the phone chooses from.
    {
        const fs::path one = dir / "one.yaml";
        writeBytes(dir / "a.png", pngHeader(32, 32));
        writeFile(one, "oem_button:\n  icons:\n    - path: a.png\n");

        NodeConfig config;
        airplay::OemIcon stale;
        stale.width_px = 999;
        stale.height_px = 999;
        config.oem_button.icons.push_back(stale);

        expect(loadNodeConfig(one.string(), config), "the config loads");
        expect(config.oem_button.icons.size() == 1, "the previous list is replaced, not extended");
        expect(config.oem_button.icons[0].width_px == 32, "with the file's icon");
    }

    // The vehicle identity, which reaches the phone by two routes and is
    // recorded against the pairing.
    {
        const fs::path vehicle = dir / "vehicle.yaml";
        writeFile(vehicle,
                  "vehicle:\n"
                  "  name: \"Test Car\"\n"
                  "  model: \"TestModel1,1\"\n"
                  "  manufacturer: \"Testworks\"\n"
                  "  serial_number: \"SN12345\"\n"
                  "  firmware_version: \"2.1.0\"\n"
                  "  hardware_version: \"3.0\"\n"
                  "  right_hand_drive: true\n"
                  "  engine_type: electric\n"
                  "  language: \"de\"\n"
                  "  supported_languages: [\"de\", \"en\", \"fr\"]\n");

        NodeConfig config;
        expect(loadNodeConfig(vehicle.string(), config), "a vehicle identity loads");
        expect(config.vehicle.name == "Test Car", "name");
        expect(config.vehicle.model == "TestModel1,1", "model");
        expect(config.vehicle.manufacturer == "Testworks", "manufacturer");
        expect(config.vehicle.serial_number == "SN12345", "serial number");
        expect(config.vehicle.firmware_version == "2.1.0", "firmware version");
        expect(config.vehicle.hardware_version == "3.0", "hardware version");
        expect(config.vehicle.right_hand_drive, "right-hand drive");
        expect(config.vehicle.engine_type == iap2::EngineType::kElectric, "engine type");
        expect(config.vehicle.language == "de", "language");
        expect(config.vehicle.supported_languages.size() == 3, "supported languages replace");
    }

    // Enumerated keys are closed sets. A typo has to stop the node rather than
    // silently take a default -- the difference between a diesel and an
    // electric is not something to guess at.
    {
        const fs::path knob = dir / "knob.yaml";
        writeFile(knob, "display:\n  primary_input: knob\n");
        NodeConfig config;
        expect(loadNodeConfig(knob.string(), config), "primary_input: knob loads");
        expect(config.display.primary_input == airplay::PrimaryInput::Knob, "and selects the knob");

        const fs::path wrong = dir / "wrong.yaml";
        writeFile(wrong, "display:\n  primary_input: wheel\n");
        NodeConfig other;
        expect(!loadNodeConfig(wrong.string(), other), "an unknown primary_input is rejected");

        const fs::path engine = dir / "engine.yaml";
        writeFile(engine, "vehicle:\n  engine_type: steam\n");
        NodeConfig third;
        expect(!loadNodeConfig(engine.string(), third), "an unknown engine_type is rejected");
    }

    // Display geometry. A zero would be advertised as a panel the phone cannot
    // draw on: the session comes up and produces nothing.
    {
        const fs::path good = dir / "display.yaml";
        writeFile(good,
                  "display:\n"
                  "  width_px: 1920\n"
                  "  height_px: 720\n"
                  "  fps: 60\n"
                  "  physical_width_mm: 260\n");
        NodeConfig config;
        expect(loadNodeConfig(good.string(), config), "display geometry loads");
        expect(config.display.width_px == 1920 && config.display.height_px == 720, "size");
        expect(config.display.fps == 60, "frame rate");
        expect(config.display.physical_width_mm == 260, "physical width");

        for (const char* key : {"width_px", "height_px", "fps", "physical_width_mm"})
        {
            const fs::path zero = dir / "zero.yaml";
            writeFile(zero, std::string("display:\n  ") + key + ": 0\n");
            NodeConfig rejected;
            expect(!loadNodeConfig(zero.string(), rejected),
                   std::string("a zero ") + key + " is rejected");
        }
    }

    // An empty language list would leave the phone nothing to pick.
    {
        const fs::path empty = dir / "no_languages.yaml";
        writeFile(empty, "vehicle:\n  supported_languages: []\n");
        NodeConfig config;
        expect(!loadNodeConfig(empty.string(), config), "an empty supported_languages is rejected");
    }

    // Failures must not half-apply. The node refuses to start on a bad config,
    // so what it holds on the way out should still be the defaults.
    {
        const fs::path bad = dir / "bad_icon.yaml";
        writeFile(bad,
                  "night_mode: true\n"
                  "oem_button:\n"
                  "  label: \"Should not stick\"\n"
                  "  icons:\n"
                  "    - path: nonexistent.png\n");

        NodeConfig config;
        const bool ok = loadNodeConfig(bad.string(), config);
        expect(!ok, "a config naming a missing icon fails");
        expect(!config.night_mode, "and nothing it set is applied");
        expect(config.oem_button.label != "Should not stick", "including keys read before the failure");
    }

    // A file that is not YAML at all, and one that is not there.
    {
        const fs::path garbage = dir / "garbage.yaml";
        writeFile(garbage, "oem_button:\n  - this is: [not, a, map\n");
        NodeConfig config;
        expect(!loadNodeConfig(garbage.string(), config), "malformed YAML is rejected");
        expect(!loadNodeConfig((dir / "absent.yaml").string(), config), "a missing file is rejected");
    }

    fs::remove_all(dir);

    if (failures == 0)
    {
        SPDLOG_INFO("node config tests passed");
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
