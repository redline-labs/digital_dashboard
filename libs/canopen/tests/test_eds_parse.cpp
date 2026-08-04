// SPDX-License-Identifier: GPL-3.0-or-later
//
// The EDS parser, checked against the file it exists to read.
//
// The previous version of this test parsed an inline sample and asserted
// `od->get(1018)` under a comment explaining that "EDS sections are decimal".
// They are not, and because the sample and the assertion shared the same wrong
// assumption the test passed while every index in every real file came out
// wrong. So the first thing here is the real EDS, with hexadecimal indices
// spelled out.

#include "canopen/eds_parser.h"
#include "canopen/pdo_mapping.h"

#include <spdlog/spdlog.h>

#include <fstream>
#include <sstream>
#include <string>

namespace
{

int failures = 0;

void check(bool condition, const std::string& what)
{
    if (!condition)
    {
        SPDLOG_ERROR("FAIL: {}", what);
        ++failures;
    }
}

void dump(const std::vector<canopen::Diagnostic>& diagnostics, const char* label)
{
    for (const auto& diagnostic : diagnostics)
    {
        SPDLOG_INFO("  {}: {}", label, canopen::to_string(diagnostic));
    }
}

size_t count_errors(const std::vector<canopen::Diagnostic>& diagnostics)
{
    size_t n = 0;
    for (const auto& diagnostic : diagnostics)
    {
        if (diagnostic.severity == canopen::Severity::Error)
        {
            ++n;
        }
    }
    return n;
}

std::string read_file(const std::string& path)
{
    std::ifstream in(path);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// ============================================================================
// The real Grayhill EDS
// ============================================================================

void test_real_eds()
{
    const std::string path = std::string(CANOPEN_EDS_DIR) + "/grayhill/DS401_3K_C.eds";
    const std::string text = read_file(path);
    check(!text.empty(), "the shipped EDS is readable at " + path);

    auto result = canopen::parse_eds(text);
    dump(result.diagnostics, "parse");
    check(result.ok(), "the shipped EDS parses without errors");

    const auto& od = result.od;

    // --- indices are hexadecimal ------------------------------------------
    check(od.get(0x1018) != nullptr, "0x1018 Identity is present");
    check(od.get(1018) == nullptr, "decimal 1018 is not an index in this file");
    check(od.get(0x1A00) != nullptr, "0x1A00 is present -- a hex-lettered section is not dropped");
    check(od.get(0x100A) != nullptr, "0x100A is present -- a hex letter inside the index is fine");

    // --- VAR objects keep their bodies -------------------------------------
    // 0x1017 is a VAR: everything about it lives in the object section, and a
    // parser that only reads subN sections learns nothing but its name. The
    // node used to hardcode the expedited 2-byte SDO command byte because of
    // exactly this gap.
    const canopen::SubObject* heartbeat = od.get(0x1017, 0);
    check(heartbeat != nullptr, "0x1017 has a sub 0 body");
    if (heartbeat != nullptr)
    {
        check(heartbeat->dataType == canopen::DataType::Unsigned16, "0x1017 is UNSIGNED16");
        check(heartbeat->access == canopen::AccessType::RW, "0x1017 is rw");
        check(canopen::data_type_bits(heartbeat->dataType) == 16, "0x1017 is 16 bits wide");
    }

    const canopen::SubObject* deviceType = od.get(0x1000, 0);
    check(deviceType != nullptr, "0x1000 has a body");
    if (deviceType != nullptr)
    {
        auto value = od.defaultValue(0x1000, 0, 0x0A);
        check(value.has_value() && *value == 0x000B0191,
              "0x1000 Device Type is 0x000B0191 (digital in + digital out + analog out)");
    }

    // 0x1008's default really is the word "manufacturer"; a VISIBLE_STRING
    // default must not be mistaken for a failed number parse.
    const canopen::SubObject* deviceName = od.get(0x1008, 0);
    check(deviceName != nullptr && deviceName->dataType == canopen::DataType::VisibleString,
          "0x1008 is VISIBLE_STRING");
    check(deviceName != nullptr && deviceName->access == canopen::AccessType::CONST,
          "0x1008 is const");

    // --- $NODEID expressions ----------------------------------------------
    // Every COB-ID default is written this way. A parser that cannot resolve
    // them has to fall back to the DS401 constants, which on this device
    // happen to be right -- so the failure is invisible until the device uses
    // a non-default COB-ID.
    const canopen::SubObject* tpdo1Cobid = od.get(0x1800, 1);
    check(tpdo1Cobid != nullptr, "0x1800:01 is present");
    if (tpdo1Cobid != nullptr && tpdo1Cobid->defaultValue.has_value())
    {
        const auto* expr = std::get_if<canopen::NodeIdExpr>(&*tpdo1Cobid->defaultValue);
        check(expr != nullptr, "0x1800:01 default is a $NODEID expression, not a string");
        if (expr != nullptr)
        {
            check(expr->usesNodeId && expr->constant == 0x40000180,
                  "0x1800:01 is $NODEID+0x40000180");
        }
    }
    // Node 0x0A is Grayhill's factory default, and Table 1 gives its COB-IDs
    // as 0x18A / 0x20A / 0x30A. Masking off the CiA control bits must produce
    // exactly those.
    auto cobid = [&](uint16_t index) -> uint32_t
    {
        auto value = od.defaultValue(index, 1, 0x0A);
        return value.has_value() ? static_cast<uint32_t>(*value & 0x1FFFFFFF) : 0;
    };
    check(cobid(0x1800) == 0x18A, "TPDO1 COB-ID at node 0x0A is 0x18A");
    check(cobid(0x1400) == 0x20A, "RPDO1 COB-ID at node 0x0A is 0x20A");
    check(cobid(0x1401) == 0x30A, "RPDO2 COB-ID at node 0x0A is 0x30A");

    // --- limits, including the ones the plan corrected ---------------------
    const canopen::SubObject* backlightScalar = od.get(0x2010, 2);
    check(backlightScalar != nullptr, "0x2010:02 is present");
    if (backlightScalar != nullptr)
    {
        check(backlightScalar->dataType == canopen::DataType::Unsigned16,
              "0x2010:02 is UNSIGNED16 -- PDM Manager expects SDO response cs 0x4B");
        check(backlightScalar->lowLimit == 64 && backlightScalar->highLimit == 255,
              "0x2010:02 limits are 0x40..0xFF per manual Table 1");
        check(canopen::is_writable(backlightScalar->access), "0x2010:02 is writable");
    }
    const canopen::SubObject* txType = od.get(0x1800, 2);
    check(txType != nullptr && txType->dataType == canopen::DataType::Unsigned8,
          "0x1800:02 is UNSIGNED8 -- PDM Manager expects SDO response cs 0x4F");

    // --- objects the correction added --------------------------------------
    check(od.get(0x1010, 1) != nullptr, "0x1010:01 Store Parameters is present");
    check(od.get(0x1011, 1) != nullptr, "0x1011:01 Restore Parameters is present");
    check(od.get(0x6006) != nullptr && od.get(0x6007) != nullptr && od.get(0x6008) != nullptr,
          "the TPDO1 interrupt masks are present");

    // --- metadata ----------------------------------------------------------
    check(od.deviceInfo.lssSupported, "LSS_Supported=1 reaches the caller");
    check(od.deviceInfo.supportedBitrates.count(10) == 1 && !od.deviceInfo.supportedBitrates.at(10),
          "10 kbit/s is declared unsupported, matching the manual");
    check(od.deviceInfo.supportedBitrates.count(250) == 1
              && od.deviceInfo.supportedBitrates.at(250),
          "250 kbit/s -- the factory rate -- is supported");
    check(od.deviceInfo.supportedBitrates.count(1000) == 1
              && od.deviceInfo.supportedBitrates.at(1000),
          "1 Mbit/s -- the rate MoTeC ships -- is supported");
    check(od.deviceInfo.vendorNumber == 0x0307, "vendor ID is 0x0307");
    check(od.deviceInfo.productNumber == 0x334B, "product code is 0x334B (\"3K\")");
    check(od.mandatoryObjects.size() == 3, "three mandatory objects are declared");
    check(od.optionalObjects.size() == 18, "eighteen optional objects are declared");
    check(od.manufacturerObjects.size() == 1, "one manufacturer object is declared");
    check(od.dummyUsage.count(0x0005) == 1, "DummyUsage is captured");
    check(!od.comments.empty(), "the [Comments] provenance survives the parse");

    // --- the file is coherent ----------------------------------------------
    auto problems = canopen::validate(od);
    dump(problems, "validate");
    check(count_errors(problems) == 0, "the shipped EDS validates clean");
}

// ============================================================================
// PDO mappings
// ============================================================================

void test_pdo_mappings()
{
    const std::string text = read_file(std::string(CANOPEN_EDS_DIR) + "/grayhill/DS401_3K_C.eds");
    auto result = canopen::parse_eds(text);
    const auto& od = result.od;

    // TPDO1: the buttons. Three bytes of 0x6000, which is what the node has
    // been hardcoding -- now derived.
    auto tpdo1 = canopen::read_pdo_mapping(od, 0x1A00);
    check(tpdo1.problems.empty(), "0x1A00 maps cleanly");
    dump({}, "");
    for (const auto& problem : tpdo1.problems)
    {
        SPDLOG_ERROR("  0x1A00: {}", problem);
    }
    check(tpdo1.entries.size() == 3, "0x1A00 has three entries");
    check(tpdo1.totalBits == 24 && tpdo1.lengthBytes() == 3, "0x1A00 is three bytes");
    if (tpdo1.entries.size() == 3)
    {
        check(tpdo1.entries[0].index == 0x6000 && tpdo1.entries[0].sub == 1
                  && tpdo1.entries[0].bits == 8 && tpdo1.entries[0].bitOffset == 0,
              "0x1A00:01 is 0x6000:01, 8 bits at offset 0");
        check(tpdo1.entries[2].bitOffset == 16, "0x1A00:03 starts at bit 16");
        check(!tpdo1.entries[2].parameterName.empty(), "mapped entries carry their target's name");
    }

    // RPDO1: the indicators. Eight bytes of 0x6200 -- a full frame.
    auto rpdo1 = canopen::read_pdo_mapping(od, 0x1600);
    check(rpdo1.problems.empty(), "0x1600 maps cleanly");
    check(rpdo1.entries.size() == 8, "0x1600 has eight entries");
    check(rpdo1.totalBits == 64, "0x1600 fills a CAN frame exactly");

    // RPDO2: brightness. Two 16-bit values, which is where the runtime node's
    // "write one, blank the other" bug lives.
    auto rpdo2 = canopen::read_pdo_mapping(od, 0x1601);
    check(rpdo2.problems.empty(), "0x1601 maps cleanly");
    check(rpdo2.entries.size() == 2, "0x1601 has two entries");
    check(rpdo2.totalBits == 32 && rpdo2.lengthBytes() == 4, "0x1601 is four bytes");
    if (rpdo2.entries.size() == 2)
    {
        check(rpdo2.entries[0].index == 0x6411 && rpdo2.entries[0].sub == 1
                  && rpdo2.entries[0].bits == 16,
              "0x1601:01 is 0x6411:01 indicator brightness, 16 bits");
        check(rpdo2.entries[1].bitOffset == 16, "backlight brightness starts at bit 16");
    }

    check(canopen::communication_index_for_mapping(0x1A00) == 0x1800,
          "0x1A00 is configured by 0x1800");
    check(canopen::communication_index_for_mapping(0x1600) == 0x1400,
          "0x1600 is configured by 0x1400");
}

// ============================================================================
// Negative cases
// ============================================================================

void test_malformed_line_is_survivable()
{
    // One bad line used to kill the whole file. It should cost a diagnostic
    // with a line number and nothing else.
    const std::string_view eds = R"([MandatoryObjects]
SupportedObjects=1
1=0x1000

[1000]
ParameterName=Device Type
this line has no equals sign
ObjectType=0x7
DataType=0x0007
AccessType=ro
DefaultValue=0x00000191
)";

    auto result = canopen::parse_eds(eds);
    check(!result.ok(), "a malformed line is an error");
    check(result.diagnostics.size() == 1, "exactly one diagnostic");
    if (!result.diagnostics.empty())
    {
        check(result.diagnostics[0].line == 7, "the diagnostic points at line 7");
        check(result.diagnostics[0].column == 1, "and at column 1");
    }
    // The rest of the section still parsed.
    const canopen::SubObject* deviceType = result.od.get(0x1000, 0);
    check(deviceType != nullptr, "the object survives the bad line");
    check(deviceType != nullptr && deviceType->dataType == canopen::DataType::Unsigned32,
          "keys after the bad line are still read");
}

void test_unknown_access_type_is_an_error()
{
    const std::string_view eds = R"([1000]
ParameterName=Device Type
ObjectType=0x7
DataType=0x0007
AccessType=readonly
)";
    auto result = canopen::parse_eds(eds);
    check(!result.ok(), "an unrecognised AccessType is an error, not a silent RO default");
}

