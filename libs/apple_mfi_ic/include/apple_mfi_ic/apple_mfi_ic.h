#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Forward declaration
class MCP2221A;

class AppleMFIIC {
public:
    // MFI IC I2C address
    static constexpr uint8_t I2C_ADDRESS = 0x11;
    
    // Register addresses
    enum class Register : uint8_t {
        DeviceVersion = 0x00,
        AuthenticationRevision = 0x01,
        AuthenticationProtocolMajorVersion = 0x02,
        AuthenticationProtocolMinorVersion = 0x03,
        ErrorCode = 0x05,
    
        AuthenticationControlAndStatus = 0x10,
        ChallengeResponseDataLength = 0x11,
        ChallengeResponseData = 0x12,
        
        ChallengeDataLength = 0x20,
        ChallengeData = 0x21,
        
        AccessoryCertificateDataLength = 0x30,
        AccessoryCertificateData = 0x31,
        
        SelfTestStatus = 0x40,
        SystemEventCounter = 0x4D
    };
    
    // Structure to hold all the queried information
    struct DeviceInfo {
        uint8_t device_version;
        uint8_t authentication_revision;
        uint8_t authentication_protocol_major_version;
        uint8_t authentication_protocol_minor_version;
        
        std::string to_string() const;
    };
    
    AppleMFIIC();
    ~AppleMFIIC();
    
    // Initialize the connection through MCP2221A
    bool init();
    
    // Close the connection
    void close();
    
    // Check if connected
    bool is_connected() const;
    
    // Read a single register
    std::optional<uint8_t> read_register(Register reg);
    
    // Query all device information
    std::optional<DeviceInfo> query_device_info();
    
private:
    std::unique_ptr<MCP2221A> mcp2221a_;
    bool connected_;
}; 