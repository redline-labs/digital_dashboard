#include "bag/playback.h"

namespace bag
{

std::map<std::string, std::string> parseRemaps(const std::vector<std::string>& arguments,
                                               std::vector<std::string>& problems)
{
    std::map<std::string, std::string> remaps;

    for (const std::string& entry : arguments)
    {
        const std::size_t equals = entry.find('=');

        if (equals == std::string::npos)
        {
            problems.push_back("--remap wants old=new, got '" + entry + "'");
            continue;
        }

        const std::string from = entry.substr(0, equals);
        const std::string to = entry.substr(equals + 1);

        // Either half empty means a key that cannot be matched or cannot be
        // published. Both are typos ("=new", "old="), and both would otherwise
        // fail later and elsewhere.
        if (from.empty() || to.empty())
        {
            problems.push_back("--remap needs a key on both sides of '=', got '" + entry + "'");
            continue;
        }

        // A repeated source is ambiguous. Reported rather than resolved by
        // last-wins, because which one wins is not something a user should have
        // to know.
        if (remaps.count(from) != 0)
        {
            problems.push_back("--remap names '" + from + "' more than once");
            continue;
        }

        remaps.emplace(from, to);
    }

    return remaps;
}

std::string resolvePlaybackKey(std::string_view recorded_key,
                               const std::map<std::string, std::string>& remaps,
                               std::string_view prefix)
{
    std::string key(recorded_key);

    // Remap first: it names the key as RECORDED. See the header.
    if (const auto found = remaps.find(key); found != remaps.end())
    {
        key = found->second;
    }

    if (!prefix.empty())
    {
        // A prefix with a trailing '/' would otherwise produce '//' -- an empty
        // segment, which pub_sub::isValidTopicKey rejects, so every message
        // would be silently dropped. Typing `--prefix replay/` is the obvious
        // thing to do.
        std::string_view trimmed = prefix;
        while (!trimmed.empty() && trimmed.back() == '/')
        {
            trimmed.remove_suffix(1);
        }

        if (!trimmed.empty())
        {
            key = std::string(trimmed) + "/" + key;
        }
    }

    return key;
}

}  // namespace bag
