// SPDX-License-Identifier: GPL-3.0-or-later
#include "pub_sub/schema_layout.h"

#include "pub_sub/schema_registry.h"

#include <capnp/schema-loader.h>
#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>

#include <cstring>

#include <map>
#include <mutex>
#include <set>
#include <string>

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

std::optional<std::uint64_t> layoutHashFor(std::string_view schema_name)
{
    // Computed once per name per process: walking a schema is cheap but this is
    // on the path a publisher takes at construction and a subscriber takes on
    // its first sample.
    static std::mutex mutex;
    static std::map<std::string, std::optional<std::uint64_t>> cache;

    const std::lock_guard<std::mutex> lock(mutex);
    const std::string key(schema_name);
    if (const auto found = cache.find(key); found != cache.end())
    {
        return found->second;
    }

    std::optional<std::uint64_t> result;
    if (const std::optional<capnp::Schema> schema = get_schema(schema_name))
    {
        result = layoutHash(schema->asStruct());
    }
    cache.emplace(key, result);
    return result;
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
