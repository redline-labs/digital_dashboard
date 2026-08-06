#include "pub_sub/topic_key.h"

#include <algorithm>
#include <vector>

namespace pub_sub
{

namespace
{

// Splits on '/' WITHOUT collapsing empty segments, because an empty segment is
// exactly what the validators are looking for. A split that swallowed them
// would make "a//b" and "a/b" indistinguishable, which is the bug rather than
// the fix.
std::vector<std::string_view> segments(std::string_view key)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t slash = key.find('/', start);
        if (slash == std::string_view::npos)
        {
            out.push_back(key.substr(start));
            return out;
        }
        out.push_back(key.substr(start, slash - start));
        start = slash + 1;
    }
}

bool isValidSegment(std::string_view segment)
{
    if (segment.empty())
    {
        return false;
    }
    return std::all_of(segment.begin(), segment.end(),
                       [](char c) { return isAllowedTopicChar(c) && c != '/'; });
}

}  // namespace

std::string topicKeyProblem(std::string_view key)
{
    if (key.empty())
    {
        return "the key is empty";
    }

    // Reported before the charset check, because "vehicle/engine/rpm/" is a
    // much more likely typo than a stray character and deserves the specific
    // message rather than being told about segment contents.
    if (key.front() == '/')
    {
        return "the key starts with '/'";
    }
    if (key.back() == '/')
    {
        return "the key ends with '/'";
    }

    for (const char c : key)
    {
        if (isAllowedTopicChar(c))
        {
            continue;
        }

        // Each of these is a silent failure rather than a loud one, so each
        // gets a reason rather than "invalid character".
        switch (c)
        {
            case kTopicSeparator:
                return std::string("the key contains '") + kTopicSeparator +
                       "', which is reserved: it is the separator a topic name is mangled to "
                       "when advertised, so a key containing one could not be recovered";
            case '@':
                return "the key contains '@'; zenoh treats a segment starting with '@' as "
                       "verbatim, so no wildcard subscription -- including topic discovery -- "
                       "would ever match it";
            case '*':
            case '$':
            case '?':
            case '#':
                return std::string("the key contains '") + c +
                       "', which zenoh does not allow in a key expression";
            default:
                break;
        }

        return std::string("the key contains '") + c +
               "'; only letters, digits, '_', '-' and '/' are allowed";
    }

    for (const std::string_view segment : segments(key))
    {
        if (segment.empty())
        {
            return "the key has an empty segment ('//')";
        }
    }

    return {};
}

bool isValidTopicKey(std::string_view key)
{
    return topicKeyProblem(key).empty();
}

bool isValidSubscribeExpr(std::string_view expr)
{
    if (expr.empty() || expr.front() == '/' || expr.back() == '/')
    {
        return false;
    }

    for (const std::string_view segment : segments(expr))
    {
        // Subscribers may wildcard; publishers may not. That asymmetry is the
        // whole reason this is a separate function -- topic discovery
        // legitimately subscribes to "**", which isValidTopicKey rejects.
        if (segment == "*" || segment == "**")
        {
            continue;
        }
        if (!isValidSegment(segment))
        {
            return false;
        }
    }

    return true;
}

std::string mangleTopicKey(std::string_view key)
{
    std::string out(key);
    std::replace(out.begin(), out.end(), '/', kTopicSeparator);
    return out;
}

std::string demangleTopicKey(std::string_view mangled)
{
    std::string out(mangled);
    std::replace(out.begin(), out.end(), kTopicSeparator, '/');
    return out;
}

std::string advertiseKey(std::string_view topic, std::string_view schema)
{
    return advertiseKey(topic, schema, std::string_view{});
}

std::string advertiseKey(std::string_view topic, std::string_view schema, std::string_view zid)
{
    std::string out(kAdvertisePrefix);
    out += '/';
    out += schema;
    out += '/';
    out += mangleTopicKey(topic);

    // Omitted rather than emitted empty. An empty trailing segment would make
    // the key five segments with nothing in the fifth, which parses back as a
    // zid of "" -- the same answer, reached by a key that is harder to read and
    // that a wildcard like '@redline/adv/*/*/*' would match differently.
    if (!zid.empty())
    {
        out += '/';
        out += zid;
    }

    return out;
}

std::string nodeKey(std::string_view zid, std::string_view node_name)
{
    std::string out(kNodePrefix);
    out += '/';
    out += zid;
    out += '/';
    out += node_name;
    return out;
}

bool parseNodeKey(std::string_view advertised, std::string& zid, std::string& node_name)
{
    const std::vector<std::string_view> parts = segments(advertised);

    // "@redline" / "node" / <zid> / <name>, extras ignored.
    constexpr std::size_t kMinimumSegments = 4;
    if (parts.size() < kMinimumSegments)
    {
        return false;
    }

    const std::vector<std::string_view> prefix = segments(kNodePrefix);
    if (parts[0] != prefix[0] || parts[1] != prefix[1])
    {
        return false;
    }

    if (parts[2].empty() || parts[3].empty())
    {
        return false;
    }

    zid = std::string(parts[2]);
    node_name = std::string(parts[3]);
    return true;
}

std::string serviceKey(std::string_view keyexpr, std::string_view request_schema,
                       std::string_view response_schema, std::string_view zid)
{
    std::string out(kServicePrefix);
    out += '/';
    out += zid;
    out += '/';
    out += request_schema;
    out += '/';
    out += response_schema;
    out += '/';
    out += mangleTopicKey(keyexpr);
    return out;
}

