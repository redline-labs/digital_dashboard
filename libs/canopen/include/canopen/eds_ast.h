// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shape of a parsed EDS: the object dictionary a device declares, plus the
// metadata around it that says what the device can do (LSS, which bit rates,
// which of the declared objects are mandatory).
//
// Two things here are easy to get wrong and are worth stating once:
//
//   * Section names are HEXADECIMAL. `[1018]` is index 0x1018, `[1A00sub3]` is
//     index 0x1A00 sub 3. Values inside a section are not -- `DefaultValue=255`
//     is decimal, `DefaultValue=0xFF` is hex. Two different radix rules in one
//     file, and mixing them up silently produces an object dictionary in which
//     nothing is where you asked for it.
//
//   * CiA 306 has no inheritance. An EDS that omits an object is asserting the
//     device does not implement it, however standard that object is. There is
//     no DS301/DS401 baseline to fall back on, so a missing entry is a defect
//     in the file rather than shorthand to be filled in by the reader.
#ifndef CANOPEN_EDS_AST_H
#define CANOPEN_EDS_AST_H

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace canopen
{

// CiA 301 data type indices. The structured types at the end (0x20..0x23) name
// the shape of a record rather than a scalar, so they have no bit width.
enum class DataType : uint16_t
{
    Boolean = 0x0001,
    Integer8 = 0x0002,
    Integer16 = 0x0003,
    Integer32 = 0x0004,
    Unsigned8 = 0x0005,
    Unsigned16 = 0x0006,
    Unsigned32 = 0x0007,
    Real32 = 0x0008,
    VisibleString = 0x0009,
    OctetString = 0x000A,
    UnicodeString = 0x000B,
    TimeOfDay = 0x000C,
    TimeDifference = 0x000D,
    Domain = 0x000F,
    Integer24 = 0x0010,
    Real64 = 0x0011,
    Integer40 = 0x0012,
    Integer48 = 0x0013,
    Integer56 = 0x0014,
    Integer64 = 0x0015,
    Unsigned24 = 0x0016,
    Unsigned40 = 0x0018,
    Unsigned48 = 0x0019,
    Unsigned56 = 0x001A,
    Unsigned64 = 0x001B,
    PdoCommParameter = 0x0020,
    PdoMapping = 0x0021,
    SdoParameter = 0x0022,
    Identity = 0x0023,
};

// True when the value is one of the enumerators above. An EDS is free to name a
// type we have never heard of; that is a diagnostic, not a cast.
bool is_known_data_type(uint16_t raw);

// Width in bits, for the fixed-width types. Strings and Domain are
// variable-length and return nullopt, as do the structured types.
std::optional<uint8_t> data_type_bits(DataType type);

const char* to_string(DataType type);

// DS306 defines exactly these six. `rwr` and `rww` mean read-write where the
// write is only meaningful on, respectively, an RPDO and a TPDO.
enum class AccessType
{
    RO,
    WO,
    RW,
    RWR,
    RWW,
    CONST,
};

// Whether an SDO client may write the object at all. `const` is read-only by
// definition and `ro` is read-only by declaration; the rest accept a download.
bool is_writable(AccessType access);
bool is_readable(AccessType access);

const char* to_string(AccessType access);

// EDS ObjectType codes (CiA 306 table 8). VAR carries its value in the object
// section itself; ARRAY and RECORD carry theirs in subN sections.
enum class ObjectCode : uint8_t
{
    Null = 0x00,
    Domain = 0x02,
    DefType = 0x05,
    DefStruct = 0x06,
    Var = 0x07,
    Array = 0x08,
    Record = 0x09,
};

// `$NODEID+0x40000180` and friends. The COB-ID defaults are all written this
// way, so a parser that cannot represent the expression cannot derive a single
// COB-ID from the file -- it can only fall back to the profile constants and
// look like it succeeded.
struct NodeIdExpr
{
    bool usesNodeId { false };
    int64_t constant { 0 };
};

using Value = std::variant<std::monostate, uint64_t, int64_t, std::string, NodeIdExpr>;

// One entry. Both a subN section and a VAR object section produce this: they
// carry the same keys, and the only difference is where they live.
struct SubObject
{
    uint8_t subIndex { 0 };
    std::string parameterName;
    ObjectCode objectCode { ObjectCode::Var };
    DataType dataType { DataType::Unsigned8 };
    std::optional<int64_t> lowLimit;
    std::optional<int64_t> highLimit;
    AccessType access { AccessType::RO };
    std::optional<Value> defaultValue;
    bool pdoMappable { false };
    uint32_t objFlags { 0 };
};

struct Object
{
    uint16_t index { 0 };
    std::string parameterName;
    ObjectCode objectCode { ObjectCode::Var };
    // The file's own claim about how many subs follow, kept separately from
    // `subs` so validate() can compare the two.
    std::optional<uint8_t> declaredSubNumber;
    uint32_t objFlags { 0 };
    // A VAR object's own value lives at sub 0 here, so `get(index, 0)` answers
    // for VAR and ARRAY alike and callers need not care which they have.
    std::map<uint8_t, SubObject> subs;

    bool isVar() const { return objectCode == ObjectCode::Var; }
};

struct FileInfo
{
    std::string fileName;
    std::string description;
    std::string createdBy;
    std::string modifiedBy;
    uint32_t fileVersion { 0 };
    uint32_t fileRevision { 0 };
    std::string edsVersion;
};

struct DeviceInfo
{
    std::string vendorName;
    uint32_t vendorNumber { 0 };
    std::string productName;
    uint32_t productNumber { 0 };
    uint32_t revisionNumber { 0 };
    std::string orderCode;
    uint8_t nrOfRxPdo { 0 };
    uint8_t nrOfTxPdo { 0 };

    // Bears directly on reconfiguration: LSS is how node ID and bit rate are
    // changed, and the bit rate table says which values may be asked for.
    bool lssSupported { false };
    // kbit/s -> supported. The keys are the eight CiA 301 rates; the Grayhill
    // keypad declares 10 kbit/s unsupported, matching its manual.
    std::map<uint32_t, bool> supportedBitrates;

    bool simpleBootUpMaster { false };
    bool simpleBootUpSlave { false };
    bool dynamicChannelsSupported { false };
    bool groupMessaging { false };
    uint8_t granularity { 0 };
    uint32_t compactPdo { 0 };
};

struct ObjectDictionary
{
    FileInfo fileInfo;
    DeviceInfo deviceInfo;
    std::map<uint16_t, Object> objects;

    // What the file declares it contains, as distinct from what it actually
    // contains. validate() compares the two in both directions.
    std::vector<uint16_t> mandatoryObjects;
    std::vector<uint16_t> optionalObjects;
    std::vector<uint16_t> manufacturerObjects;

    // Dummy data type index -> usable as a PDO mapping placeholder.
    std::map<uint16_t, bool> dummyUsage;
    std::vector<std::string> comments;

    const Object* get(uint16_t index) const;
    const SubObject* get(uint16_t index, uint8_t sub) const;

    // The declared default of an entry, resolved against a node ID so a
    // `$NODEID+...` expression yields a number. nullopt when the entry is
    // absent, has no default, or has one that is not numeric.
    std::optional<uint64_t> defaultValue(uint16_t index, uint8_t sub, uint8_t nodeId) const;
};

inline int64_t resolve_nodeid_expr(const NodeIdExpr& expr, uint8_t nodeId)
{
    return (expr.usesNodeId ? static_cast<int64_t>(nodeId) : 0) + expr.constant;
}

} // namespace canopen

#endif // CANOPEN_EDS_AST_H
