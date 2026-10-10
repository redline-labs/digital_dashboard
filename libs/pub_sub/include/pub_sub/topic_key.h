#ifndef PUB_SUB_TOPIC_KEY_H_
#define PUB_SUB_TOPIC_KEY_H_

#include <string>
#include <string_view>
#include <vector>

#include "helpers/string_leaf.h"
#include "pub_sub/node_key.h"


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

// '@redline/adv/EngineRpm/vehicle%engine%rpm/a1b2c3d4e5f6'
//
// The same, plus the publisher's zenoh session id -- which is what makes "who
// owns this topic" answerable. Before this, an advertisement said what a topic
// is and never who offers it, so a tool could list every topic on the bus and
// still only print opaque session ids beside them, with nothing joining the two.
//
// The zid rather than a node name, deliberately. A zid is fixed-charset hex, so
// it needs no mangling and cannot trip the isAllowedTopicChar allowlist the way
// a hostname (dots) or a free-text node name would -- and getting that wrong
// produces an unparseable advertisement, which is invisible. The readable name
// comes from the node space below, joined on this zid.
//
// An empty `zid` yields the four-segment form, so a caller with no session still
// produces a valid advertisement.
std::string advertiseKey(std::string_view topic, std::string_view schema, std::string_view zid);

// The node space (@redline/node/<zid>/<name>) is in node_key.h, which this
// includes: it is the one key space the browser's wasm module parses, and it
// cannot take yaml-cpp along.

// The service space: which queryables can be called, and with what.
//
//     @redline/svc/<zid>/<RequestSchema>/<ResponseSchema>/<mangled key>
//
// Services were invisible. pub_sub::ZenohService declares a zenoh queryable and
// nothing announced it, so there was no way to discover that
// CanBridgeSetBitrate or GrayhillSetIndicators exist, let alone what a request
// to one should contain -- you had to read the source of the node that offers
// it. A liveliness token per service makes the whole set enumerable, and the two
// schema names are exactly what a caller needs to build a request and interpret
// a reply.
//
// Both schemas before the key so '@redline/svc/*/CanBridgeSetBitrateRequest/**'
// selects every service taking that request, which is the useful grouping.
inline constexpr std::string_view kServicePrefix = "@redline/svc";
inline constexpr std::string_view kServiceAll = "@redline/svc/**";

std::string serviceKey(std::string_view keyexpr, std::string_view request_schema,
                       std::string_view response_schema, std::string_view zid);

// Accepts SIX OR MORE segments and ignores extras. Written that way from the
// first version rather than retrofitted, which is the lesson parseAdvertiseKey
// had to be taught.
bool parseServiceKey(std::string_view advertised, std::string& keyexpr,
                     std::string& request_schema, std::string& response_schema, std::string& zid);

// Splits an advertisement key back into its parts.
//
// Accepts FOUR OR MORE segments and ignores any it does not know. That
// tolerance is the whole contract of this key space, and it is worth stating
// why: a discovery subscriber drops every key this rejects, so a version of
// parseAdvertiseKey that insisted on an exact segment count would turn the first
// added field into a silent, total outage for every build that predates it --
// its topic picker would go empty, and a picker that can parse no
// advertisements is indistinguishable from a bus with no publishers.
//
// So the rule is append-only and positional: new information goes on the end, a
// reader that wants it must cope with its absence, and a reader that does not
// must cope with its presence.
//
// False only for a key that is not an advertisement at all, or one whose topic
// segment does not demangle into a valid topic key.
bool parseAdvertiseKey(std::string_view advertised, std::string& topic, std::string& schema);

// The same, also yielding the fifth segment: the zenoh session id of the
// publisher that declared the token.
//
// `zid` is set to empty -- not left untouched -- when the advertisement does not
// carry one, which is how an older publisher looks. Empty means "unknown owner",
// never "no owner", and a caller that reports it as the latter is lying about
// what it was told.
bool parseAdvertiseKey(std::string_view advertised, std::string& topic, std::string& schema,
                       std::string& zid);

namespace detail
{

// A zenoh key in a config: a string on the wire, checked by type wherever a
// config is loaded, patched or edited (helpers::StringLeaf). Empty is valid and
// means unbound -- that is how a widget nobody has wired up yet is spelled.
//
// Converts to `const std::string&` implicitly, so code that hands a key to the
// bus does not have to unwrap it; the type exists for the codecs and the forms,
// which are the places a bad key has to be caught by name.
template <typename Tag>
class ConfigKey
{
  public:
    ConfigKey() = default;
    ConfigKey(const char* text) : value_(text) {}
    ConfigKey(std::string text) : value_(std::move(text)) {}

    const std::string& str() const { return value_; }
    operator const std::string&() const { return value_; }
    operator std::string_view() const { return value_; }
    bool empty() const { return value_.empty(); }
    void clear() { value_.clear(); }
    const char* c_str() const { return value_.c_str(); }

    // Exact overloads, not one string_view: with the conversions above, a
    // string_view comparison is ambiguous against std::string's own.
    bool operator==(const ConfigKey&) const = default;
    bool operator==(const std::string& other) const { return value_ == other; }
    bool operator==(const char* other) const { return value_ == other; }

    // Logs as the key itself.
    friend std::string_view format_as(const ConfigKey& key) { return key.value_; }

    static constexpr std::string_view kTypeName = Tag::kTypeName;
    static constexpr std::string_view kFormatHint = "segment/segment, [A-Za-z0-9_-/]";
    static std::string problem(std::string_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const std::string why = topicKeyProblem(text);
        return why.empty() ? std::string() : "is not a usable zenoh key: " + why;
    }

  private:
    std::string value_;
};

struct TopicKeyTag
{
    static constexpr std::string_view kTypeName = "topic_key";
};
struct ServiceKeyTag
{
    static constexpr std::string_view kTypeName = "service_key";
};

}  // namespace detail

// A key a config subscribes or publishes on.
using topic_key_t = detail::ConfigKey<detail::TopicKeyTag>;

// A key a config sends queries to (a ZenohService). The same charset -- its
// liveliness token mangles it exactly as a topic's -- but a distinct type, so
// the forms and describe() do not offer it where a topic belongs.
using service_key_t = detail::ConfigKey<detail::ServiceKeyTag>;

static_assert(helpers::StringLeaf<topic_key_t>);
static_assert(helpers::StringLeaf<service_key_t>);

}  // namespace pub_sub

#endif  // PUB_SUB_TOPIC_KEY_H_
