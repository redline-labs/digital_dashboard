// SPDX-License-Identifier: GPL-3.0-or-later
//
// The 16-bit MTData2 data identifier, and the three fields packed into it.
//
//     bit  15 14 13 12 11 10 | 9 8 |  7  6  5  4 |  3  2  1  0
//          group             | rsvd| type        | format
//
// Group says what category the value belongs to (timestamps, acceleration,
// magnetic field); group and type together say which value it is; format says
// how the bytes are laid out. An identifier is therefore NOT a constant to
// compare against directly -- 0x4020 and 0x4022 are both acceleration, one in
// single-precision float and one in fixed point 16.32, and a parser that
// compared the whole 16 bits would decode the first and drop the second with
// no error anywhere.
//
// The masks are transcribed from the SDK's xstypes/xsdataidentifier.h rather
// than from the LLCP's prose, so two independent renderings of the protocol
// have to agree. The two the LLCP spells out (Table 18) match.

#ifndef XBUS_DATA_ID_H
#define XBUS_DATA_ID_H

#include <cstddef>
#include <cstdint>

#include "xbus/data_table.h"

namespace xbus
{

// XDI_TypeMask: the group alone.
inline constexpr std::uint16_t kGroupMask = 0xFE00;
// XDI_FullTypeMask: group and type, with the format nibble cleared. This is
// what the enumerators in data_table.h carry and what a parser matches on.
inline constexpr std::uint16_t kFullTypeMask = 0xFFF0;
// XDI_DataFormatMask.
inline constexpr std::uint16_t kFormatMask = 0x000F;
// XDI_SubFormatMask: the precision, within the format nibble.
inline constexpr std::uint16_t kPrecisionMask = 0x0003;
// XDI_CoordSysMask: the coordinate system, within the format nibble. Only the
// orientation and velocity groups define it, neither of which an MTi-610
// produces -- it is here so that a nonzero value can be REFUSED rather than
// silently ignored.
inline constexpr std::uint16_t kCoordinateSystemMask = 0x000C;

enum class DataId : std::uint16_t
{
#define XBUS_DATA_ENUM(id, Name, snake, elements) Name = id,
    XBUS_DATA_TABLE(XBUS_DATA_ENUM)
#undef XBUS_DATA_ENUM
};

// The groups an MTi-610 uses. Named so a log line about an unmodelled
// identifier can still say which category it belonged to, which is usually
// enough to recognise a device that is not a 610.
enum class DataGroup : std::uint16_t
{
    Temperature = 0x0800,
    Timestamp = 0x1000,
    Orientation = 0x2000,
    Pressure = 0x3000,
    Acceleration = 0x4000,
    Position = 0x5000,
    Gnss = 0x7000,
    AngularVelocity = 0x8000,
    RawSensor = 0xA000,
    Magnetic = 0xC000,
    Velocity = 0xD000,
    Status = 0xE000,
};

constexpr const char* to_string(DataGroup group)
{
    switch (group)
    {
        case DataGroup::Temperature:     return "temperature";
        case DataGroup::Timestamp:       return "timestamp";
        case DataGroup::Orientation:     return "orientation";
        case DataGroup::Pressure:        return "pressure";
        case DataGroup::Acceleration:    return "acceleration";
        case DataGroup::Position:        return "position";
        case DataGroup::Gnss:            return "gnss";
        case DataGroup::AngularVelocity: return "angular velocity";
        case DataGroup::RawSensor:       return "raw sensor";
        case DataGroup::Magnetic:        return "magnetic";
        case DataGroup::Velocity:        return "velocity";
        case DataGroup::Status:          return "status";
    }

    // Not a default: -- a group added to the enum must fail to compile here. A
    // device may send a group this build has never heard of, and that is what
    // lands here.
    return "unknown group";
}

// Table 18's precision field. The numeric values ARE the wire values.
enum class Precision : std::uint8_t
{
    Float32 = 0x0,
    Fp1220 = 0x1,
    Fp1632 = 0x2,
    Float64 = 0x3,
};

constexpr const char* to_string(Precision precision)
{
    switch (precision)
    {
        case Precision::Float32: return "float32";
        case Precision::Fp1220:  return "fp12.20";
        case Precision::Fp1632:  return "fp16.32";
        case Precision::Float64: return "float64";
    }

    return "unknown precision";
}

// Bytes one real-valued component occupies. Note that fp16.32 is SIX, not
// eight: only the low six bytes of the scaled 64-bit integer are transmitted.
constexpr std::size_t precision_size(Precision precision)
{
    switch (precision)
    {
        case Precision::Float32: return 4;
        case Precision::Fp1220:  return 4;
        case Precision::Fp1632:  return 6;
        case Precision::Float64: return 8;
    }

    return 0;
}

constexpr DataGroup data_group(std::uint16_t rawId)
{
    return static_cast<DataGroup>(rawId & kGroupMask);
}

constexpr std::uint16_t data_type(std::uint16_t rawId)
{
    return static_cast<std::uint16_t>(rawId & kFullTypeMask);
}

constexpr Precision data_precision(std::uint16_t rawId)
{
    return static_cast<Precision>(rawId & kPrecisionMask);
}

constexpr std::uint8_t data_coordinate_system(std::uint16_t rawId)
{
    return static_cast<std::uint8_t>(rawId & kCoordinateSystemMask);
}

constexpr const char* data_name(DataId id)
{
    switch (id)
    {
#define XBUS_DATA_NAME(rawId, Name, snake, elements) case DataId::Name: return snake;
        XBUS_DATA_TABLE(XBUS_DATA_NAME)
#undef XBUS_DATA_NAME
    }

    // Not a default: -- adding a row without a name must fail to compile. An
    // identifier outside the table is what lands here, which for a device that
    // is not a 610 is routine.
    return "unknown";
}

// How many real-valued components the payload holds, or 0 for the identifiers
// whose payload has a fixed binary layout.
constexpr std::size_t data_elements(DataId id)
{
    switch (id)
    {
#define XBUS_DATA_ELEMENTS(rawId, Name, snake, elements) case DataId::Name: return elements;
        XBUS_DATA_TABLE(XBUS_DATA_ELEMENTS)
#undef XBUS_DATA_ELEMENTS
    }

    return 0;
}

constexpr bool is_known_data_id(std::uint16_t rawId)
{
    switch (data_type(rawId))
    {
#define XBUS_DATA_KNOWN(id, Name, snake, elements) case id: return true;
        XBUS_DATA_TABLE(XBUS_DATA_KNOWN)
#undef XBUS_DATA_KNOWN
        default: break;
    }

    return false;
}

} // namespace xbus

#endif // XBUS_DATA_ID_H