void test_signed_limits()
{
    const std::string_view eds = R"([2000]
ParameterName=Signed thing
ObjectType=0x7
DataType=0x0003
LowLimit=-32768
HighLimit=32767
AccessType=rw
DefaultValue=0
)";
    auto result = canopen::parse_eds(eds);
    const canopen::SubObject* entry = result.od.get(0x2000, 0);
    check(entry != nullptr, "the object parsed");
    check(entry != nullptr && entry->lowLimit == -32768, "a negative LowLimit is kept");
    check(entry != nullptr && entry->highLimit == 32767, "the HighLimit is kept");
}

void test_access_types()
{
    const std::string_view eds = R"([2000]
ParameterName=A
ObjectType=0x7
DataType=0x0005
AccessType=wo

[2001]
ParameterName=B
ObjectType=0x7
DataType=0x0005
AccessType=rwr
)";
    auto result = canopen::parse_eds(eds);
    check(result.od.get(0x2000, 0) != nullptr
              && result.od.get(0x2000, 0)->access == canopen::AccessType::WO,
          "'wo' is understood");
    check(result.od.get(0x2001, 0) != nullptr
              && result.od.get(0x2001, 0)->access == canopen::AccessType::RWR,
          "'rwr' is understood");
    check(!canopen::is_readable(canopen::AccessType::WO), "write-only is not readable");
    check(canopen::is_writable(canopen::AccessType::RWR), "rwr is writable");
}