bool parseServiceKey(std::string_view advertised, std::string& keyexpr,
                     std::string& request_schema, std::string& response_schema, std::string& zid)
{
    const std::vector<std::string_view> parts = segments(advertised);

    // "@redline" / "svc" / <zid> / <request> / <response> / <mangled key>,
    // extras ignored.
    constexpr std::size_t kMinimumSegments = 6;
    if (parts.size() < kMinimumSegments)
    {
        return false;
    }

    const std::vector<std::string_view> prefix = segments(kServicePrefix);
    if (parts[0] != prefix[0] || parts[1] != prefix[1])
    {
        return false;
    }

    if (parts[2].empty() || parts[3].empty() || parts[4].empty() || parts[5].empty())
    {
        return false;
    }

    zid = std::string(parts[2]);
    request_schema = std::string(parts[3]);
    response_schema = std::string(parts[4]);
    keyexpr = demangleTopicKey(parts[5]);

    // Same reasoning as the advertisement space: a key that does not demangle
    // into something callable means the advertiser broke the contract, and
    // offering it to a caller would produce a request that can never be routed.
    return isValidTopicKey(keyexpr);
}

bool parseAdvertiseKey(std::string_view advertised, std::string& topic, std::string& schema)
{
    std::string ignored_zid;
    return parseAdvertiseKey(advertised, topic, schema, ignored_zid);
}

bool parseAdvertiseKey(std::string_view advertised, std::string& topic, std::string& schema,
                       std::string& zid)
{
    const std::vector<std::string_view> parts = segments(advertised);

    // "@redline" / "adv" / <schema> / <mangled topic> [ / <zid> [ / ... ] ]
    //
    // AT LEAST four, and extras ignored. This used to require EXACTLY four and
    // return false otherwise, with the reasoning that a longer form is one this
    // build does not understand and skipping beats guessing.
    //
    // That reasoning was backwards, and the cost of finding out would have been
    // high. TopicDirectory drops every key this rejects -- so the first build to
    // append a segment would have made every *older* build's topic picker go
    // completely empty, with no error anywhere, because a scope that can parse
    // no advertisements looks exactly like a bus with no publishers. "Ignore
    // what you do not recognise" is the rule that makes a key space extensible;
    // "reject what you do not recognise" makes the first extension a breaking
    // change for everything already deployed.
    //
    // The trailing segments are positional and append-only for the same reason:
    // a reader that wants segment 5 must tolerate its absence (an older
    // publisher), and a reader that does not want it must tolerate its presence.
    constexpr std::size_t kMinimumSegments = 4;
    if (parts.size() < kMinimumSegments)
    {
        return false;
    }

    const std::vector<std::string_view> prefix = segments(kAdvertisePrefix);
    if (parts[0] != prefix[0] || parts[1] != prefix[1])
    {
        return false;
    }

    if (parts[2].empty() || parts[3].empty())
    {
        return false;
    }

    schema = std::string(parts[2]);
    topic = demangleTopicKey(parts[3]);

    // Empty when the advertiser is an older build that did not carry one. A
    // caller must treat that as "unknown", never as "no owner".
    zid = parts.size() > kMinimumSegments ? std::string(parts[kMinimumSegments]) : std::string();

    // A mangled segment cannot contain '/', so anything demangling into a key
    // that fails validation means the advertiser broke the contract. Refusing
    // it here keeps a malformed advertisement from reaching a picker as a
    // topic nothing can ever bind to.
    return isValidTopicKey(topic);
}

namespace
{

bool namesATopicKey(const std::string& field)
{
    return field == "zenoh_key" || (field.size() > 10 && field.ends_with("_zenoh_key"));
}

// Depth-first, building the dotted/indexed path as it goes so a report can name
// exactly which field to fix.
void collectBadKeys(const YAML::Node& node,
                    const std::string& path,
                    std::vector<TopicKeyIssue>& out)
{
    if (node.IsMap())
    {
        for (const auto& entry : node)
        {
            std::string name;
            try
            {
                name = entry.first.as<std::string>();
            }
            catch (const YAML::Exception&)
            {
                continue;  // A non-scalar key. Not ours to complain about.
            }

            const std::string child_path = path.empty() ? name : path + "." + name;

            if (namesATopicKey(name) && entry.second.IsScalar())
            {
                std::string value;
                try
                {
                    value = entry.second.as<std::string>();
                }
                catch (const YAML::Exception&)
                {
                    continue;  // Reported by the type check in the generic validator.
                }

                // Empty is how an unbound widget is spelled, and several
                // shipped configs have one.
                if (!value.empty())
                {
                    if (const std::string problem = topicKeyProblem(value); !problem.empty())
                    {
                        out.push_back({child_path, value, problem});
                    }
                }
                continue;
            }

            collectBadKeys(entry.second, child_path, out);
        }
        return;
    }

    if (node.IsSequence())
    {
        for (std::size_t i = 0; i < node.size(); ++i)
        {
            collectBadKeys(node[i], path + "[" + std::to_string(i) + "]", out);
        }
    }
}

}  // namespace

std::vector<TopicKeyIssue> findBadTopicKeys(const YAML::Node& root)
{
    std::vector<TopicKeyIssue> out;
    collectBadKeys(root, "", out);
    return out;
}

}  // namespace pub_sub
