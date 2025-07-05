#include "apple_mfi_ic/apple_mfi_ic.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <algorithm>
#include <spdlog/fmt/ranges.h> // Required for fmt::join

int main() {
    // Set up logging
    spdlog::set_level(spdlog::level::debug);
    
    // Create the Apple MFI IC instance
    AppleMFIIC mfi_ic;
    
    // Initialize the connection
    if (!mfi_ic.init()) {
        SPDLOG_ERROR("Failed to initialize Apple MFI IC connection");
        return 1;
    }
    
    SPDLOG_INFO("Successfully connected to Apple MFI IC");
    
    // Query all device information
    auto device_info = mfi_ic.query_device_info();
    if (!device_info) {
        SPDLOG_ERROR("Failed to query device information");
        return 1;
    }
    
    auto value = mfi_ic.read_register(AppleMFIIC::Register::AccessoryCertificateDataLength, 2);
    if (!value) {
        SPDLOG_ERROR("Failed to read Accessory Certificate Data Length");
        return 1;
    }

    uint16_t cert_length = (value->data()[0] << 8) | value->data()[1];
    SPDLOG_INFO("Accessory Certificate Data Length: {} bytes", cert_length);

    // Now read the actual certificate data
    uint16_t current_offset = 0;
    uint8_t register_address = static_cast<uint8_t>(AppleMFIIC::Register::AccessoryCertificateData);
    while (current_offset < cert_length)
    {
        uint16_t chunk_size = std::min(static_cast<uint16_t>(128u), static_cast<uint16_t>(cert_length - current_offset));
        auto certificate_data = mfi_ic.read_register(static_cast<AppleMFIIC::Register>(register_address), chunk_size);
        if (!certificate_data) {
            SPDLOG_ERROR("Failed to read Accessory Certificate Data");
            return 1;
        }
        current_offset += chunk_size;
        ++register_address;
    }
    /*
    auto certificate_data_0 = mfi_ic.read_register(AppleMFIIC::Register::AccessoryCertificateData, 128u);
    if (!certificate_data_0) {
        SPDLOG_ERROR("Failed to read Accessory Certificate Data");
        return 1;
    }*/

    // Print the device information
    std::cout << "\nApple MFI IC Information:\n";
    std::cout << "-------------------------\n";
    std::cout << "Device Version: 0x" << std::hex 
              << static_cast<int>(device_info->device_version) << std::dec << "\n";
    std::cout << "Authentication Revision: 0x" << std::hex 
              << static_cast<int>(device_info->authentication_revision) << std::dec << "\n";
    std::cout << "Authentication Protocol Version: " 
              << static_cast<int>(device_info->authentication_protocol_major_version) 
              << "." << static_cast<int>(device_info->authentication_protocol_minor_version) << "\n";
    
    // Close the connection
    mfi_ic.close();

    return 0;
} 