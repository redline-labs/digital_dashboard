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
    std::string out(kAdvertisePrefix);
    out += '/';
    out += schema;
    out += '/';
    out += mangleTopicKey(topic);
    return out;
}

bool parseAdvertiseKey(std::string_view advertised, std::string& topic, std::string& schema)
{
    const std::vector<std::string_view> parts = segments(advertised);

    // "@redline" / "adv" / <schema> / <mangled topic>. Anything longer is a
    // form this build does not know; skipping it beats guessing which segment
    // means what.
    constexpr std::size_t kExpectedSegments = 4;
    if (parts.size() != kExpectedSegments)
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
