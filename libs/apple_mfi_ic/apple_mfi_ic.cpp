#include "apple_mfi_ic/apple_mfi_ic.h"
#include "mcp2221a/mcp2221a.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h> // Required for fmt::join
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

AppleMFIIC::AppleMFIIC() 
    : mcp2221a_{}
    , connected_(false) 
{
}

AppleMFIIC::~AppleMFIIC()
{
    close();
}

bool AppleMFIIC::init()
{
    if (!mcp2221a_.open())
    {
        SPDLOG_ERROR("Failed to open MCP2221A device");
        return false;
    }
    
    // Set I2C speed to 400kHz (standard speed for MFI communication)
    if (!mcp2221a_.set_i2c_speed(100000)) {
        SPDLOG_ERROR("Failed to set I2C speed");
        mcp2221a_.close();
        return false;
    }

    // We need to do a dummy read to the MFi IC to "wake it up".  Not sure why,
    // but it doesn't seem to respond to the first interaction.
    if (mcp2221a_.i2c_write(I2C_ADDRESS, {}) == false)
    {
        SPDLOG_ERROR("Failed to write to Apple MFI IC");
        mcp2221a_.close();
        return false;
    }

    mcp2221a_.cancel();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    connected_ = true;
    return true;
}

void AppleMFIIC::close() {
    mcp2221a_.close();
    connected_ = false;
}

bool AppleMFIIC::is_connected() const {
    return connected_ && mcp2221a_.is_open();
}

std::optional<std::vector<uint8_t>> AppleMFIIC::read_register(Register reg, size_t length)
{
    if (!is_connected())
    {
        SPDLOG_ERROR("Not connected to Apple MFI IC");
        return std::nullopt;
    }
    
    // Write the register address
    std::vector<uint8_t> reg_addr = {static_cast<uint8_t>(reg)};
    if (!mcp2221a_.i2c_write(I2C_ADDRESS, reg_addr))
    {
        SPDLOG_ERROR("Failed to write register address 0x{:02x}", static_cast<uint8_t>(reg));
        return std::nullopt;
    }
    
    // Read the register value
    auto data = mcp2221a_.i2c_read(I2C_ADDRESS, length);
    if (data.empty())
    {
        SPDLOG_ERROR("Failed to read register 0x{:02x}", static_cast<uint8_t>(reg));
        return std::nullopt;
    }
    
    return data;
}

std::optional<AppleMFIIC::DeviceInfo> AppleMFIIC::query_device_info() {
    if (!is_connected()) {
        SPDLOG_ERROR("Not connected to Apple MFI IC");
        return std::nullopt;
    }
    
    DeviceInfo info;
    
    // Read Device Version
    auto device_version = read_register(Register::DeviceVersion);
    if (!device_version)
    {
        SPDLOG_ERROR("Failed to read Device Version");
        return std::nullopt;
    }
    info.device_version = device_version->data()[0];
    
    // Read Authentication Revision
    auto auth_revision = read_register(Register::AuthenticationRevision);
    if (!auth_revision)
    {
        SPDLOG_ERROR("Failed to read Authentication Revision");
        return std::nullopt;
    }
    info.authentication_revision = auth_revision->data()[0];
    
    // Read Authentication Protocol Major Version
    auto auth_major = read_register(Register::AuthenticationProtocolMajorVersion);
    if (!auth_major)
    {
        SPDLOG_ERROR("Failed to read Authentication Protocol Major Version");
        return std::nullopt;
    }
    info.authentication_protocol_major_version = auth_major->data()[0];
    
    // Read Authentication Protocol Minor Version
    auto auth_minor = read_register(Register::AuthenticationProtocolMinorVersion);
    if (!auth_minor)
    {
        SPDLOG_ERROR("Failed to read Authentication Protocol Minor Version");
        return std::nullopt;
    }
    info.authentication_protocol_minor_version = auth_minor->data()[0];
    
    //spdlog::info("Successfully queried Apple MFI IC: {}", info.to_string());
    return info;
}

std::vector<uint8_t> AppleMFIIC::read_certificate_data()
{
    auto value = read_register(AppleMFIIC::Register::AccessoryCertificateDataLength, 2);
    if (!value)
    {
        SPDLOG_ERROR("Failed to read Accessory Certificate Data Length");
        return {};
    }

    // TODO Do sanity check on the length based on the device protocol version.

    uint16_t cert_length = (value->data()[0] << 8) | value->data()[1];
    SPDLOG_DEBUG("Accessory Certificate Data Length: {} bytes", cert_length);

    std::vector<uint8_t> certificate_data;
    certificate_data.reserve(cert_length);

    // Now read the actual certificate data
    uint16_t current_offset = 0;
    uint8_t register_address = static_cast<uint8_t>(AppleMFIIC::Register::AccessoryCertificateData);
    while (current_offset < cert_length)
    {
        uint16_t chunk_size = std::min(static_cast<uint16_t>(128u), static_cast<uint16_t>(cert_length - current_offset));
        auto chunk_data = read_register(static_cast<AppleMFIIC::Register>(register_address), chunk_size);
        if (!chunk_data) {
            SPDLOG_ERROR("Failed to read Accessory Certificate Data");
            return {};
        }

        certificate_data.insert(certificate_data.end(), chunk_data->begin(), chunk_data->end());

        current_offset += chunk_size;
        register_address += 1u;
    }
    
    return certificate_data;
}
