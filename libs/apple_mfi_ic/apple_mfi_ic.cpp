#include "apple_mfi_ic/apple_mfi_ic.h"
#include "i2c_bus/i2c_bus.h"
#include <spdlog/spdlog.h>
#include <spdlog/fmt/ranges.h> // Required for fmt::join
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <cstdlib>

// OpenSSL includes
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pkcs7.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/asn1.h>

AppleMFIIC::AppleMFIIC() 
    : bus_{}
    , connected_(false) 
{
}

AppleMFIIC::AppleMFIIC(std::unique_ptr<i2c::Bus> bus)
    : bus_(std::move(bus))
    , connected_(false)
{
}

AppleMFIIC::~AppleMFIIC()
{
    close();
}

namespace
{
// The coprocessor's I2C timing, measured on the LattePanda (DesignWare
// controller, 100 kHz, 2026-09-13) with a scripted probe that kept the bus open
// and timed every transaction:
//
//   * It sleeps after ~30-60 ms of idle. The first START after that is either
//     NACKed outright or ACKed with a ~12 ms clock stretch; after a NACK it is
//     answering again within ~0.5 ms. A NACKed register-select write leaves the
//     register pointer UNSET -- a read that follows returns garbage, so the
//     write has to be repeated, never just the read.
//   * After a SUCCESSFUL register-select write it is busy for ~0.8-1 ms and
//     NACKs everything in that window; the pointer is set and a read retried
//     at 1 ms succeeds. Over a USB bridge (MCP2221A) the round trip hid this
//     window; on a native controller the read lands inside it every time, and
//     re-issuing the write on each failure just reopens the window -- which is
//     why the old "retry the pair, 20 ms apart" never read a byte here.
//
// And the host is not real-time. A thread that sleeps waits to be scheduled
// again, and on a loaded machine a 2 ms pause can come back 40 ms later -- past
// the part's idle threshold, so the retry meant to follow a wake NACK finds it
// asleep again, and so does the next. A policy of "NACK, pause, retry" fails
// exactly when the host is busy. (It did: the test's fake stopped answering on
// a loaded build machine.)
//
// So nothing that has to happen inside one of the part's windows waits for a
// sleep, and nothing counts on a window still being open. A NACK is answered by
// the next transaction AT ONCE: whatever caused it -- the part asleep, or busy
// after a select -- that next transaction is the one that works (awake ~0.5 ms
// after a wake NACK, busy ~1 ms after a select).
//
// A read's data is trusted only if the read FINISHED within kSelectWindow of
// the select STARTING. The part's idle clock runs from the end of the select to
// the start of the read, and those two driver-side times bound it from above
// whatever the scheduler did in between; kSelectWindow is under the ~30 ms idle
// threshold, so such a read cannot have met a sleeping part. A later one might
// have: a sleeping part can ACK its first START after a ~12 ms stretch, and
// what sleep does to the register pointer was never measured -- so a late read
// is discarded and the register selected again, at once. Only a long run of NACKs -- a part that is not answering --
// earns a pause, to spare the bus, and that pause may overrun by any amount:
// the next transaction wakes the part and the one after it proceeds.
//
// Giving up takes BOTH kOperationBudget of wall time and kMinAttempts: time
// alone is no evidence, because the host can stop the process for longer than
// the whole budget (a build machine did, for seconds), and an operation that
// then gives up after one attempt has not asked the part anything. Attempts
// alone would be a multi-second stall over the bridge. The count before a
// pause is generous because a NACK costs ~0.2 ms on a native controller but
// ~15 ms over the MCP2221A bridge.
constexpr auto kSelectWindow = std::chrono::milliseconds(20);
constexpr int kNacksBeforePause = 16;
constexpr auto kUnresponsivePause = std::chrono::milliseconds(2);
constexpr auto kOperationBudget = std::chrono::seconds(1);
constexpr int kMinAttempts = 2 * kNacksBeforePause;

enum class Step
{
    done,      // the operation succeeded
    progress,  // the part answered, but there is more to do (a register selected)
    nack,      // the part did not answer
};

// Runs `step` until it is done, or until kOperationBudget is spent AND it has
// had kMinAttempts, retrying at once after anything but a long run of NACKs.
template <typename StepFn>
bool until_done(StepFn&& step)
{
    using Clock = std::chrono::steady_clock;
    const auto give_up = Clock::now() + kOperationBudget;
    int nacks_in_a_row = 0;
    for (int attempts = 0; attempts < kMinAttempts || Clock::now() < give_up; ++attempts)
    {
        switch (step())
        {
            case Step::done:
                return true;
            case Step::progress:
                nacks_in_a_row = 0;
                break;
            case Step::nack:
                if (++nacks_in_a_row >= kNacksBeforePause)
                {
                    nacks_in_a_row = 0;
                    std::this_thread::sleep_for(kUnresponsivePause);
                }
                break;
        }
    }
    return false;
}

// The coprocessor's length registers are two bytes, most significant first.
uint16_t big_endian_u16(const std::vector<uint8_t>& bytes)
{
    return static_cast<uint16_t>((uint32_t{bytes[0]} << 8u) | bytes[1]);
}

// What OpenSSL printed into a memory BIO. BIO_get_mem_data returns a long and
// is negative on failure, so that case is an empty string rather than a huge
// unsigned length.
std::string asn1_time_string(const ASN1_TIME* time)
{
    std::string text;
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio != nullptr && ASN1_TIME_print(bio, time))
    {
        char* data = nullptr;
        const long length = BIO_get_mem_data(bio, &data);
        if (length > 0 && data != nullptr)
        {
            text.assign(data, static_cast<size_t>(length));
        }
    }
    BIO_free(bio);
    return text;
}
}  // namespace

