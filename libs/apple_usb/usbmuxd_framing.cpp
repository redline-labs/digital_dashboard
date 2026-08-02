// SPDX-License-Identifier: GPL-3.0-or-later

#include "apple_usb/usbmuxd_framing.h"

#include "plist/xml.h"

#include <array>
#include <cstring>

namespace apple_usb::usbmuxd
{

std::optional<Header> parseHeader(const uint8_t* data)
{
    Header header;
    std::memcpy(&header.length, data + 0, 4);
    std::memcpy(&header.version, data + 4, 4);
    std::memcpy(&header.message, data + 8, 4);
    std::memcpy(&header.tag, data + 12, 4);

    // Below the header, bodySize() underflows to a value near SIZE_MAX and
    // becomes an allocation. Above the ceiling it is simply an allocation the
    // caller never asked for. Both are the same mistake: believing the number
    // before bounding it.
    if (header.length < kHeaderSize || header.length > kMaxRequestBytes)
    {
        return std::nullopt;
    }
    return header;
}

std::vector<uint8_t> encodeReply(uint32_t tag, const plist::Value& dict)
{
    const std::string xml = plist::encodeXml(dict);
    const std::array<uint32_t, 4> header = {
        static_cast<uint32_t>(kHeaderSize + xml.size()), kPlistVersion, kPlistMessage, tag};

    std::vector<uint8_t> out(kHeaderSize + xml.size());
    std::memcpy(out.data(), header.data(), kHeaderSize);
    std::memcpy(out.data() + kHeaderSize, xml.data(), xml.size());
    return out;
}

plist::Value resultDict(int number)
{
    plist::Value d = plist::Value::dict();
    d.set("MessageType", plist::Value::string("Result"));
    d.set("Number", plist::Value::integer(number));
    return d;
}

std::string dictString(const plist::Value& dict, const char* key)
{
    const plist::Value* node = dict.find(key);
    if (node == nullptr || !node->isString())
    {
        return {};
    }
    return node->asString();
}

std::string alternateUdidForm(const std::string& udid)
{
    if (udid.size() == 24 && udid.find('-') == std::string::npos)
    {
        return udid.substr(0, 8) + "-" + udid.substr(8);
    }
    if (udid.size() == 25 && udid[8] == '-')
    {
        return udid.substr(0, 8) + udid.substr(9);
    }
    return {};
}

}  // namespace apple_usb::usbmuxd
