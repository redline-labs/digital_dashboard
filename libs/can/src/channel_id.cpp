// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/channel_id.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <tuple>

namespace can
{
namespace
{

std::string to_lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

bool ChannelId::operator<(const ChannelId& other) const
{
    return std::tie(backend, device, channel) < std::tie(other.backend, other.device, other.channel);
}

std::string ChannelId::toString() const
{
    if (channel == 0)
    {
        return fmt::format("{}:{}", backend, device);
    }
    return fmt::format("{}:{}/{}", backend, device, channel);
}

Result<ChannelId> parse_channel_id(const std::string& text)
{
    const size_t colon = text.find(':');
    if (colon == std::string::npos)
    {
        return invalid_argument(fmt::format(
            "'{}' does not name a channel; expected <backend>:<device>[/<channel>], such as "
            "'socketcan:can0' or 'pcan:0/1'",
            text));
    }

    ChannelId id;
    id.backend = to_lower(text.substr(0, colon));
    if (id.backend.empty())
    {
        return invalid_argument(fmt::format("'{}' has no backend before the colon", text));
    }

    std::string rest = text.substr(colon + 1);
    if (rest.empty())
    {
        return invalid_argument(fmt::format("'{}' names a backend but no device", text));
    }

    // The channel suffix is the last '/', so a device name containing one --
    // an unlikely but legal serial -- still parses.
    const size_t slash = rest.rfind('/');
    if (slash != std::string::npos)
    {
        const std::string channelText = rest.substr(slash + 1);
        unsigned int channel = 0;
        const char* begin = channelText.data();
        const char* end = begin + channelText.size();
        auto [ptr, ec] = std::from_chars(begin, end, channel);

        // Only treat it as a channel suffix if the whole thing is a number.
        // Otherwise it is part of the device name.
        if (!channelText.empty() && ec == std::errc {} && ptr == end)
        {
            if (channel > 255)
            {
                return invalid_argument(
                    fmt::format("'{}' has channel {}, which does not fit in a byte", text, channel));
            }
            id.channel = static_cast<uint8_t>(channel);
            rest = rest.substr(0, slash);
        }
    }

    if (rest.empty())
    {
        return invalid_argument(fmt::format("'{}' names a channel but no device", text));
    }

    id.device = rest;
    return id;
}

} // namespace can