void test_unknown_data_type_is_reported()
{
    const std::string_view eds = R"([2000]
ParameterName=A
ObjectType=0x7
DataType=0x00FE
AccessType=ro
)";
    auto result = canopen::parse_eds(eds);
    check(result.diagnostics.size() == 1 && result.diagnostics[0].severity
              == canopen::Severity::Warning,
          "an unknown DataType is a warning rather than an unchecked cast");
}

void test_subsection_before_parent()
{
    // A subN section ahead of its parent used to default-construct the parent
    // with index 0, quietly filing the whole object under the wrong index.
    const std::string_view eds = R"([1018sub1]
ParameterName=Vendor ID
ObjectType=0x7
DataType=0x0007
AccessType=ro
DefaultValue=0x307

[1018]
ParameterName=Identity Object
SubNumber=1
ObjectType=0x8
)";
    auto result = canopen::parse_eds(eds);
    const canopen::Object* identity = result.od.get(0x1018);
    check(identity != nullptr, "the object is filed under 0x1018");
    check(identity != nullptr && identity->index == 0x1018, "and carries its own index");
    check(result.od.get(0) == nullptr, "nothing was filed under index 0");
}

void test_validate_catches_sub_count()
{
    const std::string_view eds = R"([MandatoryObjects]
SupportedObjects=3
1=0x1000
2=0x1001
3=0x1018

[1000]
ParameterName=Device Type
ObjectType=0x7
DataType=0x0007
AccessType=ro
DefaultValue=0x191

[1001]
ParameterName=Error Register
ObjectType=0x7
DataType=0x0005
AccessType=ro
DefaultValue=0

[1018]
ParameterName=Identity Object
SubNumber=4
ObjectType=0x8

[1018sub0]
ParameterName=Number of entries
ObjectType=0x7
DataType=0x0005
AccessType=ro
DefaultValue=4

[1018sub1]
ParameterName=Vendor ID
ObjectType=0x7
DataType=0x0007
AccessType=ro
DefaultValue=0x307
)";
    auto result = canopen::parse_eds(eds);
    check(result.ok(), "the file parses");
    auto problems = canopen::validate(result.od);
    check(count_errors(problems) >= 2,
          "validate() reports both the wrong SubNumber and the wrong sub 0 count");
}

