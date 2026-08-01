// SPDX-License-Identifier: GPL-3.0-or-later
//
// Differential test: our XML codec against libplist, the implementation every
// peer on the usbmux and lockdown sockets is actually running.
//
// Round-tripping through ourselves proves only self-consistency. What matters is
// that libplist can read what we write and that we can read what libplist
// writes, so every case here crosses the boundary in both directions:
//
//     ours -> encodeXml -> libplist parse  -> libplist re-encode -> ours parse
//     ours -> libplist encode -> ours parse
//
// This test exists only while libimobiledevice is still vendored. It is the
// evidence for each step that removes a piece of it; when the dependency goes,
// so does this file.
#include "plist/binary.h"
#include "plist/xml.h"

#include <plist/plist.h>

#include <spdlog/spdlog.h>

#include <cstring>
#include <string>
#include <vector>

namespace
{

using plist::Bytes;
using plist::Value;

int failures = 0;

void expect(bool condition, const std::string& what)
{
    if (!condition)
    {
        ++failures;
        SPDLOG_ERROR("FAIL: {}", what);
    }
}

// --- bridging between the two models ----------------------------------------

// Converts one of our Values into libplist's tree, so a case can be stated once
// and driven through both implementations.
plist_t toLibplist(const Value& value)
{
    switch (value.type())
    {
        case Value::Type::Null:
            return plist_new_string("");
        case Value::Type::Bool:
            return plist_new_bool(value.asBool() ? 1 : 0);
        case Value::Type::Integer:
            return plist_new_int(value.asInteger());
        case Value::Type::Real:
            return plist_new_real(value.asReal());
        case Value::Type::String:
            return plist_new_string(value.asString().c_str());
        case Value::Type::Data:
        {
            const Bytes& bytes = value.asData();
            return plist_new_data(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        }
        case Value::Type::Date:
            return plist_new_date(static_cast<int32_t>(value.asDate()), 0);
        case Value::Type::Array:
        {
            plist_t array = plist_new_array();
            for (size_t i = 0; i < value.size(); ++i)
            {
                plist_array_append_item(array, toLibplist(value.at(i)));
            }
            return array;
        }
        case Value::Type::Dict:
        {
            plist_t dict = plist_new_dict();
            for (size_t i = 0; i < value.size(); ++i)
            {
                plist_dict_set_item(dict, value.keys()[i].c_str(), toLibplist(value.valueAt(i)));
            }
            return dict;
        }
    }
    return plist_new_string("");
}

std::string libplistToXml(plist_t node)
{
    char* xml = nullptr;
    uint32_t len = 0;
    plist_to_xml(node, &xml, &len);
    std::string out(xml != nullptr ? xml : "", len);
    if (xml != nullptr)
    {
        plist_mem_free(xml);
    }
    return out;
}

// --- the two directions ------------------------------------------------------

// Ours -> libplist. Proves a real peer can read our output.
void checkLibplistReadsOurs(const Value& value, const std::string& what)
{
    const std::string ours = plist::encodeXml(value);

    plist_t parsed = nullptr;
    plist_from_xml(ours.c_str(), static_cast<uint32_t>(ours.size()), &parsed);
    if (parsed == nullptr)
    {
        expect(false, what + ": libplist could not parse our XML");
        return;
    }

    // Re-serialize with libplist and read that back with our decoder. If both
    // implementations agree on the meaning, the Value must survive the trip.
    const std::string theirs = libplistToXml(parsed);
    plist_free(parsed);

    const auto back = plist::decodeXml(theirs);
    if (!back)
    {
        expect(false, what + ": we could not parse libplist's re-encoding");
        return;
    }
    expect(*back == value, what + ": value changed crossing libplist");
}

// libplist -> ours. Proves we can read a real peer's output.
void checkOursReadsLibplist(const Value& value, const std::string& what)
{
    plist_t node = toLibplist(value);
    const std::string theirs = libplistToXml(node);
    plist_free(node);

    const auto back = plist::decodeXml(theirs);
    if (!back)
    {
        expect(false, what + ": we could not parse libplist's XML");
        SPDLOG_ERROR("document was:\n{}", theirs);
        return;
    }
    expect(*back == value, what + ": value changed reading libplist's XML");
}

void both(const Value& value, const std::string& what)
{
    checkLibplistReadsOurs(value, what);
    checkOursReadsLibplist(value, what);
}

// --- cases -------------------------------------------------------------------

void testScalars()
{
    both(Value::string("hello"), "plain string");
    both(Value::string(""), "empty string");
    both(Value::string("a & b < c > d \" e ' f"), "characters needing escapes");
    both(Value::string("caf\xC3\xA9 \xE2\x98\x83 \xF0\x9F\x9A\x97"), "multi-byte UTF-8");
    both(Value::boolean(true), "true");
    both(Value::boolean(false), "false");
    both(Value::integer(0), "zero");
    both(Value::integer(-1), "negative");
    both(Value::integer(2147483647), "int32 max");
    both(Value::integer(9223372036854775807LL), "int64 max");
    both(Value::data({}), "empty data");
    both(Value::data({0x00, 0x01, 0xFE, 0xFF}), "small data");

    Bytes all(256);
    for (size_t i = 0; i < all.size(); ++i)
    {
        all[i] = static_cast<uint8_t>(i);
    }
    both(Value::data(all), "256-byte data (line wrapping)");

    both(Value::data({0x41}), "data length 1");
    both(Value::data({0x41, 0x42}), "data length 2");
    both(Value::data({0x41, 0x42, 0x43}), "data length 3");
}

void testContainers()
{
    both(Value::dict(), "empty dict");
    both(Value::array(), "empty array");

    Value inner = Value::array();
    inner.push(Value::integer(1));
    inner.push(Value::string("two"));
    inner.push(Value::boolean(false));
    inner.push(Value::data({0xDE, 0xAD, 0xBE, 0xEF}));

    Value dict = Value::dict();
    dict.set("array", inner);
    dict.set("nested", Value::dict());
    dict.set("string", Value::string("s"));
    both(dict, "nested containers");
}

// The exact message shapes our UsbmuxdServer exchanges with libusbmuxd.
void testUsbmuxdMessages()
{
    Value result = Value::dict();
    result.set("MessageType", Value::string("Result"));
    result.set("Number", Value::integer(0));
    both(result, "Result reply");

    Value buid = Value::dict();
    buid.set("BUID", Value::string("30142B2C-1234-4C63-9C7E-000000000000"));
    both(buid, "ReadBUID reply");

    Value props = Value::dict();
    props.set("ConnectionType", Value::string("USB"));
    props.set("SerialNumber", Value::string("00008140-000138EE0184801C"));
    props.set("DeviceID", Value::integer(1));
    props.set("LocationID", Value::integer(0));
    props.set("ProductID", Value::integer(0x12a8));

    Value entry = Value::dict();
    entry.set("DeviceID", Value::integer(1));
    entry.set("MessageType", Value::string("Attached"));
    entry.set("Properties", props);

    Value list = Value::array();
    list.push(entry);
    Value device_list = Value::dict();
    device_list.set("DeviceList", list);
    both(device_list, "ListDevices reply");

    // A pair record reply: a data blob wrapping a whole nested plist.
    Value record = Value::dict();
    const std::string pem =
        "-----BEGIN CERTIFICATE-----\n"
        "MIICujCCAaKgAwIBAgIBADANBgkqhkiG9w0BAQsFADAAMB4XDTI2MDcyNzA0NTEw\n"
        "NloXDTM2MDcyNDA0NTEwNlowADCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoC\n"
        "-----END CERTIFICATE-----\n";
    record.set("PairRecordData", Value::data(Bytes(pem.begin(), pem.end())));
    both(record, "ReadPairRecord reply");
}

// The pair record itself, in the shape libimobiledevice's userpref writes.
void testPairRecord()
{
    Value record = Value::dict();
    const auto pem = [](const char* label, size_t lines) {
        std::string s = std::string("-----BEGIN ") + label + "-----\n";
        for (size_t i = 0; i < lines; ++i)
        {
            s += "MIICujCCAaKgAwIBAgIBADANBgkqhkiG9w0BAQsFADAAMB4XDTI2MDcyNzA0NTEw\n";
        }
        s += std::string("-----END ") + label + "-----\n";
        return Bytes(s.begin(), s.end());
    };

    record.set("DeviceCertificate", Value::data(pem("CERTIFICATE", 14)));
    record.set("HostPrivateKey", Value::data(pem("RSA PRIVATE KEY", 25)));
    record.set("HostCertificate", Value::data(pem("CERTIFICATE", 13)));
    record.set("RootPrivateKey", Value::data(pem("RSA PRIVATE KEY", 25)));
    record.set("RootCertificate", Value::data(pem("CERTIFICATE", 13)));
    record.set("SystemBUID", Value::string("30142B2C-1234-4C63-9C7E-000000000000"));
    record.set("HostID", Value::string("C1FDE0B2-6F84-4B0E-8B4C-000000000000"));
    record.set("EscrowBag", Value::data(Bytes(32, 0xAB)));
    record.set("WiFiMACAddress", Value::string("aa:bb:cc:dd:ee:ff"));

    both(record, "full pair record");
}

// Our binary decoder against libplist's binary encoder, and vice versa. The
// pair records on disk are bplist, so this path has to interoperate too.
void testBinaryFormat()
{
    Value record = Value::dict();
    record.set("SystemBUID", Value::string("30142B2C-1234-4C63-9C7E-000000000000"));
    record.set("HostID", Value::string("C1FDE0B2-6F84-4B0E-8B4C-000000000000"));
    record.set("EscrowBag", Value::data(Bytes(32, 0xAB)));
    record.set("Count", Value::integer(42));
    record.set("Flag", Value::boolean(true));

    // libplist writes binary, we read it.
    plist_t node = toLibplist(record);
    char* bin = nullptr;
    uint32_t bin_len = 0;
    plist_to_bin(node, &bin, &bin_len);
    plist_free(node);
    if (bin == nullptr)
    {
        expect(false, "libplist produced no binary plist");
        return;
    }
    const Bytes theirs(bin, bin + bin_len);
    plist_mem_free(bin);

    expect(plist::looksBinary(theirs), "libplist's output is recognised as binary");
    const auto ours = plist::decodeBinary(theirs);
    expect(ours.has_value(), "we parse libplist's binary plist");
    if (ours)
    {
        expect(*ours == record, "binary value survives libplist -> ours");
    }

    // We write binary, libplist reads it.
    const Bytes mine = plist::encodeBinary(record);
    plist_t parsed = nullptr;
    plist_from_bin(reinterpret_cast<const char*>(mine.data()),
                   static_cast<uint32_t>(mine.size()), &parsed);
    expect(parsed != nullptr, "libplist parses our binary plist");
    if (parsed != nullptr)
    {
        const std::string theirs_xml = libplistToXml(parsed);
        plist_free(parsed);
        const auto back = plist::decodeXml(theirs_xml);
        expect(back.has_value() && *back == record, "binary value survives ours -> libplist");
    }
}

}  // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);

    testScalars();
    testContainers();
    testUsbmuxdMessages();
    testPairRecord();
    testBinaryFormat();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} libplist conformance assertion(s) failed", failures);
        return 1;
    }
    SPDLOG_INFO("all libplist conformance tests passed");
    return 0;
}
