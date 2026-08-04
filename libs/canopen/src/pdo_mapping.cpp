// SPDX-License-Identifier: GPL-3.0-or-later

#include "canopen/pdo_mapping.h"

#include <spdlog/fmt/fmt.h>

namespace canopen
{

bool is_rpdo_mapping_index(uint16_t index)
{
    return index >= 0x1600 && index <= 0x17FF;
}

bool is_tpdo_mapping_index(uint16_t index)
{
    return index >= 0x1A00 && index <= 0x1BFF;
}

bool is_pdo_mapping_index(uint16_t index)
{
    return is_rpdo_mapping_index(index) || is_tpdo_mapping_index(index);
}

uint16_t communication_index_for_mapping(uint16_t mappingIndex)
{
    if (is_rpdo_mapping_index(mappingIndex))
    {
        return static_cast<uint16_t>(mappingIndex - 0x0200);
    }
    if (is_tpdo_mapping_index(mappingIndex))
    {
        return static_cast<uint16_t>(mappingIndex - 0x0200);
    }
    return 0;
}

PdoMapping read_pdo_mapping(const ObjectDictionary& od, uint16_t mappingIndex)
{
    PdoMapping mapping;

    if (!is_pdo_mapping_index(mappingIndex))
    {
        mapping.problems.push_back(
            fmt::format("0x{:04X} is not a PDO mapping index", mappingIndex));
        return mapping;
    }

    const Object* object = od.get(mappingIndex);
    if (object == nullptr)
    {
        mapping.problems.push_back(fmt::format("[{:04X}] is missing", mappingIndex));
        return mapping;
    }

    // sub 0 says how many of the following entries are active. An entry beyond
    // that count is still declared in the file but is not in the frame, so the
    // count -- not the section count -- decides the layout.
    size_t active = object->subs.size() > 0 ? object->subs.size() - 1 : 0;
    auto sub0 = object->subs.find(0);
    if (sub0 != object->subs.end() && sub0->second.defaultValue.has_value())
    {
        if (const auto* declared = std::get_if<uint64_t>(&*sub0->second.defaultValue))
        {
            active = static_cast<size_t>(*declared);
        }
    }

    uint8_t bitOffset = 0;
    for (size_t position = 1; position <= active; ++position)
    {
        const uint8_t sub = static_cast<uint8_t>(position);
        auto it = object->subs.find(sub);
        if (it == object->subs.end())
        {
            mapping.problems.push_back(fmt::format(
                "[{:04X}sub0] claims {} entries but sub {} is missing", mappingIndex, active, sub));
            break;
        }

        const SubObject& entry = it->second;
        if (!entry.defaultValue.has_value())
        {
            mapping.problems.push_back(
                fmt::format("[{:04X}sub{:X}] has no DefaultValue to map", mappingIndex, sub));
            continue;
        }

        const auto* word = std::get_if<uint64_t>(&*entry.defaultValue);
        if (word == nullptr)
        {
            mapping.problems.push_back(fmt::format(
                "[{:04X}sub{:X}] mapping value is not a number", mappingIndex, sub));
            continue;
        }

        PdoMappingEntry decoded;
        decoded.index = static_cast<uint16_t>((*word >> 16) & 0xFFFF);
        decoded.sub = static_cast<uint8_t>((*word >> 8) & 0xFF);
        decoded.bits = static_cast<uint8_t>(*word & 0xFF);
        decoded.bitOffset = bitOffset;

        // A zero-length entry is the CiA way of reserving space with a dummy
        // type; a zero index with a non-zero length is a malformed word.
        if (decoded.index != 0)
        {
            const SubObject* target = od.get(decoded.index, decoded.sub);
            if (target == nullptr)
            {
                mapping.problems.push_back(
                    fmt::format("[{:04X}sub{:X}] maps 0x{:04X}:{:02X}, which the file does not "
                                "declare",
                                mappingIndex, sub, decoded.index, decoded.sub));
            }
            else
            {
                decoded.parameterName = target->parameterName;
                if (!target->pdoMappable)
                {
                    mapping.problems.push_back(fmt::format(
                        "[{:04X}sub{:X}] maps 0x{:04X}:{:02X} '{}', which is marked PDOMapping=0",
                        mappingIndex, sub, decoded.index, decoded.sub, target->parameterName));
                }
                if (auto width = data_type_bits(target->dataType))
                {
                    if (*width != decoded.bits)
                    {
                        mapping.problems.push_back(fmt::format(
                            "[{:04X}sub{:X}] maps {} bits of 0x{:04X}:{:02X}, which is {} ({} "
                            "bits)",
                            mappingIndex, sub, decoded.bits, decoded.index, decoded.sub,
                            to_string(target->dataType), *width));
                    }
                }
            }
        }

        bitOffset = static_cast<uint8_t>(bitOffset + decoded.bits);
        mapping.totalBits = static_cast<uint16_t>(mapping.totalBits + decoded.bits);
        mapping.entries.push_back(std::move(decoded));
    }

    return mapping;
}

} // namespace canopen
