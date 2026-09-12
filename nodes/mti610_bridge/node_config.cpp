// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include <fstream>
#include <limits>

namespace mti610_node
{
namespace
{

// Accumulates problems instead of stopping at the first, so one run reports
// everything wrong with the file.
struct Context
{
    bool ok { true };

    void fail(const std::string& message)
    {
        SPDLOG_ERROR("[config] {}", message);
        ok = false;
    }
};

template <typename T>
void readUint(Context& context, const YAML::Node& node, const std::string& where, T& out)
{
    if (!node)
    {
        return;
    }

    try
    {
        const std::uint64_t value = node.as<std::uint64_t>();
        if (value > std::numeric_limits<T>::max())
        {
            context.fail(where + ": " + std::to_string(value) + " is out of range");
            return;
        }
        out = static_cast<T>(value);
    }
    catch (const YAML::Exception&)
    {
        context.fail(where + ": expected a number");
    }
}

void readString(Context& context, const YAML::Node& node, const std::string& where,
                std::string& out)
{
    if (!node)
    {
        return;
    }

    try
    {
        out = node.as<std::string>();
    }
    catch (const YAML::Exception&)
    {
        context.fail(where + ": expected a string");
    }
}

void readBool(Context& context, const YAML::Node& node, const std::string& where, bool& out)
{
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
        context.fail(where + ": expected true or false");
    }
}

// A top-level section must be a map. Without this check `device: [1, 2, 3]`
// parses clean: indexing a sequence by a string name yields an invalid node,
// every readString sees "absent" rather than "wrong", and the node starts with
// defaults for everything the file was trying to set.
bool sectionIsMap(Context& context, const YAML::Node& node, const std::string& where)
{
    if (!node.IsMap())
    {
        context.fail(where + ": expected a map");
        return false;
    }
    return true;
}

bool parseDataName(const std::string& name, xbus::DataId& out)
{
#define MTI610_MATCH_DATA(id, Name, snake, elements) \
    if (name == snake)                                \
    {                                                 \
        out = xbus::DataId::Name;                     \
        return true;                                  \
    }
    XBUS_DATA_TABLE(MTI610_MATCH_DATA)
#undef MTI610_MATCH_DATA

    return false;
}

bool parsePrecisionName(const std::string& name, xbus::Precision& out)
{
    if (name == "float32") { out = xbus::Precision::Float32; return true; }
    if (name == "fp1220")  { out = xbus::Precision::Fp1220;  return true; }
    if (name == "fp1632")  { out = xbus::Precision::Fp1632;  return true; }
    if (name == "float64") { out = xbus::Precision::Float64; return true; }
    return false;
}

void parseOutputs(Context& context, const YAML::Node& node, std::vector<OutputEntry>& out)
{
    if (!node)
    {
        return;
    }

    if (!node.IsSequence())
    {
        context.fail("configuration.outputs: expected a list");
        return;
    }

    out.clear();

    for (std::size_t i = 0; i < node.size(); ++i)
    {
        const std::string where = "configuration.outputs[" + std::to_string(i) + "]";
        const YAML::Node& item = node[i];

        if (!item.IsMap())
        {
            context.fail(where + ": expected a map with a 'data' key");
            continue;
        }

        OutputEntry entry;

        std::string dataName;
        readString(context, item["data"], where + ".data", dataName);

        if (dataName.empty())
        {
            context.fail(where + ": 'data' is required. One of: " + known_output_names());
            continue;
        }

        if (!parseDataName(dataName, entry.data))
        {
            context.fail(where + ".data: '" + dataName + "' is not an output an MTi-610 emits. "
                                 "One of: " + known_output_names());
            continue;
        }

        if (const YAML::Node precision = item["precision"])
        {
            std::string precisionName;
            readString(context, precision, where + ".precision", precisionName);

            if (!precisionName.empty() && !parsePrecisionName(precisionName, entry.precision))
            {
                context.fail(where + ".precision: '" + precisionName + "' is not a format. One of: " +
                             known_precision_names());
                continue;
            }
        }

        // The fixed-layout outputs have no format nibble: the LLCP gives them
        // a settable field but a payload defined as U2/U4 regardless. Asking
        // for a precision on one is a misunderstanding worth reporting rather
        // than a value to quietly drop.
        if (xbus::data_elements(entry.data) == 0)
        {
            if (item["precision"])
            {
                context.fail(where + ": '" + dataName +
                             "' has a fixed payload layout and takes no precision");
                continue;
            }
            entry.precision = xbus::Precision::Float32;
        }

        if (const YAML::Node rate = item["rate"])
        {
            std::string rateName;
            try
            {
                rateName = rate.as<std::string>();
            }
            catch (const YAML::Exception&)
            {
                context.fail(where + ".rate: expected a number of hertz, or 'max'");
                continue;
            }

            if (rateName == "max")
            {
                entry.frequencyHz = xbus::kMaxFrequency;
            }
            else
            {
                readUint(context, rate, where + ".rate", entry.frequencyHz);
            }
        }

        out.push_back(entry);
    }

    if (out.size() > xbus::kMaxOutputEntries)
    {
        context.fail("configuration.outputs: " + std::to_string(out.size()) +
                     " entries, but one SetOutputConfiguration carries at most " +
                     std::to_string(xbus::kMaxOutputEntries));
    }
}

} // namespace

