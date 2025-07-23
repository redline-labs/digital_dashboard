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
        SPDLOG_ERROR("Failed to set I2C speed.");
        return 1;
    }

    std::vector<uint8_t> devices = mcp.scan_i2c_bus();

    if (devices.empty()) {
        SPDLOG_INFO("No I2C devices found.");
    } else {
        SPDLOG_INFO("Found {} devices:", devices.size());
        for (const auto& device : devices) {
            SPDLOG_INFO(" - 0x{:02x}", device);
        }
    }

    return 0;
} 