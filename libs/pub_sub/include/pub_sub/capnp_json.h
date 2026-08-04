#ifndef PUB_SUB_CAPNP_JSON_H_
#define PUB_SUB_CAPNP_JSON_H_

#include <capnp/dynamic.h>
#include <capnp/schema.h>

#include <nlohmann/json.hpp>

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
json describeSchema(capnp::Schema schema);

}  // namespace pub_sub

#endif  // PUB_SUB_CAPNP_JSON_H_