std::uint16_t OutputEntry::rawDataId() const
{
    return static_cast<std::uint16_t>(static_cast<std::uint16_t>(data) |
                                      static_cast<std::uint16_t>(precision));
}

std::string known_output_names()
{
    std::string out;
#define MTI610_NAME_DATA(id, Name, snake, elements) \
    if (!out.empty()) { out += ", "; }               \
    out += snake;
    XBUS_DATA_TABLE(MTI610_NAME_DATA)
#undef MTI610_NAME_DATA
    return out;
}

std::string known_precision_names()
{
    return "float32, fp1220, fp1632, float64";
}

std::vector<mti610::OutputEntry> to_output_entries(const std::vector<OutputEntry>& outputs)
{
    std::vector<mti610::OutputEntry> entries;
    entries.reserve(outputs.size());

    for (const OutputEntry& entry : outputs)
    {
        entries.push_back(mti610::OutputEntry { entry.rawDataId(), entry.frequencyHz });
    }

    return entries;
}

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
        SPDLOG_ERROR("[config] not valid YAML: {}", error.what());
        return false;
    }

    if (!root || !root.IsMap())
    {
        SPDLOG_ERROR("[config] expected a map at the top level");
        return false;
    }

    if (const YAML::Node device = root["device"];
        device && sectionIsMap(context, device, "device"))
    {
        readString(context, device["port"], "device.port", out.device.port);
        readUint(context, device["baud"], "device.baud", out.device.baud);
        readUint(context, device["open_timeout_ms"], "device.open_timeout_ms",
                 out.device.openTimeoutMs);

        if (const YAML::Node backoff = device["reopen_backoff_ms"])
        {
            if (!backoff.IsSequence())
            {
                context.fail("device.reopen_backoff_ms: expected a list of milliseconds");
            }
            else
            {
                out.device.reopenBackoffMs.clear();
                for (std::size_t i = 0; i < backoff.size(); ++i)
                {
                    std::uint32_t value = 0;
                    readUint(context, backoff[i],
                             "device.reopen_backoff_ms[" + std::to_string(i) + "]", value);
                    out.device.reopenBackoffMs.push_back(value);
                }
            }
        }
    }

    if (const YAML::Node configuration = root["configuration"];
        configuration && sectionIsMap(context, configuration, "configuration"))
    {
        if (const YAML::Node mode = configuration["mode"])
        {
            std::string name;
            readString(context, mode, "configuration.mode", name);
            if (name == "enforce")
            {
                out.configuration.mode = mti610::ConfigMode::Enforce;
            }
            else if (name == "report_only")
            {
                out.configuration.mode = mti610::ConfigMode::ReportOnly;
            }
            else if (!name.empty())
            {
                context.fail("configuration.mode: '" + name +
                             "' is not a mode. One of: enforce, report_only");
            }
        }

        if (const YAML::Node policy = configuration["port_policy"])
        {
            std::string name;
            readString(context, policy, "configuration.port_policy", name);
            if (name == "additive")
            {
                out.configuration.policy = mti610::PortPolicy::Additive;
            }
            else if (name == "exclusive")
            {
                out.configuration.policy = mti610::PortPolicy::Exclusive;
            }
            else if (!name.empty())
            {
                context.fail("configuration.port_policy: '" + name +
                             "' is not a policy. One of: additive, exclusive");
            }
        }

        readUint(context, configuration["reply_timeout_ms"], "configuration.reply_timeout_ms",
                 out.configuration.replyTimeoutMs);
        readUint(context, configuration["retries"], "configuration.retries",
                 out.configuration.retries);
        readUint(context, configuration["recheck_interval_s"], "configuration.recheck_interval_s",
                 out.configuration.recheckIntervalS);

        parseOutputs(context, configuration["outputs"], out.configuration.outputs);
    }

    if (const YAML::Node publish = root["publish"];
        publish && sectionIsMap(context, publish, "publish"))
    {
        readString(context, publish["topic_prefix"], "publish.topic_prefix",
                   out.publish.topicPrefix);
        readString(context, publish["status_key"], "publish.status_key", out.publish.statusKey);
        readUint(context, publish["status_interval_ms"], "publish.status_interval_ms",
                 out.publish.statusIntervalMs);
        readBool(context, publish["publish_unknown_items"], "publish.publish_unknown_items",
                 out.publish.publishUnknownItems);
    }

    return context.ok;
}

bool load_node_config(const std::string& path, NodeConfig& out)
{
    std::ifstream file(path);
    if (!file)
    {
        SPDLOG_ERROR("[config] cannot open {}", path);
        return false;
    }

    const std::string yaml((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());

    return parse_node_config(yaml, out);
}

} // namespace mti610_node
