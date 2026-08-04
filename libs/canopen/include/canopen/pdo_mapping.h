// SPDX-License-Identifier: GPL-3.0-or-later
//
// What a PDO actually carries, read out of the object dictionary rather than
// assumed.
//
// A mapping object (0x1600.., 0x1A00..) is an array of UNSIGNED32 words, each
// naming an object dictionary entry and how many of its bits go into the frame:
//
//     31            16 15      8 7      0
//     +---------------+---------+--------+
//     |     index     |   sub   |  bits  |
//     +---------------+---------+--------+
//
// Entries pack into the frame in sub-index order, least significant bit of the
// first entry at bit 0 of byte 0. That is the whole layout rule, and it is why
// hardcoding "buttons are bytes 0..2" is a guess that happens to be right:
// 0x1A00 says 0x6000:01/02/03 at 8 bits each, and byte offsets follow.
#ifndef CANOPEN_PDO_MAPPING_H
#define CANOPEN_PDO_MAPPING_H

#include "canopen/eds_ast.h"

#include <cstdint>
#include <string>
#include <vector>

namespace canopen
{

struct PdoMappingEntry
{
    uint16_t index { 0 };
    uint8_t sub { 0 };
    // Width taken from the mapping word, not from the target's DataType. The
    // two should agree; read_pdo_mapping() reports it when they do not.
    uint8_t bits { 0 };
    // Where this entry starts in the frame, in bits from the start of byte 0.
    uint8_t bitOffset { 0 };
    // The name of the object it points at, when the file declares one. Useful
    // for generated accessors and for logs.
    std::string parameterName;
};

struct PdoMapping
{
    std::vector<PdoMappingEntry> entries;
    uint16_t totalBits { 0 };
    // Anything wrong with the mapping, phrased for a diagnostic. Kept here
    // rather than returned separately so a caller that only wants the layout
    // can ignore it.
    std::vector<std::string> problems;

    // Whole bytes, rounded up. Mappings that are not byte-aligned are legal but
    // do not occur on this device.
    uint8_t lengthBytes() const { return static_cast<uint8_t>((totalBits + 7) / 8); }
};

bool is_rpdo_mapping_index(uint16_t index);
bool is_tpdo_mapping_index(uint16_t index);
bool is_pdo_mapping_index(uint16_t index);

// The communication parameter object paired with a mapping object: 0x1600 is
// configured by 0x1400, 0x1A00 by 0x1800.
uint16_t communication_index_for_mapping(uint16_t mappingIndex);

// Decode a mapping object. An object that is not a mapping object, or is
// absent, yields an empty mapping with a problem recorded.
PdoMapping read_pdo_mapping(const ObjectDictionary& od, uint16_t mappingIndex);

} // namespace canopen

#endif // CANOPEN_PDO_MAPPING_H
