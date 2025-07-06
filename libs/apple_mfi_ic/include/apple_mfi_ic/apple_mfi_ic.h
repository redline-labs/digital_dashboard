#pragma once

#include "mcp2221a/mcp2221a.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <memory>

// Forward declarations for OpenSSL types
typedef struct x509_st X509;
typedef struct pkcs7_st PKCS7;

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
    struct DeviceInfo
    {
        uint8_t device_version;
        uint8_t authentication_revision;
        uint8_t authentication_protocol_major_version;
        uint8_t authentication_protocol_minor_version;
        
        std::string to_string() const;
    };
    
    // Structure to hold certificate information
    struct CertificateInfo
    {
        std::string subject;
        std::string issuer;
        std::string serial_number;
        std::string not_before;
        std::string not_after;
        std::string public_key_algorithm;
        std::string signature_algorithm;
        std::vector<std::string> subject_alt_names;
        bool is_valid;
        
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
    std::optional<std::vector<uint8_t>> read_register(Register reg, size_t length = 1);

    // Read the certificate data
    std::vector<uint8_t> read_certificate_data();
    
    // Parse certificate data from raw bytes
    std::optional<CertificateInfo> parse_certificate(const std::vector<uint8_t>& cert_data);
    
    // Read and parse certificate in one call
    std::optional<CertificateInfo> read_and_parse_certificate();
    
    // Sign challenge data using the MFI IC
    std::optional<std::vector<uint8_t>> sign_challenge(const std::vector<uint8_t>& challenge_data);
    
    // Query all device information
    std::optional<DeviceInfo> query_device_info();
    
private:
    MCP2221A mcp2221a_;
    bool connected_;
}; 