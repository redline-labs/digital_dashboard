// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_usb/pair_record.h"

#include "plist/binary.h"
#include "plist/xml.h"

#include <spdlog/spdlog.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <ctime>
#include <memory>

namespace apple_usb
{

namespace
{

// The key names are libimobiledevice's, because the records already on disk were
// written by it and have to keep working across this change.
constexpr const char* kRootCertificate = "RootCertificate";
constexpr const char* kRootPrivateKey = "RootPrivateKey";
constexpr const char* kHostCertificate = "HostCertificate";
constexpr const char* kHostPrivateKey = "HostPrivateKey";
constexpr const char* kDeviceCertificate = "DeviceCertificate";
constexpr const char* kEscrowBag = "EscrowBag";
constexpr const char* kHostId = "HostID";
constexpr const char* kSystemBuid = "SystemBUID";
constexpr const char* kWifiMac = "WiFiMACAddress";

std::vector<uint8_t> dataField(const plist::Value& dict, const char* key)
{
    const plist::Value* node = dict.find(key);
    if (node == nullptr)
    {
        return {};
    }
    if (node->isData())
    {
        return node->asData();
    }
    // Some writers store the PEM as a string rather than as data. The bytes are
    // the same either way, so accept both rather than fail on a cosmetic
    // difference in how a record was produced.
    if (node->isString())
    {
        const std::string& s = node->asString();
        return {s.begin(), s.end()};
    }
    return {};
}

std::string stringField(const plist::Value& dict, const char* key)
{
    const plist::Value* node = dict.find(key);
    return (node != nullptr && node->isString()) ? node->asString() : std::string();
}

}  // namespace

std::optional<PairRecord> PairRecord::parse(const std::vector<uint8_t>& blob)
{
    if (blob.empty())
    {
        return std::nullopt;
    }

    std::optional<plist::Value> root;
    if (plist::looksBinary(blob))
    {
        root = plist::decodeBinary(blob);
    }
    else
    {
        root = plist::decodeXml(
            std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
    }
    if (!root || !root->isDict())
    {
        SPDLOG_DEBUG("[pair] record is not a plist dictionary");
        return std::nullopt;
    }

    PairRecord out;
    out.root_certificate = dataField(*root, kRootCertificate);
    out.root_private_key = dataField(*root, kRootPrivateKey);
    out.host_certificate = dataField(*root, kHostCertificate);
    out.host_private_key = dataField(*root, kHostPrivateKey);
    out.device_certificate = dataField(*root, kDeviceCertificate);
    out.escrow_bag = dataField(*root, kEscrowBag);
    out.host_id = stringField(*root, kHostId);
    out.system_buid = stringField(*root, kSystemBuid);
    out.wifi_mac_address = stringField(*root, kWifiMac);

    // HostID is what StartSession identifies us by, and the root pair is the TLS
    // client identity. Without those three the record cannot open a session, so
    // it is better rejected here than half-used.
    if (out.host_id.empty())
    {
        SPDLOG_DEBUG("[pair] record has no HostID");
        return std::nullopt;
    }
    if (out.root_certificate.empty() || out.root_private_key.empty())
    {
        SPDLOG_DEBUG("[pair] record has no root certificate/key pair");
        return std::nullopt;
    }
    return out;
}

std::vector<uint8_t> PairRecord::encode() const
{
    plist::Value dict = plist::Value::dict();
    // Ordered as libimobiledevice writes them, so a record we rewrite stays
    // comparable with one it wrote.
    dict.set(kDeviceCertificate, plist::Value::data(device_certificate));
    dict.set(kHostPrivateKey, plist::Value::data(host_private_key));
    dict.set(kHostCertificate, plist::Value::data(host_certificate));
    dict.set(kRootPrivateKey, plist::Value::data(root_private_key));
    dict.set(kRootCertificate, plist::Value::data(root_certificate));
    dict.set(kSystemBuid, plist::Value::string(system_buid));
    dict.set(kHostId, plist::Value::string(host_id));
    if (!escrow_bag.empty())
    {
        dict.set(kEscrowBag, plist::Value::data(escrow_bag));
    }
    if (!wifi_mac_address.empty())
    {
        dict.set(kWifiMac, plist::Value::string(wifi_mac_address));
    }
    return plist::encodeBinary(dict);
}

// --- generation --------------------------------------------------------------

namespace
{

// OpenSSL objects have their own free functions and there are a lot of them
// here; these keep the generation path free of goto-style cleanup.
template <typename T, void (*Free)(T*)>
struct Deleter
{
    void operator()(T* p) const { Free(p); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using X509Ptr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using BioPtr = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;
using Asn1IntPtr = std::unique_ptr<ASN1_INTEGER, Deleter<ASN1_INTEGER, ASN1_INTEGER_free>>;
using Asn1TimePtr = std::unique_ptr<ASN1_TIME, Deleter<ASN1_TIME, ASN1_TIME_free>>;
using ExtPtr = std::unique_ptr<X509_EXTENSION, Deleter<X509_EXTENSION, X509_EXTENSION_free>>;

// Ten years. Long enough that a paired car never has to re-prompt for trust
// because a certificate quietly expired.
constexpr long kValiditySeconds = 60L * 60L * 24L * 365L * 10L;

bool addExtension(X509* cert, int nid, const char* value)
{
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, nullptr, cert, nullptr, nullptr, 0);

    ExtPtr ext(X509V3_EXT_conf_nid(nullptr, &ctx, nid, value));
    if (ext == nullptr)
    {
        return false;
    }
    return X509_add_ext(cert, ext.get(), -1) == 1;
}

// The parts every one of the three certificates shares: serial 0, v3, and a
// ten-year window opening now.
bool initCertificate(X509* cert)
{
    Asn1IntPtr serial(ASN1_INTEGER_new());
    if (serial == nullptr || ASN1_INTEGER_set(serial.get(), 0) != 1 ||
        X509_set_serialNumber(cert, serial.get()) != 1)
    {
        return false;
    }

    // 2 means v3 -- the version field is zero-based, and v3 is what carries
    // extensions.
    if (X509_set_version(cert, 2) != 1)
    {
        return false;
    }

    const std::time_t now = std::time(nullptr);
    Asn1TimePtr when(ASN1_TIME_new());
    if (when == nullptr)
    {
        return false;
    }
    if (ASN1_TIME_set(when.get(), now) == nullptr || X509_set1_notBefore(cert, when.get()) != 1)
    {
        return false;
    }
    if (ASN1_TIME_set(when.get(), now + kValiditySeconds) == nullptr ||
        X509_set1_notAfter(cert, when.get()) != 1)
    {
        return false;
    }
    return true;
}

std::vector<uint8_t> bioBytes(BIO* bio)
{
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    if (len <= 0 || data == nullptr)
    {
        return {};
    }
    return {data, data + len};
}

std::vector<uint8_t> certificateToPem(X509* cert)
{
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (bio == nullptr || PEM_write_bio_X509(bio.get(), cert) <= 0)
    {
        return {};
    }
    return bioBytes(bio.get());
}

std::vector<uint8_t> privateKeyToPem(EVP_PKEY* key)
{
    BioPtr bio(BIO_new(BIO_s_mem()));
    if (bio == nullptr ||
        PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) <= 0)
    {
        return {};
    }
    return bioBytes(bio.get());
}

// The device hands its public key over as PEM SubjectPublicKeyInfo ("BEGIN
// PUBLIC KEY").
//
// Only that spelling. libimobiledevice also accepts a bare PKCS#1 body ("BEGIN
// RSA PUBLIC KEY") but only on OpenSSL 1.x -- its OpenSSL 3 path is
// PEM_read_bio_PUBKEY alone, because the PKCS#1 reader is deprecated there. We
// build against OpenSSL 3, so matching that is both warning-free and the same
// coverage the reference gives on this platform.
PkeyPtr readDevicePublicKey(const std::vector<uint8_t>& pem)
{
    BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (bio == nullptr)
    {
        return nullptr;
    }
    return PkeyPtr(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr));
}

}  // namespace

std::optional<PairRecord> PairRecord::generate(const std::vector<uint8_t>& device_public_key_pem,
                                               const std::string& system_buid,
                                               const std::string& host_id)
{
    if (device_public_key_pem.empty() || host_id.empty())
    {
        return std::nullopt;
    }

    PkeyPtr root_key(EVP_RSA_gen(2048));
    PkeyPtr host_key(EVP_RSA_gen(2048));
    if (root_key == nullptr || host_key == nullptr)
    {
        SPDLOG_ERROR("[pair] RSA key generation failed");
        return std::nullopt;
    }

    // Root: self-signed, and the only CA in the picture. Everything else chains
    // to it, and it is what the TLS sessions present afterwards.
    X509Ptr root_cert(X509_new());
    if (root_cert == nullptr || !initCertificate(root_cert.get()) ||
        !addExtension(root_cert.get(), NID_basic_constraints, "critical,CA:TRUE") ||
        X509_set_pubkey(root_cert.get(), root_key.get()) != 1 ||
        X509_sign(root_cert.get(), root_key.get(), EVP_sha256()) == 0)
    {
        SPDLOG_ERROR("[pair] could not build the root certificate");
        return std::nullopt;
    }

    X509Ptr host_cert(X509_new());
    if (host_cert == nullptr || !initCertificate(host_cert.get()) ||
        !addExtension(host_cert.get(), NID_basic_constraints, "critical,CA:FALSE") ||
        !addExtension(host_cert.get(), NID_key_usage,
                      "critical,digitalSignature,keyEncipherment") ||
        X509_set_pubkey(host_cert.get(), host_key.get()) != 1 ||
        X509_sign(host_cert.get(), root_key.get(), EVP_sha256()) == 0)
    {
        SPDLOG_ERROR("[pair] could not build the host certificate");
        return std::nullopt;
    }

    // Device: the phone's own public key, wrapped in a certificate our root
    // vouches for. This is what tells the device we accept it later.
    PkeyPtr device_key = readDevicePublicKey(device_public_key_pem);
    if (device_key == nullptr)
    {
        SPDLOG_ERROR("[pair] could not read the device public key");
        return std::nullopt;
    }

    X509Ptr device_cert(X509_new());
    if (device_cert == nullptr || !initCertificate(device_cert.get()) ||
        !addExtension(device_cert.get(), NID_basic_constraints, "critical,CA:FALSE") ||
        X509_set_pubkey(device_cert.get(), device_key.get()) != 1 ||
        !addExtension(device_cert.get(), NID_subject_key_identifier, "hash") ||
        !addExtension(device_cert.get(), NID_key_usage,
                      "critical,digitalSignature,keyEncipherment") ||
        X509_sign(device_cert.get(), root_key.get(), EVP_sha256()) == 0)
    {
        SPDLOG_ERROR("[pair] could not build the device certificate");
        return std::nullopt;
    }

    PairRecord record;
    record.root_certificate = certificateToPem(root_cert.get());
    record.root_private_key = privateKeyToPem(root_key.get());
    record.host_certificate = certificateToPem(host_cert.get());
    record.host_private_key = privateKeyToPem(host_key.get());
    record.device_certificate = certificateToPem(device_cert.get());
    record.system_buid = system_buid;
    record.host_id = host_id;

    if (record.root_certificate.empty() || record.root_private_key.empty() ||
        record.host_certificate.empty() || record.host_private_key.empty() ||
        record.device_certificate.empty())
    {
        SPDLOG_ERROR("[pair] PEM encoding failed");
        return std::nullopt;
    }
    return record;
}

}  // namespace apple_usb
