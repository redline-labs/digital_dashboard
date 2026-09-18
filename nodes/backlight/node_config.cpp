#include "node_config.h"

#include "pub_sub/topic_key.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace backlight_node
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
    static const std::vector<std::string> kKnown { "role", "record_dir", "topic_prefix", "poll_ms", "min_percent",
                                                   "light_integration_time" };
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

    read_string("role", out.role);
    read_string("record_dir", out.recordDir);
    read_string("topic_prefix", out.topicPrefix);

    if (const YAML::Node node = root["poll_ms"])
    {
        try
        {
            const long long value = node.as<long long>();
            if (value < 50 || value > 60000)
            {
                context.fail(fmt::format("poll_ms: {} is outside 50..60000", value));
            }
            else
            {
                out.pollMs = static_cast<uint32_t>(value);
            }
        }
        catch (const YAML::Exception&)
        {
            context.fail(fmt::format("poll_ms: '{}' is not a whole number", node.Scalar()));
        }
    }

    if (const YAML::Node node = root["min_percent"])
    {
        try
        {
            const double value = node.as<double>();
            if (!std::isfinite(value) || value < 0.0 || value > 100.0)
            {
                context.fail(fmt::format("min_percent: {} is outside 0..100", node.Scalar()));
            }
            else
            {
                out.minPercent = value;
            }
        }
        catch (const YAML::Exception&)
        {
            context.fail(fmt::format("min_percent: '{}' is not a number", node.Scalar()));
        }
    }

    if (const YAML::Node node = root["light_integration_time"])
    {
        try
        {
            const double value = node.as<double>();
            if (!std::isfinite(value) || value < 0.0 || value > 10.0)
            {
                context.fail(fmt::format("light_integration_time: {} is outside 0..10 s", node.Scalar()));
            }
            else
            {
                out.lightIntegrationTime = value;
            }
        }
        catch (const YAML::Exception&)
        {
            context.fail(fmt::format("light_integration_time: '{}' is not a number", node.Scalar()));
        }
    }

    // The role names the record file, so anything else would look for a file
    // the rootfs never writes and report "no display" forever.
    if (out.role != "primary" && out.role != "secondary")
    {
        context.fail(fmt::format("role: '{}' is not primary or secondary", out.role));
    }
    if (out.recordDir.empty())
    {
        context.fail("record_dir: cannot be empty");
    }
    if (!out.topicPrefix.empty())
    {
        if (const std::string problem = pub_sub::topicKeyProblem(out.topicPrefix); !problem.empty())
        {
            context.fail(fmt::format("topic_prefix: '{}' {}", out.topicPrefix, problem));
        }
    }

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

} // namespace backlight_node
