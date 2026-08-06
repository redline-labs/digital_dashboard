// SPDX-License-Identifier: GPL-3.0-or-later
//
// The serialized schema descriptors the registry generator emits.
//
// A descriptor is the schema as DATA rather than as generated C++: a capnp
// CodeGeneratorRequest, pruned to one schema's transitive closure, embedded in
// the binary. It is what a bag file carries so that something outside this build
// -- another tool, Foxglove Studio, a future version of us -- can decode a
// recorded message.
//
// The failure mode is why this is tested rather than eyeballed. A descriptor
// that is truncated, or that is missing a type one of its fields refers to, does
// NOT fail to load. capnp::SchemaLoader accepts it, the struct appears, and the
// affected field resolves to something wrong or not at all -- at which point the
// recording is undecodable and the recording is the only copy of the data.
//
// So every case here goes through SchemaLoader, the same way a foreign consumer
// would, and compares against what capnp::Schema::from<> says in-process. Two
// independent routes to the same answer; if the descriptor is wrong they
// disagree.
//
// Mutation-check: make addClosure() in the generator skip field types (drop the
// addTypeClosure call) and testNestedTypesAreReachable must fail.

#include "pub_sub/schema_registry.h"

#include <capnp/schema-loader.h>
#include <capnp/schema.capnp.h>
#include <capnp/serialize.h>

#include <kj/array.h>

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace
{

int failures = 0;
int checks = 0;

void expect(bool condition, const std::string& what)
{
    ++checks;
    if (!condition)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    }
}

// Loads a descriptor exactly as a foreign consumer would: parse the
// CodeGeneratorRequest, feed every node to a SchemaLoader, then look up the
// root by id.
struct Loaded
{
    capnp::SchemaLoader loader;
    std::vector<std::uint64_t> node_ids;
    std::string requested_file;
    bool parsed = false;
};

// The descriptor bytes are a flat capnp message, so they are word-aligned by
// construction in the generated array? They are NOT: a std::uint8_t[] has no
// alignment guarantee at all. Copy into a word array first -- which is exactly
// what a real consumer reading them out of a file has to do, and exactly the
// case pub_sub/capnp_payload.h exists for.
bool load(std::span<const std::uint8_t> descriptor, Loaded& out)
{
    if (descriptor.empty() || descriptor.size() % sizeof(capnp::word) != 0)
    {
        return false;
    }

    kj::Array<capnp::word> words = kj::heapArray<capnp::word>(descriptor.size() /
                                                              sizeof(capnp::word));
    std::memcpy(words.begin(), descriptor.data(), descriptor.size());

    capnp::ReaderOptions options;
    options.traversalLimitInWords = 1 << 30;
    capnp::FlatArrayMessageReader reader(words.asPtr(), options);
    auto request = reader.getRoot<capnp::schema::CodeGeneratorRequest>();

    for (auto node : request.getNodes())
    {
        out.node_ids.push_back(node.getId());
        out.loader.load(node);
    }

    for (auto file : request.getRequestedFiles())
    {
        out.requested_file = file.getFilename().cStr();
    }

    out.parsed = true;
    return true;
}

// Field names of a struct schema, sorted.
std::vector<std::string> fieldNames(capnp::StructSchema schema)
{
    std::vector<std::string> names;
    for (auto field : schema.getFields())
    {
        names.emplace_back(field.getProto().getName().cStr());
    }
    std::sort(names.begin(), names.end());
    return names;
}

// ------------------------------------------------------------------ the cases

void testEverySchemaHasADescriptor()
{
    std::size_t total = 0;
    std::size_t missing = 0;

    for (const auto& name : pub_sub::get_available_schemas())
    {
        ++total;
        if (pub_sub::schema_descriptor(name).empty())
        {
            std::fprintf(stderr, "  '%s' has no descriptor\n", std::string(name).c_str());
            ++missing;
        }
    }

    expect(total > 0, "the registry is not empty");
    expect(missing == 0, "every registered schema has a descriptor");
}

// The whole point: the descriptor loads standalone, and the struct it describes
// has the same fields the in-process schema does.
void testDescriptorsAgreeWithTheCompiledSchema()
{
    std::size_t compared = 0;
    bool all_agree = true;

    for (const auto& name : pub_sub::get_available_schemas())
    {
        const auto compiled = pub_sub::get_schema(name);
        if (!compiled)
        {
            continue;
        }

        Loaded loaded;
        if (!load(pub_sub::schema_descriptor(name), loaded))
        {
            std::fprintf(stderr, "  '%s': descriptor did not parse\n", std::string(name).c_str());
            all_agree = false;
            continue;
        }

        const std::uint64_t root_id = compiled->getProto().getId();

        capnp::Schema from_descriptor;
        try
        {
            from_descriptor = loaded.loader.get(root_id);
        }
        catch (const kj::Exception&)
        {
            std::fprintf(stderr, "  '%s': root node absent from its own descriptor\n",
                         std::string(name).c_str());
            all_agree = false;
            continue;
        }

        const std::vector<std::string> expected = fieldNames(compiled->asStruct());
        const std::vector<std::string> actual = fieldNames(from_descriptor.asStruct());

        if (expected != actual)
        {
            std::fprintf(stderr, "  '%s': fields differ (%zu compiled vs %zu in descriptor)\n",
                         std::string(name).c_str(), expected.size(), actual.size());
            all_agree = false;
        }

        ++compared;
    }

    expect(compared > 50, "a substantial number of schemas were compared");
    expect(all_agree,
           "every descriptor loads standalone and reports the same fields as the compiled "
           "schema");
}

