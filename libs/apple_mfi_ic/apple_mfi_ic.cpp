#include "apple_mfi_ic/apple_mfi_ic.h"
#include "mcp2221a/mcp2221a.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h> // Required for fmt::join
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>

// OpenSSL includes
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs7.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/asn1.h>

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

std::optional<AppleMFIIC::CertificateInfo> AppleMFIIC::parse_certificate(const std::vector<uint8_t>& cert_data)
{
    if (cert_data.empty()) {
        SPDLOG_ERROR("Certificate data is empty");
        return std::nullopt;
    }
    
    // Create a BIO from the certificate data
    BIO* bio = BIO_new_mem_buf(cert_data.data(), static_cast<int>(cert_data.size()));
    if (!bio) {
        SPDLOG_ERROR("Failed to create BIO from certificate data");
        return std::nullopt;
    }
    
    // Try to parse as PKCS#7 first
    PKCS7* pkcs7 = d2i_PKCS7_bio(bio, nullptr);
    X509* cert = nullptr;
    
    if (pkcs7) {
        SPDLOG_DEBUG("Certificate data appears to be PKCS#7 format");
        
        // Extract the certificate from PKCS#7
        STACK_OF(X509)* certs = nullptr;
        int type = OBJ_obj2nid(pkcs7->type);
        
        if (type == NID_pkcs7_signed) {
            certs = pkcs7->d.sign->cert;
        } else if (type == NID_pkcs7_signedAndEnveloped) {
            certs = pkcs7->d.signed_and_enveloped->cert;
        }
        
        if (certs && sk_X509_num(certs) > 0) {
            cert = sk_X509_value(certs, 0); // Get the first certificate
            X509_up_ref(cert); // Increment reference count
        }
        
        PKCS7_free(pkcs7);
    }
    
    BIO_free(bio);
    
    if (!cert) {
        SPDLOG_ERROR("Failed to parse certificate data as PKCS#7.");
        return std::nullopt;
    }
    
    CertificateInfo info;
    info.is_valid = true;
    
    // Extract subject
    char* subject_str = X509_NAME_oneline(X509_get_subject_name(cert), nullptr, 0);
    if (subject_str) {
        info.subject = subject_str;
        OPENSSL_free(subject_str);
    }
    
    // Extract issuer
    char* issuer_str = X509_NAME_oneline(X509_get_issuer_name(cert), nullptr, 0);
    if (issuer_str) {
        info.issuer = issuer_str;
        OPENSSL_free(issuer_str);
    }
    
    // Extract serial number
    ASN1_INTEGER* serial = X509_get_serialNumber(cert);
    if (serial) {
        BIGNUM* bn = ASN1_INTEGER_to_BN(serial, nullptr);
        if (bn) {
            char* serial_str = BN_bn2hex(bn);
            if (serial_str) {
                info.serial_number = serial_str;
                OPENSSL_free(serial_str);
            }
            BN_free(bn);
        }
    }
    
    // Extract validity dates
    const ASN1_TIME* not_before = X509_get0_notBefore(cert);
    const ASN1_TIME* not_after = X509_get0_notAfter(cert);
    
    if (not_before) {
        BIO* time_bio = BIO_new(BIO_s_mem());
        if (ASN1_TIME_print(time_bio, not_before)) {
            char* time_str;
            long time_len = BIO_get_mem_data(time_bio, &time_str);
            info.not_before = std::string(time_str, time_len);
        }
        BIO_free(time_bio);
    }
    
    if (not_after) {
        BIO* time_bio = BIO_new(BIO_s_mem());
        if (ASN1_TIME_print(time_bio, not_after)) {
            char* time_str;
            long time_len = BIO_get_mem_data(time_bio, &time_str);
            info.not_after = std::string(time_str, time_len);
        }
        BIO_free(time_bio);
    }
    
    // Extract public key algorithm
    EVP_PKEY* pkey = X509_get_pubkey(cert);
    if (pkey) {
        int pkey_type = EVP_PKEY_base_id(pkey);
        switch (pkey_type) {
            case EVP_PKEY_RSA:
                info.public_key_algorithm = "RSA";
                break;
            case EVP_PKEY_EC:
                info.public_key_algorithm = "EC";
                break;
            case EVP_PKEY_DSA:
                info.public_key_algorithm = "DSA";
                break;
            default:
                info.public_key_algorithm = "Unknown";
                break;
        }
        EVP_PKEY_free(pkey);
    }
    
    // Extract signature algorithm
    const X509_ALGOR* sig_alg;
    X509_get0_signature(nullptr, &sig_alg, cert);
    if (sig_alg) {
        int sig_nid = OBJ_obj2nid(sig_alg->algorithm);
        const char* sig_name = OBJ_nid2ln(sig_nid);
        if (sig_name) {
            info.signature_algorithm = sig_name;
        }
    }
    
    // Extract Subject Alternative Names
    STACK_OF(GENERAL_NAME)* san_names = static_cast<STACK_OF(GENERAL_NAME)*>(
        X509_get_ext_d2i(cert, NID_subject_alt_name, nullptr, nullptr));
    
    if (san_names) {
        int san_count = sk_GENERAL_NAME_num(san_names);
        for (int i = 0; i < san_count; i++) {
            GENERAL_NAME* gen = sk_GENERAL_NAME_value(san_names, i);
            if (gen->type == GEN_DNS) {
                unsigned char* dns_name = nullptr;
                int dns_len = ASN1_STRING_to_UTF8(&dns_name, gen->d.dNSName);
                if (dns_len > 0 && dns_name) {
                    info.subject_alt_names.emplace_back(reinterpret_cast<char*>(dns_name), dns_len);
                    OPENSSL_free(dns_name);
                }
            }
        }
        sk_GENERAL_NAME_pop_free(san_names, GENERAL_NAME_free);
    }
    
    X509_free(cert);

    return info;
}

