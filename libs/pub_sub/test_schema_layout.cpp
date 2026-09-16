// SPDX-License-Identifier: GPL-3.0-or-later
//
// The schema fingerprint: what tells a message written against one revision of
// a schema from the same message written against another.
//
// The name on a sample says WHICH message it is. It has never said which
// version, and capnp will decode whatever it is handed -- so widening a field
// (the M1's temperatures went from Int8 to Int16) leaves every reader of the
// old build reporting plausible wrong numbers. These checks pin the two
// properties that makes usable: the same schema always hashes the same, and a
// descriptor recorded by another build hashes to the same value as the schema
// it describes.
//
// The fingerprints of registered schemas are CONSTANTS, computed by the
// registry generator while it had capnp's parse in hand. That makes the check
// below the load-bearing one: the generator hashes a schema it loaded out of a
// CodeGeneratorRequest, and this hashes the schema the compiler generated code
// for. Those are two different capnp::StructSchema objects reached two
// different ways, and every fingerprint in the build is wrong if they disagree.
// The same equivalence is what layoutHashOfDescriptor() depends on.
#include "pub_sub/schema_layout.h"

#include "pub_sub/detail/byte_subscriber.h"
#include "pub_sub/schema_registry.h"

#include "engine_rpm.capnp.h"

#include <capnp/schema.h>

#include <cstdio>
#include <set>
#include <string>

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

}  // namespace

int main()
{
    // Generated constants against the linked schemas, for every schema in the
    // registry rather than a sample of them: this is the only place the two
    // halves of the fingerprint meet, and a schema with unusual shape (a union,
    // a group, a nested struct, a list of structs) is exactly where a loaded
    // schema and a compiled one could diverge.
    for (const std::string_view name : pub_sub::get_available_schemas())
    {
        const std::optional<capnp::Schema> schema = pub_sub::get_schema(name);
        expect(schema.has_value(), std::string(name) + " resolves to a schema");
        if (!schema)
        {
            continue;
        }
        expect(pub_sub::schema_layout_hash(name) == pub_sub::layoutHash(schema->asStruct()),
               std::string(name) + "'s generated fingerprint matches the schema it links");
    }

    // The compile-time form. schema_traits<T>::layout is usable in a constant
    // expression, which is the point of generating it at all.
    static_assert(pub_sub::schema_traits<::EngineRpm>::layout != pub_sub::kNoLayout,
                  "a registered schema's fingerprint is a constant, and not the unknown value");
    expect(pub_sub::schema_traits<::EngineRpm>::layout == pub_sub::schema_layout_hash("EngineRpm"),
           "the traits constant and the name lookup are the same number");

    expect(pub_sub::schema_layout_hash("NoSuchSchema") == pub_sub::kNoLayout,
           "an unknown name has no fingerprint in the generated table either");

    // Stable: the same schema, asked twice, in two ways.
    const auto rpm = pub_sub::layoutHashFor("EngineRpm");
    expect(rpm.has_value(), "a registered schema has a fingerprint");
    expect(rpm == pub_sub::layoutHashFor("EngineRpm"), "and it does not change between calls");
    if (const auto schema = pub_sub::get_schema("EngineRpm"))
    {
        expect(pub_sub::layoutHash(schema->asStruct()) == rpm,
               "computed from the schema directly, it is the same number");
    }

    expect(pub_sub::layoutHashFor("NoSuchSchema") == std::nullopt,
           "a name this build does not know has no fingerprint");
    expect(rpm != pub_sub::kNoLayout, "a real fingerprint is never the 'unknown' value");

    // Different schemas, different fingerprints. Not a proof -- it is a 64-bit
    // hash -- but a collision across the whole registry would say the input is
    // not distinguishing enough.
    std::set<std::uint64_t> seen;
    std::size_t counted = 0;
    for (const std::string_view name : pub_sub::get_available_schemas())
    {
        if (const auto hash = pub_sub::layoutHashFor(name))
        {
            ++counted;
            seen.insert(*hash);
        }
    }
    expect(counted > 20, "the registry has schemas to compare");
    expect(seen.size() == counted, "every schema in the registry has its own fingerprint");

    // A descriptor is what a recording stores: the same schema, as data. It has
    // to hash to the same value, or a replay would report every recording as
    // written against a different revision.
    for (const std::string_view name : {std::string_view("EngineRpm"), std::string_view("CanFrame"),
                                        std::string_view("MotecM1Temperatures")})
    {
        const auto from_descriptor =
            pub_sub::layoutHashOfDescriptor(pub_sub::schema_descriptor(name), name);
        expect(from_descriptor.has_value(),
               std::string(name) + "'s stored descriptor can be read back");
        expect(from_descriptor == pub_sub::layoutHashFor(name),
               std::string(name) + " hashes the same from its descriptor as from the registry");
    }

    // Nothing to compare against is not a mismatch.
    expect(pub_sub::layoutHashOfDescriptor({}, "EngineRpm") == std::nullopt,
           "an empty descriptor has no fingerprint");
    const std::uint8_t garbage[16] = {};
    expect(pub_sub::layoutHashOfDescriptor(std::span<const std::uint8_t>(garbage, sizeof(garbage)),
                                           "EngineRpm") == std::nullopt,
           "a descriptor that is not a schema has none either");
    expect(pub_sub::layoutHashOfDescriptor(pub_sub::schema_descriptor("EngineRpm"),
                                           "NoSuchSchema") == std::nullopt,
           "and neither does a name this build does not know");

    // The decision the fingerprint exists to make. layoutMatches() takes the
    // expected value as an argument -- the typed subscriber passes
    // schema_traits<T>::layout, a constant -- so what happens on a mismatch can
    // be checked here without a bus.
    const std::uint64_t engine_rpm = pub_sub::schema_traits<::EngineRpm>::layout;
    expect(pub_sub::detail::layoutMatches("a/key", "EngineRpm", engine_rpm, engine_rpm),
           "a sample stamped with this build's fingerprint is decoded");
    expect(!pub_sub::detail::layoutMatches("a/key", "EngineRpm", engine_rpm, engine_rpm + 1),
           "a sample stamped with another revision's is dropped");
    expect(pub_sub::detail::layoutMatches("a/key", "EngineRpm", engine_rpm, std::nullopt),
           "an unstamped sample is decoded, so a publisher predating this keeps working");
    expect(pub_sub::detail::layoutMatches("a/key", "Whatever", pub_sub::kNoLayout, engine_rpm),
           "a schema this build does not know is not a mismatch");

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
