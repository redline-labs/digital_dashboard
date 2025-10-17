#include <spdlog/spdlog.h>
#include "canopen/eds_parser.h"

int main()
{
    // Hardcoded minimal EDS sample for testing
    const std::string_view eds_sample = R"(
[FileInfo]
FileName=test_device.eds
Description=Test CANopen Device
CreatedBy=Test Suite

[DeviceInfo]
Vendorname=Test Vendor
VendorNumber=0x1234
ProductName=Test Product
ProductNumber=0x5678
RevisionNumber=0x0001
NrOfRXPDO=2
NrOfTXPDO=1

[Comments]
Lines=1
Line1=This is a test EDS file for parser validation

[MandatoryObjects]
SupportedObjects=1
1=0x1018

[1018]
ParameterName=Identity Object
SubNumber=5
ObjectType=0x8

[1018sub0]
ParameterName=Number of entries
ObjectType=0x7
DataType=0x0005
LowLimit=1
HighLimit=4
AccessType=ro
DefaultValue=4
PDOMapping=0
ObjFlags=0x0

[1018sub1]
ParameterName=Vendor ID
ObjectType=0x7
DataType=0x0007
LowLimit=
HighLimit=
AccessType=ro
DefaultValue=0x00000307
PDOMapping=0
ObjFlags=0x0

[1018sub2]
ParameterName=Product code
ObjectType=0x7
DataType=0x0007
LowLimit=
HighLimit=
AccessType=ro
DefaultValue=0x0000334B
PDOMapping=0
ObjFlags=0x0

[1018sub3]
ParameterName=Revision number
ObjectType=0x7
DataType=0x0007
LowLimit=
HighLimit=
AccessType=ro
DefaultValue=1
PDOMapping=0
ObjFlags=0x0

[1018sub4]
ParameterName=Serial number
ObjectType=0x7
DataType=0x0007
LowLimit=
HighLimit=
AccessType=ro
DefaultValue=
PDOMapping=0
ObjFlags=0x0

[OptionalObjects]
SupportedObjects=1
1=0x1008

[1008]
ParameterName=Manufacturer device name
ObjectType=0x7
DataType=0x0009
LowLimit=
HighLimit=
AccessType=const
DefaultValue=manufacturer
PDOMapping=0
ObjFlags=0x0
)";

    auto od = canopen::parse_eds(eds_sample);

    if (!od)
    {
        SPDLOG_ERROR("Failed to parse EDS.");
        return 1;
    }

    // Debug: print what objects were parsed
    SPDLOG_INFO("Parsed {} objects", od->objects.size());
    for (const auto& [idx, obj] : od->objects)
    {
        SPDLOG_INFO("  Object {}: {}", idx, obj.parameterName);
    }

    // Validate parsed data
    if (od->deviceInfo.vendorName != "Test Vendor")
    {
        SPDLOG_ERROR("Vendor name mismatch: {}", od->deviceInfo.vendorName);
        return 2;
    }

    if (od->deviceInfo.nrOfRxPdo != 2 || od->deviceInfo.nrOfTxPdo != 1)
    {
        SPDLOG_ERROR("PDO counts mismatch: RX={}, TX={}", od->deviceInfo.nrOfRxPdo, od->deviceInfo.nrOfTxPdo);
        return 3;
    }

    // Check required objects exist (EDS sections are decimal: [1000] = index 1000, not 0x1000)
    if (!od->get(1018))
    {
        SPDLOG_ERROR("Mandatory objects missing");
        return 4;
    }

    if (!od->get(1008))
    {
        SPDLOG_ERROR("Digital input object structure incomplete");
        return 5;
    }

    SPDLOG_INFO("EDS parse test PASSED: parsed {} objects successfully", od->objects.size());
    return 0;
}
