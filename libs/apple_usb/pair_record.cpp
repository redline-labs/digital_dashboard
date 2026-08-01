// SPDX-License-Identifier: GPL-3.0-or-later
#include "apple_usb/pair_record.h"

#include "plist/binary.h"
#include "plist/xml.h"

#include <spdlog/spdlog.h>

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

}  // namespace apple_usb
