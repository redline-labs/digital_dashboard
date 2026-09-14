// SPDX-License-Identifier: GPL-3.0-or-later
//
// The doc comments the registry generator lifts out of schemas/*.capnp.
//
// capnp::Schema carries no comments, so a program building a form from a schema
// at runtime had field names and types and nothing to say what a field is for.
// The generator now emits them from CodeGeneratorRequest.sourceInfo. What is
// worth pinning is the INDEXING: members are looked up by the same index
// capnp::StructSchema::Field::getIndex() returns, and an off-by-one there puts
// every tooltip on the wrong field -- plausible text, silently misattributed.
//
// Mutation-check: have the generator emit members in reverse, and the
// per-field checks below must fail.

#include "pub_sub/schema_registry.h"

#include "carplay_location.capnp.h"
#include "grayhill_keypad.capnp.h"

#include <capnp/schema.h>

#include <cstdio>
#include <string>
#include <string_view>

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

bool contains(std::string_view haystack, std::string_view needle)
{
    return haystack.find(needle) != std::string_view::npos;
}

std::string_view fieldDoc(capnp::StructSchema schema, const char* field)
{
    return pub_sub::member_doc(schema.getProto().getId(),
                               schema.getFieldByName(field).getIndex());
}

}  // namespace

int main()
{
    // A struct's doc comment: the lines directly inside its opening brace.
    {
        const auto schema = capnp::Schema::from<GrayhillSetIndicatorsRequest>();
        const std::string_view doc = pub_sub::schema_doc(schema.getProto().getId());
        expect(contains(doc, "Up to eight bytes, indicators 1..64"),
               "a struct's doc comment is available by node id");
        expect(!doc.empty() && doc.back() != '\n', "the trailing newline capnp keeps is trimmed");
    }

    // Field comments, each on its own field. Neighbours with different text, so
    // a shifted index reads the wrong one rather than an identical one.
    {
        const auto schema = capnp::Schema::from<CarPlayLocation>().asStruct();
        expect(fieldDoc(schema, "latitudeDeg") == "positive north",
               "the first field's comment is on the first field");
        expect(fieldDoc(schema, "longitudeDeg") == "positive east",
               "the second field's comment is on the second field, not the first's");
        expect(fieldDoc(schema, "utcEpochMs") == "fix time; 0 = use current wall clock",
               "a later field's comment is on that field");
    }

    // Absent is empty, never an error.
    {
        expect(pub_sub::schema_doc(0).empty(), "an unknown node id has no doc");
        const auto schema = capnp::Schema::from<CarPlayLocation>();
        expect(pub_sub::member_doc(schema.getProto().getId(), 10000).empty(),
               "an out-of-range member index has no doc");
    }

    if (failures != 0)
    {
        std::fprintf(stderr, "%d/%d checks failed\n", failures, checks);
        return 1;
    }
    std::fprintf(stderr, "all %d checks passed\n", checks);
    return 0;
}
