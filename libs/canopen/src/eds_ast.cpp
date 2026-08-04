// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/eds_ast.h"

namespace canopen
{

bool is_known_data_type(uint16_t raw)
{
    switch (static_cast<DataType>(raw))
    {
    case DataType::Boolean:
    case DataType::Integer8:
    case DataType::Integer16:
    case DataType::Integer32:
    case DataType::Unsigned8:
    case DataType::Unsigned16:
    case DataType::Unsigned32:
    case DataType::Real32:
    case DataType::VisibleString:
    case DataType::OctetString:
    case DataType::UnicodeString:
    case DataType::TimeOfDay:
    case DataType::TimeDifference:
    case DataType::Domain:
    case DataType::Integer24:
    case DataType::Real64:
    case DataType::Integer40:
    case DataType::Integer48:
    case DataType::Integer56:
    case DataType::Integer64:
    case DataType::Unsigned24:
    case DataType::Unsigned40:
    case DataType::Unsigned48:
    case DataType::Unsigned56:
    case DataType::Unsigned64:
    case DataType::PdoCommParameter:
    case DataType::PdoMapping:
    case DataType::SdoParameter:
    case DataType::Identity:
        return true;
    }
    return false;
}

std::optional<uint8_t> data_type_bits(DataType type)
{
    switch (type)
    {
    case DataType::Boolean:
        return 1;
    case DataType::Integer8:
    case DataType::Unsigned8:
        return 8;
    case DataType::Integer16:
    case DataType::Unsigned16:
        return 16;
    case DataType::Integer24:
    case DataType::Unsigned24:
        return 24;
    case DataType::Integer32:
    case DataType::Unsigned32:
    case DataType::Real32:
        return 32;
    case DataType::Integer40:
    case DataType::Unsigned40:
        return 40;
    case DataType::Integer48:
    case DataType::Unsigned48:
    case DataType::TimeOfDay:
    case DataType::TimeDifference:
        return 48;
    case DataType::Integer56:
    case DataType::Unsigned56:
        return 56;
    case DataType::Integer64:
    case DataType::Unsigned64:
    case DataType::Real64:
        return 64;

    // Variable-length, or a description of a record rather than a value.
    case DataType::VisibleString:
    case DataType::OctetString:
    case DataType::UnicodeString:
    case DataType::Domain:
    case DataType::PdoCommParameter:
    case DataType::PdoMapping:
    case DataType::SdoParameter:
    case DataType::Identity:
        return std::nullopt;
    }
    return std::nullopt;
}

const char* to_string(DataType type)
{
    switch (type)
    {
    case DataType::Boolean: return "BOOLEAN";
    case DataType::Integer8: return "INTEGER8";
    case DataType::Integer16: return "INTEGER16";
    case DataType::Integer24: return "INTEGER24";
    case DataType::Integer32: return "INTEGER32";
    case DataType::Integer40: return "INTEGER40";
    case DataType::Integer48: return "INTEGER48";
    case DataType::Integer56: return "INTEGER56";
    case DataType::Integer64: return "INTEGER64";
    case DataType::Unsigned8: return "UNSIGNED8";
    case DataType::Unsigned16: return "UNSIGNED16";
    case DataType::Unsigned24: return "UNSIGNED24";
    case DataType::Unsigned32: return "UNSIGNED32";
    case DataType::Unsigned40: return "UNSIGNED40";
    case DataType::Unsigned48: return "UNSIGNED48";
    case DataType::Unsigned56: return "UNSIGNED56";
    case DataType::Unsigned64: return "UNSIGNED64";
    case DataType::Real32: return "REAL32";
    case DataType::Real64: return "REAL64";
    case DataType::VisibleString: return "VISIBLE_STRING";
    case DataType::OctetString: return "OCTET_STRING";
    case DataType::UnicodeString: return "UNICODE_STRING";
    case DataType::TimeOfDay: return "TIME_OF_DAY";
    case DataType::TimeDifference: return "TIME_DIFFERENCE";
    case DataType::Domain: return "DOMAIN";
    case DataType::PdoCommParameter: return "PDO_COMM_PARAMETER";
    case DataType::PdoMapping: return "PDO_MAPPING";
    case DataType::SdoParameter: return "SDO_PARAMETER";
    case DataType::Identity: return "IDENTITY";
    }
    return "UNKNOWN";
}

bool is_writable(AccessType access)
{
    switch (access)
    {
    case AccessType::WO:
    case AccessType::RW:
    case AccessType::RWR:
    case AccessType::RWW:
        return true;
    case AccessType::RO:
    case AccessType::CONST:
        return false;
    }
    return false;
}

bool is_readable(AccessType access)
{
    switch (access)
    {
    case AccessType::RO:
    case AccessType::RW:
    case AccessType::RWR:
    case AccessType::RWW:
    case AccessType::CONST:
        return true;
    case AccessType::WO:
        return false;
    }
    return false;
}

const char* to_string(AccessType access)
{
    switch (access)
    {
    case AccessType::RO: return "ro";
    case AccessType::WO: return "wo";
    case AccessType::RW: return "rw";
    case AccessType::RWR: return "rwr";
    case AccessType::RWW: return "rww";
    case AccessType::CONST: return "const";
    }
    return "?";
}

const Object* ObjectDictionary::get(uint16_t index) const
{
    auto it = objects.find(index);
    return it == objects.end() ? nullptr : &it->second;
}

const SubObject* ObjectDictionary::get(uint16_t index, uint8_t sub) const
{
    const Object* object = get(index);
    if (object == nullptr)
    {
        return nullptr;
    }

    auto it = object->subs.find(sub);
    return it == object->subs.end() ? nullptr : &it->second;
}

std::optional<uint64_t> ObjectDictionary::defaultValue(uint16_t index, uint8_t sub,
                                                      uint8_t nodeId) const
{
    const SubObject* entry = get(index, sub);
    if (entry == nullptr || !entry->defaultValue.has_value())
    {
        return std::nullopt;
    }

    const Value& value = *entry->defaultValue;
    if (const auto* number = std::get_if<uint64_t>(&value))
    {
        return *number;
    }
    if (const auto* signedNumber = std::get_if<int64_t>(&value))
    {
        return static_cast<uint64_t>(*signedNumber);
    }
    if (const auto* expr = std::get_if<NodeIdExpr>(&value))
    {
        return static_cast<uint64_t>(resolve_nodeid_expr(*expr, nodeId));
    }

    return std::nullopt;
}

} // namespace canopen
