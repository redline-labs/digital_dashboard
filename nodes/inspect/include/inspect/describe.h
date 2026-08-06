#ifndef INSPECT_DESCRIBE_H_
#define INSPECT_DESCRIBE_H_

#include <nlohmann/json.hpp>

#include <string>

namespace inspect
{

// Renders pub_sub::describeSchema() output as aligned lines on stdout.
//
// Shared by `schema` and `info` because both answer "what fields does this
// have", and the shape they render is not obvious from the call site:
// describeSchema returns
//
//     { "fields": { "rpm": {"type": "uint"}, "mode": {"type": "enum",
//                                                     "values": [...]} } }
//
// -- an OBJECT keyed by field name, not an array of {name, type} pairs. Writing
// that assumption out twice is how the two drift apart, and getting it wrong
// silently prints nothing rather than failing.
void printSchemaFields(const nlohmann::json& described, const std::string& indent);

}  // namespace inspect

#endif  // INSPECT_DESCRIBE_H_
