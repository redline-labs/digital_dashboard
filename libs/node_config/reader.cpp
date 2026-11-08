// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config/reader.h"

#include <algorithm>

#include <spdlog/spdlog.h>

#include "pub_sub/topic_key.h"

namespace node_config
{

void Context::fail(const std::string& message)
{
    SPDLOG_ERROR("[config] {}", message);
    ok = false;
}

std::string fieldPath(const std::string& where, const char* key)
{
    if (where.empty())
    {
        return key;
    }
    if (where.back() == '.')
    {
        return where + key;
    }
    return where + "." + key;
}

void readString(const YAML::Node& parent, const char* key, std::string& out, Context& context,
                const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    if (!node.IsScalar())
    {
        context.fail(fieldPath(where, key) + " must be a string");
        return;
    }
    out = node.Scalar();
}

void readBool(const YAML::Node& parent, const char* key, bool& out, Context& context,
              const std::string& where)
{
    const YAML::Node node = parent[key];
    if (!node)
    {
        return;
    }
    try
    {
        out = node.as<bool>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(fieldPath(where, key) + " must be true or false");
    }
}

void checkTopicKey(const std::string& key, const std::string& field, Context& context)
{
    if (const std::string problem = pub_sub::topicKeyProblem(key); !problem.empty())
    {
        context.fail(field + " ('" + key + "'): " + problem);
    }
}

void rejectUnknownKeys(const YAML::Node& node, std::initializer_list<std::string_view> known, Context& context,
                       const std::string& where)
{
    if (!node || !node.IsMap())
    {
        return;
    }
    for (const auto& entry : node)
    {
        const std::string key = entry.first.as<std::string>();
        if (std::find(known.begin(), known.end(), key) == known.end())
        {
            context.fail((where.empty() ? std::string("(top level)") : where) + ": unknown key '" + key + "'");
        }
    }
}

}  // namespace node_config
