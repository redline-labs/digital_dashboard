// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/tlv8.ts
#include "airplay/tlv8.h"

#include <algorithm>

namespace airplay::tlv8
{

std::vector<uint8_t> encode(const std::vector<Item>& items)
{
    std::vector<uint8_t> out;
    for (const auto& [type, value] : items)
    {
        if (value.empty())
        {
            out.push_back(type);
            out.push_back(0);
            continue;
        }

        size_t offset = 0;
        while (offset < value.size())
        {
            const size_t fragment = std::min<size_t>(255, value.size() - offset);
            out.push_back(type);
            out.push_back(static_cast<uint8_t>(fragment));
            out.insert(out.end(), value.begin() + offset, value.begin() + offset + fragment);
            offset += fragment;
        }
    }
    return out;
}

std::vector<Item> decode(const std::vector<uint8_t>& data)
{
    std::vector<Item> items;
    size_t offset = 0;
    while (offset + 2 <= data.size())
    {
        const uint8_t type = data[offset];
        const uint8_t length = data[offset + 1];
        offset += 2;
        if (offset + length > data.size())
        {
            break;  // Truncated input; drop the partial item.
        }

        // A 255-byte fragment followed by the same type is a continuation.
        if (!items.empty() && items.back().first == type && items.back().second.size() % 255 == 0 &&
            !items.back().second.empty())
        {
            items.back().second.insert(items.back().second.end(),
                                       data.begin() + offset, data.begin() + offset + length);
        }
        else
        {
            items.emplace_back(type, std::vector<uint8_t>(data.begin() + offset, data.begin() + offset + length));
        }
        offset += length;
    }
    return items;
}

const std::vector<uint8_t>* find(const std::vector<Item>& items, uint8_t type)
{
    for (const auto& item : items)
    {
        if (item.first == type)
        {
            return &item.second;
        }
    }
    return nullptr;
}

}  // namespace airplay::tlv8
