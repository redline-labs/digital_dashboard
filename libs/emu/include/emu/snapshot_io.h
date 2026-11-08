// Snapshot helpers: a plain value goes out as its bytes and comes back the same way. Snapshots stay in
// memory, so the layout only has to be the same for save and load.
#pragma once

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

#include "emu/common.h"

namespace sim::sys {

template <class T>
void put_value(std::vector<uint8_t>& out, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

template <class T>
void get_value(const uint8_t*& p, T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    std::memcpy(&value, p, sizeof(T));
    p += sizeof(T);
}

}  // namespace sim::sys