void test_validate_catches_undeclared_object()
{
    const std::string_view eds = R"([MandatoryObjects]
SupportedObjects=3
1=0x1000
2=0x1001
3=0x1018

[1000]
ParameterName=Device Type
ObjectType=0x7
DataType=0x0007
AccessType=ro
DefaultValue=0x191

[1001]
ParameterName=Error Register
ObjectType=0x7
DataType=0x0005
AccessType=ro
DefaultValue=0

[1018]
ParameterName=Identity Object
SubNumber=1
ObjectType=0x8

[1018sub0]
ParameterName=Number of entries
ObjectType=0x7
DataType=0x0005
AccessType=ro
DefaultValue=0

[2010]
ParameterName=Undeclared manufacturer object
ObjectType=0x7
DataType=0x0005
AccessType=rw
DefaultValue=0
)";
    auto result = canopen::parse_eds(eds);
    auto problems = canopen::validate(result.od);
    // CiA 306 has no inheritance: an object that is present but unlisted is a
    // defect in the file, because a tool reading the lists would never look
    // for it.
    check(count_errors(problems) == 1, "an unlisted object is reported exactly once");
}

void test_validate_catches_mapping_overflow()
{
    const std::string_view eds = R"([MandatoryObjects]
SupportedObjects=3
1=0x1000
2=0x1001
3=0x1018

[1000]
ParameterName=Device Type
ObjectType=0x7
DataType=0x0007
AccessType=ro
DefaultValue=0x191

[1001]
ParameterName=Error Register
ObjectType=0x7
DataType=0x0005
AccessType=ro
DefaultValue=0

[1018]
ParameterName=Identity Object
SubNumber=1
ObjectType=0x8

[1018sub0]
ParameterName=Number of entries
ObjectType=0x7
DataType=0x0005
AccessType=ro
DefaultValue=0

[OptionalObjects]
SupportedObjects=2
1=0x1A00
2=0x6000

[1A00]
ParameterName=TPDO1 mapping
SubNumber=3
ObjectType=0x8

[1A00sub0]
ParameterName=Number of entries
ObjectType=0x7
DataType=0x0005
AccessType=ro
DefaultValue=2

[1A00sub1]
ParameterName=Entry
ObjectType=0x7
DataType=0x0007
AccessType=ro
DefaultValue=0x60000140

[1A00sub2]
ParameterName=Entry
ObjectType=0x7
DataType=0x0007
AccessType=ro
DefaultValue=0x60000240

[6000]
ParameterName=Inputs
SubNumber=3
ObjectType=0x8

[6000sub0]
ParameterName=Number of entries
ObjectType=0x7
DataType=0x0005
AccessType=ro
DefaultValue=2

[6000sub1]
ParameterName=A
ObjectType=0x7
DataType=0x0015
AccessType=ro
PDOMapping=1

[6000sub2]
ParameterName=B
ObjectType=0x7
DataType=0x0015
AccessType=ro
PDOMapping=1
)";
    auto result = canopen::parse_eds(eds);
    check(result.ok(), "the overflowing file still parses");
    auto mapping = canopen::read_pdo_mapping(result.od, 0x1A00);
    check(mapping.totalBits == 128, "two 64-bit entries are 128 bits");
    auto problems = canopen::validate(result.od);
    check(count_errors(problems) >= 1, "validate() reports the 128-bit mapping");
}

void test_not_an_eds()
{
    auto result = canopen::parse_eds("\x01\x02\x03 not an ini file at all");
    check(!result.ok(), "binary input is rejected");
    check(!result.diagnostics.empty(), "and says why");
}

} // namespace

int main()
{
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%^%l%$] %v");

    test_real_eds();
    test_pdo_mappings();
    test_malformed_line_is_survivable();
    test_unknown_access_type_is_an_error();
    test_signed_limits();
    test_access_types();
    test_unknown_data_type_is_reported();
    test_subsection_before_parent();
    test_validate_catches_sub_count();
    test_validate_catches_undeclared_object();
    test_validate_catches_mapping_overflow();
    test_not_an_eds();

    if (failures != 0)
    {
        SPDLOG_ERROR("{} check(s) failed", failures);
        return 1;
    }

    SPDLOG_INFO("all EDS parser checks passed");
    return 0;
}
