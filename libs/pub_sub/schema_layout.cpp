// SPDX-License-Identifier: GPL-3.0-or-later
#include "pub_sub/schema_layout.h"

#include "pub_sub/schema_registry.h"

#include <capnp/schema-loader.h>
#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>

#include <cstring>

namespace pub_sub
{

std::optional<std::uint64_t> layoutHashFor(std::string_view schema_name)
{
    // A table lookup, not a walk: the generator hashed every registered schema
    // at build time with the same layoutHash() this links, so there is nothing
    // left to compute or to cache. pub_sub_test_schema_layout is what holds the
    // two sides to the same number.
    const std::uint64_t hash = schema_layout_hash(schema_name);
    if (hash == kNoLayout)
    {
        return std::nullopt;
    }
    return hash;
}

std::optional<std::uint64_t> layoutHashOfDescriptor(std::span<const std::uint8_t> descriptor,
                                                    std::string_view schema_name)
{
    if (descriptor.empty() || descriptor.size() % sizeof(capnp::word) != 0)
    {
        return std::nullopt;
    }

    // What this build calls the same schema in capnp's own terms
    // ("motec_m1.capnp:MotecM1Temperatures"): the descriptor's nodes are
    // identified that way, and the registry name is ours alone.
    const std::optional<capnp::Schema> known = get_schema(schema_name);
    if (!known)
    {
        return std::nullopt;
    }
    const capnp::Text::Reader display = known->getProto().getDisplayName();

    // capnp reads whole words and requires alignment, which a byte span does
    // not promise; the same reason WordAlignedPayload exists.
    kj::Array<capnp::word> words = kj::heapArray<capnp::word>(descriptor.size() / sizeof(capnp::word));
    std::memcpy(words.begin(), descriptor.data(), descriptor.size());

    try
    {
        capnp::ReaderOptions options;
        options.traversalLimitInWords = 1 << 30;
        capnp::FlatArrayMessageReader reader(words.asPtr(), options);
        const auto request = reader.getRoot<capnp::schema::CodeGeneratorRequest>();

        capnp::SchemaLoader loader;
        std::uint64_t root = 0;
        for (const auto node : request.getNodes())
        {
            loader.load(node);
            if (node.getDisplayName() == display)
            {
                root = node.getId();
            }
        }
        if (root == 0)
        {
            return std::nullopt;
        }
        return layoutHash(loader.get(root).asStruct());
    }
    catch (const kj::Exception&)
    {
        // A truncated or foreign descriptor. Unknown, not a mismatch: saying
        // "the schema changed" because the bytes could not be read would send
        // someone looking for a change that is not there.
        return std::nullopt;
    }
}

}  // namespace pub_sub
