#ifndef PUB_SUB_CAPNP_JSON_H_
#define PUB_SUB_CAPNP_JSON_H_

#include <capnp/dynamic.h>
#include <capnp/schema.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pub_sub
{

using json = nlohmann::json;

// Cap'n Proto <-> JSON over the DYNAMIC api, so this works for every schema in
// the registry with no per-schema code. A schema added to schemas/CMakeLists.txt
// becomes readable and publishable through here automatically.

struct CapnpJsonOptions
{
    // Data fields up to this many bytes decode as a lowercase hex string --
    // the spelling jsonToCapnp accepts, so a decoded reply can be sent back.
    // Larger ones decode as {"_data_bytes": N, "hex_prefix": "<first N bytes>"}.
    //
    // 0, the default, keeps {"_data_bytes": N} with no bytes at all: the Data
    // on topics is video and audio, and a tool echoing those must not inline
    // megabytes of hex it was never asked for.
    std::size_t data_hex_limit = 0;
};

// Decodes a serialised capnp message against `schema`. Returns the message as a
// JSON object.
//
// Throws kj::Exception on a malformed message, which callers must catch --
// capnp's readers signal structural damage that way rather than by return value.
json capnpToJson(const std::vector<std::uint8_t>& bytes, capnp::Schema schema);
json capnpToJson(const std::vector<std::uint8_t>& bytes, capnp::Schema schema,
                 const CapnpJsonOptions& options);

// A message already in hand -- e.g. a freshly initialised builder's reader,
// which is how a form learns a schema's declared defaults.
json capnpToJson(capnp::DynamicStruct::Reader reader, const CapnpJsonOptions& options = {});

// The reverse: fills `builder` from `value`. Appends a description of every
// problem to `errors` and returns false if any were found.
//
// Unknown field names are errors rather than being ignored: a typo that silently
// publishes a default-valued message produces a plausible wrong reading on a
// gauge, which is far harder to notice than a rejection.
//
// Spellings, for every field and every list element alike:
//   integers  JSON integers, range-checked for the field's width. Negative into
//             an unsigned type is an error, never a wrap.
//   floats    any JSON number
//   Text      string          Enum    enumerant name
//   Data      hex string, as helpers::fromHex reads it: whitespace and ':'
//             between bytes, optional 0x
//   struct    object          List    array (nested lists recurse)
//   union     object naming exactly one arm, e.g. {"value": {"speed": 3.5}}
//   Void      null, true or {} (selects a payload-less union arm)
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

// {"fields": {name: entry}, "union": true?}. Each entry has:
//   type        the category every picker reasons about: bool int uint float
//               text data list struct enum void other
//   capnp_type  the exact type: int8..uint64, float32/64, group, ...
//   min, max    integer bounds, exact (uint64 max is not rounded)
//   values      enumerant names in declaration order (enums, lists of enums)
//   value_docs  their doc comments, when any has one
//   fields      a struct's or group's own entries, recursively; "union": true
//               when they are a union's arms
//   element     a list element's entry; element_type and fixed_length as well
//   union_arm   true for a field that is one arm of its struct's union
//   default     the schema's default (Data as hex); absent for an inactive arm
//   doc         the field's doc comment, from the registry
//   order       declaration order -- the JSON object itself is alphabetical
// Everything but type, element_type, fixed_length and values was added for the
// web console's form; readers that ignore unknown keys see no change.
json describeSchema(capnp::Schema schema);

}  // namespace pub_sub

#endif  // PUB_SUB_CAPNP_JSON_H_
