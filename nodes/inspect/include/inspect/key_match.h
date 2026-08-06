#ifndef INSPECT_KEY_MATCH_H_
#define INSPECT_KEY_MATCH_H_

#include <string_view>

namespace inspect
{

// Does `key` match `pattern`, using zenoh's wildcard rules?
//
//   '*'   matches exactly one segment
//   '**'  matches zero or more segments
//
// A LOCAL matcher rather than zenoh's own, for one reason: the directory being
// filtered is already in memory. Asking zenoh to intersect key expressions
// would mean constructing a KeyExpr per row -- an allocation and a foreign call
// per comparison -- and pulling <zenoh.hxx> (89,000 preprocessed lines) into a
// translation unit that otherwise does a string compare.
//
// The cost of that choice is that this can DISAGREE with zenoh, and a filter
// that quietly omits a topic looks exactly like a topic that is not there. So it
// is separated out and tested rather than sitting inline in the verb.
//
// Deliberately NOT a general zenoh key-expression implementation: '$*' and the
// other zenoh 1.x forms are not supported, because a topic key in this tree
// cannot contain the characters they use (see pub_sub/topic_key.h -- '*', '$',
// '?' and '#' are all rejected at publish time).
bool keyMatches(std::string_view pattern, std::string_view key);

}  // namespace inspect

#endif  // INSPECT_KEY_MATCH_H_
