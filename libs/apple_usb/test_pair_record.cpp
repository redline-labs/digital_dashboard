// SPDX-License-Identifier: GPL-3.0-or-later
//
// Pair record generation and parsing, without a phone.
//
// The device-facing half of pairing cannot be tested here -- only an iPhone can
// say whether it accepts our certificates. What can be pinned down is everything
// up to the wire: that the three certificates have the shape lockdown expects,
// that the chain actually verifies, and that a record survives the round trip
// through the on-disk format. Interop with libimobiledevice's own records is
// covered by parsing one it wrote, below.
#include "apple_usb/pair_record.h"

#include <spdlog/spdlog.h>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <string>
#include <vector>

namespace
{

using apple_usb::PairRecord;
using Bytes = std::vector<uint8_t>;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

// A stand-in for the key GetValue("DevicePublicKey") returns: PEM
// SubjectPublicKeyInfo, which is what a modern device sends.
Bytes makeDevicePublicKey()
{
    EVP_PKEY* key = EVP_RSA_gen(2048);
    if (key == nullptr)
    {
        return {};
    }
    BIO* bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PUBKEY(bio, key);
    char* data = nullptr;
    const long len = BIO_get_mem_data(bio, &data);
    Bytes out(data, data + len);
    BIO_free(bio);
    EVP_PKEY_free(key);
    return out;
}

X509* parseCert(const Bytes& pem)
{
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    X509* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return cert;
}

bool isCa(X509* cert)
{
    // X509_check_ca returns non-zero for a CA; 1 specifically for a v3 cert with
    // basicConstraints CA:TRUE.
    return X509_check_ca(cert) != 0;
}

// Confirms `cert` is signed by `issuer`'s key. The certificates carry empty
// subject and issuer names -- Apple's design, and libimobiledevice does the same
// -- so a name-based chain build cannot work and X509_verify is the real check.
bool signedBy(X509* cert, X509* issuer)
{
    EVP_PKEY* key = X509_get_pubkey(issuer);
    if (key == nullptr)
    {
        return false;
    }
    const bool ok = X509_verify(cert, key) == 1;
    EVP_PKEY_free(key);
    return ok;
}

void testGeneration()
{
    const Bytes device_key = makeDevicePublicKey();
    expect(!device_key.empty(), "test fixture device key was generated");

    const auto record =
        PairRecord::generate(device_key, "30142B2C-1234-4C63-9C7E-000000000000",
                             "C1FDE0B2-6F84-4B0E-8B4C-000000000000");
    expect(record.has_value(), "generate() succeeds");
    if (!record)
    {
        return;
    }

    expect(!record->root_certificate.empty(), "root certificate present");
    expect(!record->root_private_key.empty(), "root private key present");
    expect(!record->host_certificate.empty(), "host certificate present");
    expect(!record->host_private_key.empty(), "host private key present");
    expect(!record->device_certificate.empty(), "device certificate present");
    expect(record->host_id == "C1FDE0B2-6F84-4B0E-8B4C-000000000000", "host id is carried");
    expect(record->system_buid == "30142B2C-1234-4C63-9C7E-000000000000", "buid is carried");
    // The device supplies this in its answer to Pair, not at generation time.
    expect(record->escrow_bag.empty(), "no escrow bag before the device answers");

    X509* root = parseCert(record->root_certificate);
    X509* host = parseCert(record->host_certificate);
    X509* dev = parseCert(record->device_certificate);
    expect(root != nullptr, "root certificate parses");
    expect(host != nullptr, "host certificate parses");
    expect(dev != nullptr, "device certificate parses");

    if (root != nullptr && host != nullptr && dev != nullptr)
    {
        expect(isCa(root), "root is a CA");
        expect(!isCa(host), "host is not a CA");
        expect(!isCa(dev), "device is not a CA");

        expect(signedBy(root, root), "root is self-signed");
        expect(signedBy(host, root), "host is signed by root");
        expect(signedBy(dev, root), "device is signed by root");

        // v3 is what carries the extensions above; X509_get_version is
        // zero-based, so v3 reads as 2.
        expect(X509_get_version(root) == 2, "root is X.509 v3");
        expect(X509_get_version(host) == 2, "host is X.509 v3");
        expect(X509_get_version(dev) == 2, "device is X.509 v3");

        // The device certificate must wrap the key the device sent, or the
        // device will not recognise itself in it.
        BIO* bio = BIO_new_mem_buf(device_key.data(), static_cast<int>(device_key.size()));
        EVP_PKEY* expected = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);
        EVP_PKEY* actual = X509_get_pubkey(dev);
        expect(expected != nullptr && actual != nullptr &&
                   EVP_PKEY_eq(expected, actual) == 1,
               "device certificate carries the device's own public key");
        EVP_PKEY_free(expected);
        EVP_PKEY_free(actual);
    }

