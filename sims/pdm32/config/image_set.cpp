#include "pdm32/config/image_set.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

#include "pdm32/config/layout.h"

namespace pdm::config {

const FlashBlock* ImageSet::find(const std::string& name) const {
    for (const FlashBlock& b : blocks) {
        if (b.name == name) {
            return &b;
        }
    }
    return nullptr;
}

ImageSet synthetic_images(const ConfigSpec& spec) {
    ImageSet set;
    set.blocks.push_back({"config", layout::kConfig, build_config(spec)});
    set.blocks.push_back({"current_cal", layout::kCurrentCal, build_current_cal(spec.current_cal)});
    set.blocks.push_back({"factory", layout::kFactory, build_factory()});
    return set;
}

ImageSet images_from_json(const nlohmann::json& j, const std::string& base_dir) {
    ImageSet set = synthetic_images(parse_config_spec(j));
    if (!j.contains("files")) {
        return set;
    }

    for (const auto& [name, path_json] : j.at("files").items()) {
        FlashBlock* block = nullptr;
        for (FlashBlock& b : set.blocks) {
            if (b.name == name) {
                block = &b;
            }
        }
        if (block == nullptr) {
            throw std::runtime_error("config.files: unknown block '" + name + "' (config, current_cal, factory)");
        }

        std::filesystem::path path = path_json.get<std::string>();
        if (path.is_relative()) {
            path = std::filesystem::path(base_dir) / path;
        }
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("config.files." + name + ": cannot open " + path.string());
        }
        block->bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    return set;
}

}  // namespace pdm::config
