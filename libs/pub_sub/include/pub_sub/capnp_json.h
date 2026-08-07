#ifndef PUB_SUB_CAPNP_JSON_H_
#define PUB_SUB_CAPNP_JSON_H_

#include <capnp/dynamic.h>
#include <capnp/schema.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>

#include <cstdint>
#include <string>
#include <vector>

namespace pub_sub
{

using json = nlohmann::json;

// Cap'n Proto <-> JSON over the DYNAMIC api, so this works for every schema in
// the registry with no per-schema code. A schema added to schemas/CMakeLists.txt
// becomes readable and publishable through here automatically.

// Decodes a serialised capnp message against `schema`. Returns the message as a
// JSON object.
//
// Throws kj::Exception on a malformed message, which callers must catch --
// capnp's readers signal structural damage that way rather than by return value.
json capnpToJson(const std::vector<std::uint8_t>& bytes, capnp::Schema schema);

// The reverse: fills `builder` from `value`. Appends a description of every
// problem to `errors` and returns false if any were found.
//
// Unknown field names are errors rather than being ignored: a typo that silently
// publishes a default-valued message produces a plausible wrong reading on a
// gauge, which is far harder to notice than a rejection.
bool jsonToCapnp(const json& value, capnp::DynamicStruct::Builder builder,
                 std::vector<std::string>& errors);

// Field names and types of a schema, for callers that want to know what they can
// set before trying.
// The id of `fixedLength` in schemas/annotations.capnp.
//
// Spelled out rather than taken from the generated header because capnpc-c++
// emits no usable C++ constant for an annotation, and because this is the one
// place the number appears: everything else asks fixedListLength() below.
// schemas_test_annotation pins that it still matches the schema.
inline constexpr std::uint64_t kFixedLengthAnnotationId = 0x896cba7e8df9da6eull;

// How many elements this list field always carries, or nullopt when the schema
// does not say.
//
// A capnp list declares no length -- the count is a property of each message --
// so without the annotation a consumer either guesses or has to wait for
// traffic. Both are wrong for a device whose channel count is a fact about the
// hardware. See schemas/annotations.capnp.
std::optional<std::uint32_t> fixedListLength(const capnp::StructSchema::Field& field);

json describeSchema(capnp::Schema schema);

}  // namespace pub_sub

#endif  // PUB_SUB_CAPNP_JSON_H_
