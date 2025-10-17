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

enum class DataType : uint16_t
{
    Boolean = 0x0001,
    Integer8 = 0x0002,
    Integer16 = 0x0003,
    Integer32 = 0x0004,
    Unsigned8 = 0x0005,
    Unsigned16 = 0x0006,
    Unsigned32 = 0x0007,
    VisibleString = 0x0009,
    OctetString = 0x000A,
    Domain = 0x000F,
};

enum class AccessType
{
    RO,
    RW,
    RWW,
    CONST,
};

struct NodeIdExpr
{
    // Either a plain value, or "$NODEID + const" (or minus); supports hex/dec input
    bool usesNodeId { false };
    int64_t constant { 0 };
};

using Value = std::variant<std::monostate, uint64_t, int64_t, std::string, NodeIdExpr>;

struct SubObject
{
    uint8_t subIndex { 0 };
    std::string parameterName;
    DataType dataType { DataType::Unsigned8 };
    std::optional<int64_t> lowLimit;
    std::optional<int64_t> highLimit;
    AccessType access { AccessType::RO };
    std::optional<Value> defaultValue; // may be NodeIdExpr
    bool pdoMappable { false };
};

struct Object
{
    uint16_t index { 0 };
    std::string parameterName;
    uint8_t objectType { 0 }; // raw EDS ObjectType
    std::map<uint8_t, SubObject> subs; // includes sub0 (# entries) when present
};

struct FileInfo
{
    std::string fileName;
    std::string description;
    std::string createdBy;
};

struct DeviceInfo
{
    std::string vendorName;
    uint32_t vendorNumber { 0 };
    std::string productName;
    uint32_t productNumber { 0 };
    uint32_t revisionNumber { 0 };
    uint8_t nrOfRxPdo { 0 };
    uint8_t nrOfTxPdo { 0 };
};

struct ObjectDictionary
{
    FileInfo fileInfo;
    DeviceInfo deviceInfo;
    std::map<uint16_t, Object> objects;

    const Object* get(uint16_t index) const;
    const SubObject* get(uint16_t index, uint8_t sub) const;
};

// Helpers
inline uint32_t resolve_nodeid_expr(const NodeIdExpr& expr, uint8_t nodeId)
{
    return static_cast<uint32_t>((expr.usesNodeId ? nodeId : 0) + expr.constant);
}

} // namespace canopen

#endif // CANOPEN_EDS_AST_H


