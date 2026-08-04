// SPDX-License-Identifier: GPL-3.0-or-later

#include "can/backend.h"

#include <spdlog/fmt/fmt.h>

namespace can
{

Backend::~Backend() = default;

Registry::Registry() = default;
Registry::~Registry() = default;
Registry::Registry(Registry&&) noexcept = default;
Registry& Registry::operator=(Registry&&) noexcept = default;

void Registry::add(std::shared_ptr<Backend> backend)
{
    if (backend != nullptr)
    {
        backends_.push_back(std::move(backend));
    }
}

std::vector<ChannelInfo> Registry::enumerate() const
{
    std::vector<ChannelInfo> all;
    for (const auto& backend : backends_)
    {
        auto found = backend->enumerate();
        all.insert(all.end(), std::make_move_iterator(found.begin()),
                   std::make_move_iterator(found.end()));
    }
    return all;
}

Result<std::shared_ptr<Channel>> Registry::open(const ChannelId& id,
                                                const OpenOptions& options) const
{
    for (const auto& backend : backends_)
    {
        if (backend->name() == id.backend)
        {
            return backend->open(id, options);
        }
    }

    std::string known;
    for (const auto& backend : backends_)
    {
        known += (known.empty() ? "" : ", ") + backend->name();
    }
    return not_found(fmt::format("no backend named '{}'; this build has {}", id.backend,
                                 known.empty() ? "none" : known));
}

Result<std::shared_ptr<Channel>> Registry::open(const std::string& id,
                                                const OpenOptions& options) const
{
    auto parsed = parse_channel_id(id);
    if (!parsed.has_value())
    {
        return std::unexpected(parsed.error());
    }
    return open(*parsed, options);
}

std::vector<std::string> Registry::backend_names() const
{
    std::vector<std::string> names;
    names.reserve(backends_.size());
    for (const auto& backend : backends_)
    {
        names.push_back(backend->name());
    }
    return names;
}

} // namespace can
