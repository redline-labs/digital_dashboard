// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from LIVI src/main/services/projection/driver/cp/stack/tlv8.ts
#ifndef AIRPLAY_TLV8_H_
#define AIRPLAY_TLV8_H_

#include <cstdint>
#include <utility>
#include <vector>

namespace airplay::tlv8
{

using Item = std::pair<uint8_t, std::vector<uint8_t>>;

// HAP TLV8: values longer than 255 bytes are split into consecutive
// fragments with the same type; decode() merges them back together.
std::vector<uint8_t> encode(const std::vector<Item>& items);
std::vector<Item> decode(const std::vector<uint8_t>& data);

// First value for a given type, if present.
const std::vector<uint8_t>* find(const std::vector<Item>& items, uint8_t type);

}  // namespace airplay::tlv8

#endif  // AIRPLAY_TLV8_H_