    X509_free(root);
    X509_free(host);
    X509_free(dev);

    // Two generations must not collide: a shared root key across devices would
    // mean one phone's record could impersonate another's.
    const auto second =
        PairRecord::generate(device_key, "30142B2C-1234-4C63-9C7E-000000000000",
                             "C1FDE0B2-6F84-4B0E-8B4C-000000000000");
    expect(second.has_value() && second->root_private_key != record->root_private_key,
           "each generation mints a fresh root key");
}

void testRoundTrip()
{
    const Bytes device_key = makeDevicePublicKey();
    auto record = PairRecord::generate(device_key, "BUID-VALUE", "HOST-ID-VALUE");
    expect(record.has_value(), "generate() for round trip");
    if (!record)
    {
        return;
    }
    record->escrow_bag = Bytes(32, 0xAB);
    record->wifi_mac_address = "aa:bb:cc:dd:ee:ff";

    const Bytes encoded = record->encode();
    expect(!encoded.empty(), "encode() produces bytes");

    const auto parsed = PairRecord::parse(encoded);
    expect(parsed.has_value(), "encode/parse round trips");
    if (!parsed)
    {
        return;
    }
    expect(parsed->root_certificate == record->root_certificate, "root certificate survives");
    expect(parsed->root_private_key == record->root_private_key, "root key survives");
    expect(parsed->host_certificate == record->host_certificate, "host certificate survives");
    expect(parsed->host_private_key == record->host_private_key, "host key survives");
    expect(parsed->device_certificate == record->device_certificate, "device certificate survives");
    expect(parsed->escrow_bag == record->escrow_bag, "escrow bag survives");
    expect(parsed->host_id == record->host_id, "host id survives");
    expect(parsed->system_buid == record->system_buid, "buid survives");
    expect(parsed->wifi_mac_address == record->wifi_mac_address, "wifi mac survives");
}

void testRejections()
{
    expect(!PairRecord::parse({}).has_value(), "empty blob is rejected");
    expect(!PairRecord::parse({'n', 'o', 'p', 'e'}).has_value(), "garbage is rejected");

    // A record with no HostID cannot open a session, and one with no root pair
    // cannot present a TLS identity. Both are rejected rather than half-used.
    const Bytes device_key = makeDevicePublicKey();
    auto record = PairRecord::generate(device_key, "BUID", "HOST");
    expect(record.has_value(), "fixture record");
    if (record)
    {
        PairRecord no_host = *record;
        no_host.host_id.clear();
        expect(!PairRecord::parse(no_host.encode()).has_value(), "a record with no HostID is rejected");

        PairRecord no_root = *record;
        no_root.root_certificate.clear();
        expect(!PairRecord::parse(no_root.encode()).has_value(),
               "a record with no root certificate is rejected");
    }

    expect(!PairRecord::generate({}, "BUID", "HOST").has_value(),
           "generate() rejects an empty device key");
    expect(!PairRecord::generate(makeDevicePublicKey(), "BUID", "").has_value(),
           "generate() rejects an empty host id");
    expect(!PairRecord::generate({'n', 'o', 't', ' ', 'p', 'e', 'm'}, "BUID", "HOST").has_value(),
           "generate() rejects a device key that is not PEM");
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testGeneration();
    testRoundTrip();
    testRejections();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} pair record assertion(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all pair record tests passed");
    return 0;
}
