#ifndef PUB_SUB_NODE_KEY_H_
#define PUB_SUB_NODE_KEY_H_

// The node key space, apart from topic_key.h so that it needs nothing but the
// standard library: the web console's wasm module parses these keys too, and
// topic_key.h brings yaml-cpp.

#include <string>
#include <string_view>
#include <vector>

namespace pub_sub
{

// Splits on '/' WITHOUT collapsing empty segments, because an empty segment is
// exactly what the validators are looking for. A split that swallowed them
// would make "a//b" and "a/b" indistinguishable, which is the bug rather than
// the fix.
std::vector<std::string_view> keySegments(std::string_view key);

// The node space: which of our processes are alive, and what they are called.
//
//     @redline/node/<zid>/<node_name>
//
// Separate from the advertisement space because it answers a different question
// and has a different lifetime. An advertisement exists per publisher; this
// exists once per process, and it is the ONLY way a process that subscribes but
// never publishes -- scope, the dashboard, the editor -- appears on the bus at
// all. Those three were previously invisible to every tool.
//
// The '@' is load-bearing here for the same reason it is on the advertisement
// prefix: a leading-'@' segment is verbatim in zenoh, so '**' does not match it
// and these do not show up as topics in every wildcard subscriber in the tree.
inline constexpr std::string_view kNodePrefix = "@redline/node";
inline constexpr std::string_view kNodeAll = "@redline/node/**";

std::string nodeKey(std::string_view zid, std::string_view node_name);

// Accepts FOUR OR MORE segments and ignores extras -- the same append-only rule
// as parseAdvertiseKey, and for the same reason. False when the key is not a
// node advertisement or either field is empty.
bool parseNodeKey(std::string_view advertised, std::string& zid, std::string& node_name);

}  // namespace pub_sub

#endif  // PUB_SUB_NODE_KEY_H_