bool AppleMFIIC::write_with_retry(const std::vector<uint8_t>& data)
{
    return until_done([&] { return bus_->write(I2C_ADDRESS, data) ? Step::done : Step::nack; });
}

bool AppleMFIIC::wake()
{
    // The coprocessor ignores the first transaction after it has been idle;
    // that NACK *is* the wake-up, and the next access succeeds. Retry rather
    // than treating one failure as absence.
    return until_done([&] { return bus_->read(I2C_ADDRESS, 1).empty() ? Step::nack : Step::done; });
}

bool AppleMFIIC::init(const std::string& bus_hint)
{
    // Explicit argument, then the environment (how a deployed board names its
    // coprocessor bus without every caller learning a flag), then auto-detect.
    std::string hint = bus_hint;
    if (hint.empty())
    {
        if (const char* env = std::getenv("REDLINE_MFI_I2C_DEV"); env != nullptr && *env != '\0')
        {
            hint = env;
        }
    }
    if (!bus_)
    {
        bus_ = i2c::makeBus(hint);
    }
    if (!bus_ || !bus_->open())
    {
        SPDLOG_ERROR("Failed to open the I2C bus for the Apple MFI IC");
        return false;
    }

    if (!wake())
    {
        SPDLOG_ERROR("Apple MFI IC at 0x{:02x} did not respond on {}. Check power, the SDA/SCL "
                     "wiring, and that the RESET pin is released.",
                     I2C_ADDRESS, bus_->description());
        return false;
    }

    connected_ = true;
    return true;
}

void AppleMFIIC::close()
{
    connected_ = false;
    if (bus_)
    {
        bus_->close();
    }
}

bool AppleMFIIC::is_connected() const {
    return connected_ && bus_ && bus_->is_open();
}

std::optional<std::vector<uint8_t>> AppleMFIIC::read_register(Register reg, size_t length)
{
    if (!is_connected())
    {
        SPDLOG_ERROR("Not connected to Apple MFI IC");
        return std::nullopt;
    }
    
    // Select the register, then read it back as a *separate* transaction: this
    // part rejects a combined write/read with a repeated START.
    //
    // A NACKed write means asleep or busy and leaves the pointer unset, so it
    // is sent again. A successful write is followed by the ~1 ms busy window
    // described at the top of the file, so the READ is retried on its own; the
    // pointer is already set and re-writing it would only restart the window.
    // A read that could have met the part asleep since the select is not
    // trusted, answered or not: see kSelectWindow.
    const std::vector<uint8_t> reg_addr = {static_cast<uint8_t>(reg)};
    std::vector<uint8_t> data;
    using SteadyClock = std::chrono::steady_clock;
    std::optional<SteadyClock::time_point> selected_at;  // when the successful select STARTED
    const bool ok = until_done([&] {
        if (!selected_at)
        {
            const auto started = SteadyClock::now();
            if (!bus_->write(I2C_ADDRESS, reg_addr))
            {
                return Step::nack;
            }
            selected_at = started;
            return Step::progress;
        }
        data = bus_->read(I2C_ADDRESS, length);
        const bool in_window = SteadyClock::now() - *selected_at <= kSelectWindow;
        if (!in_window)
        {
            // Too late to be sure the part stayed awake since the select,
            // answered or not: select again.
            selected_at.reset();
            return data.empty() ? Step::nack : Step::progress;
        }
        return data.empty() ? Step::nack : Step::done;
    });
    if (ok)
    {
        return data;
    }
    SPDLOG_ERROR("Failed to read register 0x{:02x}: no answer in {} ms and {} attempts", static_cast<uint8_t>(reg),
                 std::chrono::duration_cast<std::chrono::milliseconds>(kOperationBudget).count(), kMinAttempts);
    return std::nullopt;
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
    
    //SPDLOG_INFO("Successfully queried Apple MFI IC: {}", info.to_string());
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

    uint16_t cert_length = big_endian_u16(*value);
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
        info.not_before = asn1_time_string(not_before);
    }
    
    if (not_after) {
        info.not_after = asn1_time_string(not_after);
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
    
    if (!write_with_retry(length_write)) {
        SPDLOG_ERROR("Failed to write challenge data length");
        return std::nullopt;
    }
    
    SPDLOG_DEBUG("Wrote challenge data length: {} bytes", challenge_length);
    
    // Step 2: Write Challenge Data (0x21)
    std::vector<uint8_t> challenge_write;
    challenge_write.push_back(static_cast<uint8_t>(Register::ChallengeData));
    challenge_write.insert(challenge_write.end(), challenge_data.begin(), challenge_data.end());
    
    if (!write_with_retry(challenge_write)) {
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
    
    if (!write_with_retry(auth_start_write)) {
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
    
    uint16_t actual_response_length = big_endian_u16(*response_length);
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
