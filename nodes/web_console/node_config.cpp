#include "node_config.h"

#include "core/core.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

namespace web_console
{
namespace
{

struct Context
{
    bool ok { true };

    void fail(const std::string& message)
    {
        SPDLOG_ERROR("[config] {}", message);
        ok = false;
    }
};

} // namespace

bool parse_node_config(const std::string& yaml, NodeConfig& out)
{
    Context context;

    YAML::Node root;
    try
    {
        root = YAML::Load(yaml);
    }
    catch (const YAML::Exception& error)
    {
        context.fail(fmt::format("not valid YAML: {}", error.what()));
        return false;
    }

    if (!root || root.IsNull())
    {
        return true;  // An empty file is every default.
    }
    if (!root.IsMap())
    {
        context.fail("the top level must be a mapping");
        return false;
    }

    // An unrecognised key is almost always a typo, and a typo that is ignored
    // looks exactly like a setting that does not work.
    static const std::vector<std::string> kKnown { "bind_address", "port", "asset_dir", "token_file" };
    for (const auto& entry : root)
    {
        const std::string key = entry.first.as<std::string>();
        if (std::find(kKnown.begin(), kKnown.end(), key) == kKnown.end())
        {
            context.fail(fmt::format("unknown key '{}'", key));
        }
    }

    const auto read_string = [&](const char* key, std::string& value)
    {
        if (const YAML::Node node = root[key])
        {
            if (!node.IsScalar())
            {
                context.fail(fmt::format("{}: expected a string", key));
                return;
            }
            value = node.Scalar();
        }
    };

    read_string("bind_address", out.bindAddress);
    read_string("asset_dir", out.assetDir);
    read_string("token_file", out.tokenFile);

    if (const YAML::Node node = root["port"])
    {
        try
        {
            const long long value = node.as<long long>();
            // Below 1024 needs privileges this service should not keep, and the
            // upper bound is the protocol's.
            if (value < 1024 || value > 65535)
            {
                context.fail(fmt::format("port: {} is outside 1024..65535", value));
            }
            else
            {
                out.port = static_cast<uint16_t>(value);
            }
        }
        catch (const YAML::Exception&)
        {
            context.fail(fmt::format("port: '{}' is not a whole number", node.Scalar()));
        }
    }

    if (out.bindAddress.empty())
    {
        context.fail("bind_address: cannot be empty; use 0.0.0.0 to listen on every interface");
    }

    // Expanded here rather than at every use, so everything downstream of this
    // function holds a path it can open.
    out.assetDir = core::paths::expand(out.assetDir);
    out.tokenFile = core::paths::expand(out.tokenFile);

    return context.ok;
}

bool load_node_config(const std::string& path, NodeConfig& out)
{
    std::ifstream in(path);
    if (!in)
    {
        SPDLOG_ERROR("[config] cannot read {}", path);
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parse_node_config(buffer.str(), out);
}

} // namespace web_console
