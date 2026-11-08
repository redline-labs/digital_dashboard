// The PDM application image for the tests that run it, where the simulator itself would look for it
// (default_firmware_path), or empty when it is not there: the image is MoTeC's and not in the tree, and the tests
// that need it skip.
#pragma once

#include <filesystem>
#include <string>

#include "pdm32/top/instance.h"

namespace sim::test {

inline std::string firmware_hex() {
    const std::string path = sim::sys::default_firmware_path();
    return std::filesystem::exists(path) ? path : std::string();
}

}  // namespace sim::test
