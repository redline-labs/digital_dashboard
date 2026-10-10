#ifndef PUB_SUB_DETAIL_LAYOUT_ATTACHMENT_H_
#define PUB_SUB_DETAIL_LAYOUT_ATTACHMENT_H_

// Private to pub_sub: the schema revision as a zenoh attachment, written by
// every publisher, service and client and read by every subscriber and caller.
// Eight bytes, little-endian.

#include "pub_sub/schema_layout.h"

#include <zenoh.hxx>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace pub_sub::detail
{

inline std::optional<zenoh::Bytes> layoutAttachment(std::uint64_t layout)
{
    if (layout == kNoLayout)
    {
        return std::nullopt;
    }
    std::vector<std::uint8_t> stamp(sizeof(layout));
    for (std::size_t i = 0; i < stamp.size(); ++i)
    {
        stamp[i] = static_cast<std::uint8_t>((layout >> (i * 8)) & 0xffu);
    }
    return zenoh::Bytes(std::move(stamp));
}

// nullopt for no attachment, or one that is not eight bytes: someone else's
// attachment, not a fingerprint.
inline std::optional<std::uint64_t> layoutFromAttachment(const zenoh::Bytes& attached)
{
    // Into a fixed buffer: this runs on every sample of a typed subscription,
    // and as_vector() allocated for eight bytes.
    std::array<std::uint8_t, sizeof(std::uint64_t)> bytes{};
    if (attached.size() != bytes.size() ||
        attached.reader().read(bytes.data(), bytes.size()) != bytes.size())
    {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
    {
        value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8);
    }
    return value;
}

}  // namespace pub_sub::detail

#endif  // PUB_SUB_DETAIL_LAYOUT_ATTACHMENT_H_
