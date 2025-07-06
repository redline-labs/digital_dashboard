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

    SPDLOG_INFO("Device Version: 0x{:02X}", device_info->device_version);
    SPDLOG_INFO("Authentication Revision: 0x{:02X}", device_info->authentication_revision);
    SPDLOG_INFO("Authentication Protocol Version: {}.{}", device_info->authentication_protocol_major_version, device_info->authentication_protocol_minor_version);
    
    // Read and parse the certificate
    auto certificate_info = mfi_ic.read_and_parse_certificate();
    if (!certificate_info) {
        SPDLOG_ERROR("Failed to read and parse certificate");
        return 1;
    }

    SPDLOG_INFO(certificate_info->to_string());

    // Close the connection
    mfi_ic.close();

    return 0;
} 