#ifndef PUB_SUB_TOPIC_KEY_H_
#define PUB_SUB_TOPIC_KEY_H_

#include <string>
#include <string_view>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace pub_sub
{

// What a topic key may contain, and how one is packed into the advertisement
// key space.
//
// THE RULE, and why it is enforced rather than assumed:
//
// A topic is advertised by declaring a zenoh liveliness token whose *key
// expression* carries the metadata -- there is no payload on a token, so the
// key is the whole message. Key expressions are '/'-separated segments, so a
// topic name (which contains '/') has to collapse into a single segment or the
// structure is lost. Following ROS 2's rmw_zenoh, that is done by replacing
// '/' with '%':
//
//     vehicle/engine/rpm   ->   vehicle%engine%rpm
//
// That substitution is only reversible while '%' cannot occur in a topic name.
// Nothing structurally prevents someone typing one into a YAML config, and the
// failure would be silent: the advertisement would demangle into a topic name
// that never existed, and a picker would offer a signal that can never bind.
// So the charset is checked, loudly, at every point a key enters the system.
//
// The same check excludes three other characters that would each break
// something quietly:
//
//   '*', '$', '?', '#'  zenoh rejects these outright; a key containing one
//                       fails to construct, so a publisher would silently
//                       never publish.
//   '@'                 a segment starting with '@' is VERBATIM in zenoh: no
//                       wildcard, not even '**', will match it. A topic with
//                       an '@' would be invisible to every '**' subscriber in
//                       the tree, including topic discovery, while looking
//                       perfectly normal in config.

// Characters allowed in a concrete topic key. An allowlist rather than a
// denylist: the failure mode of forgetting to exclude something is a silent
// wrong answer, and the failure mode of being too strict is a startup error
// that says exactly what to fix.
constexpr bool isAllowedTopicChar(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-' || c == '/';
}

// The separator a mangled topic uses in place of '/'. Chosen to match ROS 2's
// rmw_zenoh; the charset check above is what keeps it collision-free.
constexpr char kTopicSeparator = '%';

// The advertisement key space.
//
// The leading '@' is load-bearing, not decoration. Zenoh treats a segment
// beginning with '@' as verbatim, so '**' does NOT match it -- which is what
// keeps advertisements out of the topic observers that subscribe to '**'.
// Without it, every '**' subscriber in the tree would see our own
// advertisements and report them as topics, and each would need its own filter.
// Zenoh's admin space ('@/...') and ROS 2's liveliness space ('@ros2_lv/...')
// use the same property.
inline constexpr std::string_view kAdvertisePrefix = "@redline/adv";

// Everything advertised, for a discovery subscriber. The first segment has to
// be spelled literally: a wildcard cannot match a verbatim segment, and
// '@*/adv/**' is not even a constructible key expression.
inline constexpr std::string_view kAdvertiseAll = "@redline/adv/**";

// True when `key` is a concrete, publishable topic name.
//
// Rejects: empty, any character outside the allowlist, a leading or trailing
// '/', and empty segments ("a//b"). Wildcards are rejected too -- a publisher
// cannot publish on one, and this is the check a publisher uses.
bool isValidTopicKey(std::string_view key);

// True when `expr` is usable as a *subscription*, which is a weaker rule:
// subscribers legitimately use wildcards ("**", "vehicle/**"), and topic
// discovery subscribes to "**". Each segment must be either a valid topic
// segment, "*", or "**".
bool isValidSubscribeExpr(std::string_view expr);

// Returns why `key` is not a valid topic key, for a message a user can act on.
// Empty string when it is valid.
std::string topicKeyProblem(std::string_view key);

// 'vehicle/engine/rpm' <-> 'vehicle%engine%rpm'.
//
// mangle() assumes its argument passed isValidTopicKey(); it does not check,
// because every caller is downstream of a check that already reported the
// problem with more context than this function has.
std::string mangleTopicKey(std::string_view key);
std::string demangleTopicKey(std::string_view mangled);

// '@redline/adv/EngineRpm/vehicle%engine%rpm'
//
// Schema before topic so that '@redline/adv/EngineRpm/*' selects one schema's
// topics. Both fields are one segment, so either can be wildcarded -- which is
// the whole reason for mangling.
std::string advertiseKey(std::string_view topic, std::string_view schema);

// Splits an advertisement key back into its parts. False when it does not have
// the expected shape, which is what a discovery subscriber does with a key it
// does not recognise -- a newer build may advertise a longer form, and skipping
// it is better than guessing.
bool parseAdvertiseKey(std::string_view advertised, std::string& topic, std::string& schema);

// One bad key found in a config tree: where it is, and what is wrong with it.
struct TopicKeyIssue
{
    std::string path;     // e.g. "widgets[3].config.zenoh_key"
    std::string key;      // the offending value
    std::string problem;  // from topicKeyProblem()
};

// Walks a parsed YAML tree and reports every zenoh key that would be refused.
//
// Lives here rather than in config_codec's validator because that one is
// deliberately free of any dependency beyond reflection and yaml -- it should
// not learn what a zenoh key is. Each application converts these into its own
// Issue type, so a bad key is reported with a field path alongside every other
// config problem rather than surfacing much later as a publisher that silently
// refused to start.
//
// Keys are recognised by field name, the same convention the editor's
// properties panel uses: `zenoh_key`, or a prefixed variant such as
// `odometer_zenoh_key` where a widget binds two streams. An empty value is
// skipped -- that is how an unbound widget is spelled.
std::vector<TopicKeyIssue> findBadTopicKeys(const YAML::Node& root);

}  // namespace pub_sub

#endif  // PUB_SUB_TOPIC_KEY_H_
