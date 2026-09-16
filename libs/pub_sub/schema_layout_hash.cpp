// SPDX-License-Identifier: GPL-3.0-or-later
//
// The fingerprint walk itself, and nothing else.
//
// This file is compiled into TWO targets: zenoh_pub_sub, where it hashes a
// schema this build links, and capnpc_schema_registry, the `capnp compile`
// plugin, where it hashes the same schemas at build time so the registry can
// carry each one as a constant.
//
// That is why it is split out of schema_layout.cpp: the rest of that file needs
// pub_sub/schema_registry.h, which is the very thing the plugin generates. One
// implementation shared by both sides is the point -- two implementations that
// had to agree bit for bit would be a worse trade than the runtime walk it
// replaces.
#include "pub_sub/schema_layout.h"

#include <set>
#include <string_view>

namespace pub_sub
{

namespace
{

// FNV-1a, 64-bit. Not a cryptographic hash and does not need to be: it defends
// against an honest mismatch -- a node built from a different revision of the
// schemas -- not against anyone trying to forge one.
constexpr std::uint64_t kOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kPrime = 1099511628211ull;

void mix(std::uint64_t& hash, std::uint64_t value)
{
    for (int byte = 0; byte < 8; ++byte)
    {
        hash ^= (value >> (byte * 8)) & 0xffull;
        hash *= kPrime;
    }
}

void mix(std::uint64_t& hash, std::string_view text)
{
    for (const char c : text)
    {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= kPrime;
    }
    mix(hash, text.size());
}

// Deep enough to reach through the nesting the schemas here actually use, and
// shallow enough that a schema that refers to itself cannot run away. `seen`
// stops a cycle from being followed twice.
constexpr int kMaxDepth = 8;

void mixType(std::uint64_t& hash, capnp::Type type, std::set<std::uint64_t>& seen, int depth);

void mixStruct(std::uint64_t& hash, capnp::StructSchema schema, std::set<std::uint64_t>& seen,
               int depth)
{
    const std::uint64_t id = schema.getProto().getId();
    mix(hash, id);

    if (depth >= kMaxDepth || !seen.insert(id).second)
    {
        return;
    }

    const auto fields = schema.getFields();
    mix(hash, fields.size());
    for (const capnp::StructSchema::Field field : fields)
    {
        const auto proto = field.getProto();
        const capnp::Text::Reader name = proto.getName();
        mix(hash, std::string_view(name.cStr(), name.size()));
        mix(hash, field.getIndex());
        mix(hash, proto.getDiscriminantValue());
        if (proto.isSlot())
        {
            // The offset is the whole point: it is where the field's bytes are.
            mix(hash, proto.getSlot().getOffset());
        }
        mixType(hash, field.getType(), seen, depth + 1);
    }
}

void mixType(std::uint64_t& hash, capnp::Type type, std::set<std::uint64_t>& seen, int depth)
{
    mix(hash, static_cast<std::uint64_t>(type.which()));

    if (type.isStruct())
    {
        mixStruct(hash, type.asStruct(), seen, depth);
        return;
    }
    if (type.isEnum())
    {
        const auto schema = type.asEnum();
        mix(hash, schema.getProto().getId());
        // The enumerant count, so adding a value is a change: a reader that
        // does not know the new value reports it as unknown.
        mix(hash, schema.getEnumerants().size());
        return;
    }
    if (type.isList())
    {
        mixType(hash, type.asList().getElementType(), seen, depth + 1);
        return;
    }
    if (type.isInterface())
    {
        mix(hash, type.asInterface().getProto().getId());
    }
}

}  // namespace

std::uint64_t layoutHash(capnp::StructSchema schema)
{
    std::uint64_t hash = kOffsetBasis;
    std::set<std::uint64_t> seen;
    mixStruct(hash, schema, seen, 0);
    // Never zero: kNoLayout means "not known", and a fingerprint that happened
    // to be zero would read as an unstamped sample.
    return hash == kNoLayout ? kPrime : hash;
}

}  // namespace pub_sub