std::optional<AppleMFIIC::CertificateInfo> AppleMFIIC::read_and_parse_certificate()
{
    auto cert_data = read_certificate_data();
    if (cert_data.empty()) {
        SPDLOG_ERROR("Failed to read certificate data");
        return std::nullopt;
    }
    
    return parse_certificate(cert_data);
}

std::optional<std::vector<uint8_t>> AppleMFIIC::sign_challenge(const std::vector<uint8_t>& challenge_data)
{
    if (!is_connected()) {
        SPDLOG_ERROR("Not connected to Apple MFI IC");
        return std::nullopt;
    }
    
    if (challenge_data.empty() || challenge_data.size() > 20) {
        SPDLOG_ERROR("Challenge data must be between 1 and 20 bytes");
        return std::nullopt;
    }
    
    SPDLOG_DEBUG("Starting challenge-response authentication with {} bytes of challenge data", challenge_data.size());
    
    // Step 1: Write Challenge Data Length (0x20)
    uint16_t challenge_length = static_cast<uint16_t>(challenge_data.size());
    std::vector<uint8_t> length_write = {
        static_cast<uint8_t>(Register::ChallengeDataLength),
        static_cast<uint8_t>((challenge_length >> 8) & 0xFF),  // High byte
        static_cast<uint8_t>(challenge_length & 0xFF)          // Low byte
    };
    
    if (!mcp2221a_.i2c_write(I2C_ADDRESS, length_write)) {
        SPDLOG_ERROR("Failed to write challenge data length");
        return std::nullopt;
    }
    
    SPDLOG_DEBUG("Wrote challenge data length: {} bytes", challenge_length);
    
    // Step 2: Write Challenge Data (0x21)
    std::vector<uint8_t> challenge_write;
    challenge_write.push_back(static_cast<uint8_t>(Register::ChallengeData));
    challenge_write.insert(challenge_write.end(), challenge_data.begin(), challenge_data.end());
    
    if (!mcp2221a_.i2c_write(I2C_ADDRESS, challenge_write)) {
        SPDLOG_ERROR("Failed to write challenge data");
        return std::nullopt;
    }
    
    SPDLOG_DEBUG("Wrote challenge data: {} bytes", challenge_data.size());
    
    // TODO: It seems like the MFi IC is busy after this.  We should wait for it to be ready.
    std::this_thread::sleep_for(std::chrono::milliseconds(10u));

    // Step 3: Start Authentication (0x10) - Write 0x01 to start the process
    std::vector<uint8_t> auth_start_write = {
        static_cast<uint8_t>(Register::AuthenticationControlAndStatus),
        0x01
    };
    
    if (!mcp2221a_.i2c_write(I2C_ADDRESS, auth_start_write)) {
        SPDLOG_ERROR("Failed to start authentication process");
        return std::nullopt;
    }
    
    SPDLOG_DEBUG("Started authentication process");

    // It seems like its on the order of 400ms to complete the authentication.
    // Lets wait the majority of the time here.
    std::this_thread::sleep_for(std::chrono::milliseconds(400u));
    
    // Step 4: Poll Authentication Control and Status (0x10) until ready
    bool authentication_complete = false;
    const int max_attempts = 10;  // Give it up to 1 second
    
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        // It seems like its on the order of 400ms to complete the authentication.
        std::this_thread::sleep_for(std::chrono::milliseconds(100u));
        
        auto status_data = read_register(Register::AuthenticationControlAndStatus, 1);
        if (!status_data) {
            SPDLOG_ERROR("Failed to read authentication status on attempt {}", attempt);
            continue;
        }
        
        uint8_t status = status_data->data()[0];
        SPDLOG_DEBUG("Authentication status on attempt {}: 0x{:02x}", attempt, status);
        
        if (status == 0x10) {
            // Authentication complete
            authentication_complete = true;
            SPDLOG_DEBUG("Authentication completed after {} attempts", attempt + 1);
            break;
        } else if (status == 0x01) {
            // Still processing
            continue;
        } else {
            SPDLOG_WARN("Unexpected authentication status: 0x{:02x}", status);
            continue;
        }
    }
    
    if (!authentication_complete) {
        SPDLOG_ERROR("Authentication did not complete within timeout");
        return std::nullopt;
    }
    
    // Step 5: Read Challenge Response Data Length (0x11) to confirm
    auto response_length = read_register(Register::ChallengeResponseDataLength, 2);
    if (!response_length) {
        SPDLOG_ERROR("Failed to read challenge response data length");
        return std::nullopt;
    }
    
    uint16_t actual_response_length = (response_length->data()[0] << 8) | response_length->data()[1];
    SPDLOG_DEBUG("Challenge response data length: {} bytes", actual_response_length);
    
    // Step 6: Read Challenge Response Data (0x12)
    auto signature_data = read_register(Register::ChallengeResponseData, actual_response_length);
    if (!signature_data) {
        SPDLOG_ERROR("Failed to read challenge response data");
        return std::nullopt;
    }

    return *signature_data;
}

std::string AppleMFIIC::DeviceInfo::to_string() const
{
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

std::string AppleMFIIC::CertificateInfo::to_string() const
{
    std::ostringstream oss;
    oss << "Certificate Information:\n";
    oss << "  Subject: " << subject << "\n";
    oss << "  Issuer: " << issuer << "\n";
    oss << "  Serial Number: " << serial_number << "\n";
    oss << "  Valid From: " << not_before << "\n";
    oss << "  Valid To: " << not_after << "\n";
    oss << "  Public Key Algorithm: " << public_key_algorithm << "\n";
    oss << "  Signature Algorithm: " << signature_algorithm << "\n";
    oss << "  Valid: " << (is_valid ? "Yes" : "No") << "\n";
    
    if (!subject_alt_names.empty()) {
        oss << "  Subject Alternative Names:\n";
        for (const auto& san : subject_alt_names) {
            oss << "    - " << san << "\n";
        }
    }
    
    return oss.str();
}
