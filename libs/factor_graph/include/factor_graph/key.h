#pragma once

// A variable's name in the graph: a character naming its kind and an index,
// packed into one integer the way gtsam::Symbol does it, so a key reads as
// "x42" in a log instead of as 8646911284551352362.

#include <cstdint>
#include <string>

namespace factor_graph
{

using Key = std::uint64_t;

inline constexpr int kKeyIndexBits = 56;
inline constexpr std::uint64_t kKeyIndexMask = (std::uint64_t{1} << kKeyIndexBits) - 1;

constexpr Key symbol(char kind, std::uint64_t index)
{
    return (static_cast<std::uint64_t>(static_cast<unsigned char>(kind)) << kKeyIndexBits) | (index & kKeyIndexMask);
}

constexpr char symbolKind(Key key)
{
    return static_cast<char>(key >> kKeyIndexBits);
}

constexpr std::uint64_t symbolIndex(Key key)
{
    return key & kKeyIndexMask;
}

inline std::string keyName(Key key)
{
    return std::string(1, symbolKind(key)) + std::to_string(symbolIndex(key));
}

}  // namespace factor_graph
