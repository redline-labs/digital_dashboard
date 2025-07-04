#include "apple_mfi_ic/apple_mfi_ic.h"
#include "mcp2221a/mcp2221a.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h> // Required for fmt::join
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

AppleMFIIC::AppleMFIIC() 
    : mcp2221a_(std::make_unique<MCP2221A>())
    , connected_(false) {
}

AppleMFIIC::~AppleMFIIC() {
    close();
}

bool AppleMFIIC::init() {
    if (!mcp2221a_->open()) {
        spdlog::error("Failed to open MCP2221A device");
        return false;
    }
    
    // Set I2C speed to 400kHz (standard speed for MFI communication)
    if (!mcp2221a_->set_i2c_speed(100000)) {
        spdlog::error("Failed to set I2C speed");
        mcp2221a_->close();
        return false;
    }
    
    /*
    // Scan for the device
    auto devices = mcp2221a_->scan_i2c_bus();
    bool found = false;
    for (auto addr : devices) {
        if (addr == I2C_ADDRESS) {
            found = true;
            break;
        }
    }
    
    if (!found) {
        spdlog::error("Apple MFI IC not found at address 0x{:02x}", I2C_ADDRESS);
        mcp2221a_->close();
        return false;
    }*/
    
    //spdlog::info("Apple MFI IC found at address 0x{:02x}", I2C_ADDRESS);
    connected_ = true;
    return true;
}

void AppleMFIIC::close() {
    if (mcp2221a_) {
        mcp2221a_->close();
    }
    connected_ = false;
}

bool AppleMFIIC::is_connected() const {
    return connected_ && mcp2221a_ && mcp2221a_->is_open();
}

std::optional<std::vector<uint8_t>> AppleMFIIC::read_register(Register reg, size_t length) {
    if (!is_connected()) {
        spdlog::error("Not connected to Apple MFI IC");
        return std::nullopt;
    }
    
    // Write the register address
    std::vector<uint8_t> reg_addr = {static_cast<uint8_t>(reg)};
    if (!mcp2221a_->i2c_write(I2C_ADDRESS, reg_addr)) {
        spdlog::error("Failed to write register address 0x{:02x}", static_cast<uint8_t>(reg));
        return std::nullopt;
    }
    
    // Read the register value
    auto data = mcp2221a_->i2c_read(I2C_ADDRESS, length);
    if (data.empty()) {
        spdlog::error("Failed to read register 0x{:02x}", static_cast<uint8_t>(reg));
        return std::nullopt;
    }
    
    SPDLOG_INFO("Read register 0x{:02X} = [{:02X}]", static_cast<uint8_t>(reg), fmt::join(data, ", "));
    return data;
}

std::optional<AppleMFIIC::DeviceInfo> AppleMFIIC::query_device_info() {
    if (!is_connected()) {
        spdlog::error("Not connected to Apple MFI IC");
        return std::nullopt;
    }
    
    DeviceInfo info;
    
    // Read Device Version
    auto device_version = read_register(Register::DeviceVersion);
    if (!device_version) {
        spdlog::error("Failed to read Device Version");
        return std::nullopt;
    }
    info.device_version = device_version->data()[0];
    
    // Read Authentication Revision
    auto auth_revision = read_register(Register::AuthenticationRevision);
    if (!auth_revision) {
        spdlog::error("Failed to read Authentication Revision");
        return std::nullopt;
    }
    info.authentication_revision = auth_revision->data()[0];
    
    // Read Authentication Protocol Major Version
    auto auth_major = read_register(Register::AuthenticationProtocolMajorVersion);
    if (!auth_major) {
        spdlog::error("Failed to read Authentication Protocol Major Version");
        return std::nullopt;
    }
    info.authentication_protocol_major_version = auth_major->data()[0];
    
    // Read Authentication Protocol Minor Version
    auto auth_minor = read_register(Register::AuthenticationProtocolMinorVersion);
    if (!auth_minor) {
        spdlog::error("Failed to read Authentication Protocol Minor Version");
        return std::nullopt;
    }
    info.authentication_protocol_minor_version = auth_minor->data()[0];
    
    //spdlog::info("Successfully queried Apple MFI IC: {}", info.to_string());
    return info;
}

std::string AppleMFIIC::DeviceInfo::to_string() const {
    std::ostringstream oss;
    oss << "Device Version: 0x" << std::hex << std::setw(2) << std::setfill('0') 
        << static_cast<int>(device_version)
        << ", Authentication Revision: 0x" << std::setw(2) << std::setfill('0') 
        << static_cast<int>(authentication_revision)
        << ", Authentication Protocol: " << std::dec 
        << static_cast<int>(authentication_protocol_major_version) << "."
        << static_cast<int>(authentication_protocol_minor_version);
    return oss.str();
} 