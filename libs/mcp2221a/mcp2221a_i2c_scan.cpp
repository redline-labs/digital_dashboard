#include <iostream>
#include <vector>
#include <spdlog/spdlog.h>
#include "mcp2221a/mcp2221a.h"

int main() {
    spdlog::set_level(spdlog::level::debug);

    MCP2221A mcp;
    if (!mcp.open()) {
        return 1;
    }

    if (!mcp.set_i2c_speed(100000)) {
        spdlog::error("Failed to set I2C speed.");
        return 1;
    }

    std::vector<uint8_t> devices = mcp.scan_i2c_bus();

    if (devices.empty()) {
        spdlog::info("No I2C devices found.");
    } else {
        spdlog::info("Found {} devices:", devices.size());
        for (const auto& device : devices) {
            spdlog::info(" - 0x{:02x}", device);
        }
    }

    return 0;
} 