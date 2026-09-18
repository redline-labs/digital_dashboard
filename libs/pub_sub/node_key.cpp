#include "pub_sub/node_key.h"

namespace pub_sub
{

std::vector<std::string_view> keySegments(std::string_view key)
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
    const std::vector<std::string_view> parts = keySegments(advertised);

    // "@redline" / "node" / <zid> / <name>, extras ignored.
    constexpr std::size_t kMinimumSegments = 4;
    if (parts.size() < kMinimumSegments)
    {
        return false;
    }

    const std::vector<std::string_view> prefix = keySegments(kNodePrefix);
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

}  // namespace pub_sub