// The pruning has to reach types a field REFERS to, not just the struct itself.
// A descriptor missing an enum or a nested struct still loads -- and then that
// one field cannot be resolved, which is a decoding failure long after the
// recording was made.
void testNestedTypesAreReachable()
{
    // RaceGradeTc8ConfigureRequest has two enum-typed fields, which makes it the
    // schema in this tree that actually exercises the closure. A test using only
    // all-primitive schemas would pass against a generator that never followed a
    // field type at all.
    const auto compiled = pub_sub::get_schema("RaceGradeTc8ConfigureRequest");
    if (!compiled)
    {
        expect(false, "RaceGradeTc8ConfigureRequest is registered");
        return;
    }

    Loaded loaded;
    expect(load(pub_sub::schema_descriptor("RaceGradeTc8ConfigureRequest"), loaded),
           "its descriptor parses");
    if (!loaded.parsed)
    {
        return;
    }

    capnp::StructSchema root;
    try
    {
        root = loaded.loader.get(compiled->getProto().getId()).asStruct();
    }
    catch (const kj::Exception& e)
    {
        expect(false, std::string("the root struct loads: ") + e.getDescription().cStr());
        return;
    }

    std::size_t enums_resolved = 0;
    bool all_resolvable = true;

    for (auto field : root.getFields())
    {
        const auto proto = field.getProto();
        if (!proto.isSlot())
        {
            continue;
        }
        const auto type = proto.getSlot().getType();
        if (!type.isEnum())
        {
            continue;
        }

        try
        {
            // THE assertion: the enum's own node has to be in the descriptor, or
            // this throws. It is not enough for the field to exist.
            const capnp::EnumSchema enum_schema =
                loaded.loader.get(type.getEnum().getTypeId()).asEnum();
            expect(enum_schema.getEnumerants().size() > 0,
                   std::string("enum field '") + proto.getName().cStr() +
                       "' resolves to an enum with enumerants");
            ++enums_resolved;
        }
        catch (const kj::Exception&)
        {
            std::fprintf(stderr, "  enum field '%s' refers to a type absent from the descriptor\n",
                         proto.getName().cStr());
            all_resolvable = false;
        }
    }

    expect(enums_resolved == 2, "both enum fields were found and resolved");
    expect(all_resolvable, "every type a field refers to is present in the descriptor");
}

// The file node is included, and requestedFiles names it. A consumer uses that
// to say which file a descriptor describes.
void testRequestedFileIsPresent()
{
    Loaded loaded;
    expect(load(pub_sub::schema_descriptor("CanFrame"), loaded), "CanFrame's descriptor parses");
    if (!loaded.parsed)
    {
        return;
    }

    expect(loaded.requested_file == "can_frame.capnp",
           "requestedFiles names the schema's source file, got '" + loaded.requested_file + "'");
}

// The display name is capnp's, not ours -- a consumer resolving a root node by
// name uses capnp's, because the registry name appears nowhere in the graph.
void testDisplayNames()
{
    expect(pub_sub::schema_display_name(pub_sub::schema_type_t::CanFrame) ==
               "can_frame.capnp:CanFrame",
           "the display name is capnp's qualified form");

    // And it is genuinely different from the registry name, which is the reason
    // both exist.
    expect(pub_sub::schema_display_name(pub_sub::schema_type_t::CanFrame) != "CanFrame",
           "the display name is not the registry name");
}

// Pruning is the reason this is usable at all: a bag writes one descriptor per
// schema it records, so an unpruned descriptor would put the whole node graph in
// the file once per topic.
void testDescriptorsAreSmall()
{
    std::size_t largest = 0;
    std::string largest_name;

    for (const auto& name : pub_sub::get_available_schemas())
    {
        const std::size_t size = pub_sub::schema_descriptor(name).size();
        if (size > largest)
        {
            largest = size;
            largest_name = std::string(name);
        }
    }

    // Generous: the biggest in the tree today is MotecM1Diagnostics at under
    // 4 KiB. This is a regression guard against the pruning silently ceasing to
    // prune -- at which point every descriptor becomes the whole graph.
    expect(largest < 64 * 1024,
           "the largest descriptor (" + largest_name + ", " + std::to_string(largest) +
               " bytes) is still pruned rather than the whole node graph");
}

}  // namespace

int main()
{
    testEverySchemaHasADescriptor();
    testDescriptorsAgreeWithTheCompiledSchema();
    testNestedTypesAreReachable();
    testRequestedFileIsPresent();
    testDisplayNames();
    testDescriptorsAreSmall();

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
