#include "apple_mfi_ic/apple_mfi_ic.h"
#include <spdlog/spdlog.h>
#include <iostream>
#include <spdlog/fmt/ranges.h> // Required for fmt::join

int main() {
    // Set up logging
    spdlog::set_level(spdlog::level::info);
    spdlog::info("Apple MFI IC Demo");
    
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
    //spdlog::info("Accessory Certificate Data Length: [{:02x}]", fmt::join(value->data(), ", "));

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