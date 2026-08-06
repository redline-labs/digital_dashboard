#include "inspect/key_match.h"

#include <cstddef>
#include <vector>

namespace inspect
{

namespace
{

std::vector<std::string_view> segments(std::string_view input)
{
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true)
    {
        const std::size_t slash = input.find('/', start);
        if (slash == std::string_view::npos)
        {
            parts.push_back(input.substr(start));
            return parts;
        }
        parts.push_back(input.substr(start, slash - start));
        start = slash + 1;
    }
}

}  // namespace

bool keyMatches(std::string_view pattern, std::string_view key)
{
    // The two common cases, before doing any work.
    if (pattern == "**" || pattern == key)
    {
        return true;
    }

    const std::vector<std::string_view> pattern_parts = segments(pattern);
    const std::vector<std::string_view> key_parts = segments(key);

    // Recursive descent. Exponential in the worst case for a pattern full of
    // '**', which does not matter here: both sides are a handful of segments and
    // the pattern comes from a human typing a filter.
    const auto match = [&](auto&& self, std::size_t pi, std::size_t ki) -> bool
    {
        if (pi == pattern_parts.size())
        {
            return ki == key_parts.size();
        }

        if (pattern_parts[pi] == "**")
        {
            // Zero or more segments, so every split point has to be tried --
            // including consuming nothing, which is what makes 'a/**/b' match
            // 'a/b'. An implementation that required at least one segment here
            // would silently drop exactly that case.
            for (std::size_t skip = ki; skip <= key_parts.size(); ++skip)
            {
                if (self(self, pi + 1, skip))
                {
                    return true;
                }
            }
            return false;
        }

        if (ki == key_parts.size())
        {
            return false;
        }

        if (pattern_parts[pi] == "*" || pattern_parts[pi] == key_parts[ki])
        {
            return self(self, pi + 1, ki + 1);
        }

        return false;
    };

    return match(match, 0, 0);
}

}  // namespace inspect
