// SPDX-License-Identifier: GPL-3.0-or-later

#include "node_config.h"

#include <yaml-cpp/yaml.h>

#include <spdlog/spdlog.h>

#include <fstream>
#include <iterator>
#include <limits>

#include "gsof/record_table.h"

namespace bd992_node
{

const char* to_string(ConfigMode mode)
{
    switch (mode)
    {
        case ConfigMode::ReportOnly: return "report_only";
        case ConfigMode::Enforce:    return "enforce";
    }

    return "unknown";
}

namespace
{

// Accumulates problems rather than failing on the first, so a config with
// three mistakes takes one run to fix rather than three.
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
void readUint(const YAML::Node& parent, const char* key, T& out, Context& context, const std::string& where)
{
    if (!parent[key])
    {
        return;
    }

    try
    {
        const auto value = parent[key].as<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<T>::max()))
        {
            context.fail(where + "." + key + ": " + std::to_string(value) + " is out of range");
            return;
        }
        out = static_cast<T>(value);
    }
    catch (const YAML::Exception& e)
    {
        context.fail(where + "." + key + ": " + e.what());
    }
}

void readString(const YAML::Node& parent, const char* key, std::string& out, Context& context,
                const std::string& where)
{
    if (!parent[key])
    {
        return;
    }

    try
    {
        out = parent[key].as<std::string>();
    }
    catch (const YAML::Exception& e)
    {
        context.fail(where + "." + key + ": " + e.what());
    }
}

void readBool(const YAML::Node& parent, const char* key, bool& out, Context& context, const std::string& where)
{
    if (!parent[key])
    {
        return;
    }

    try
    {
        out = parent[key].as<bool>();
    }
    catch (const YAML::Exception& e)
    {
        context.fail(where + "." + key + ": " + e.what());
    }
}

// Every record name the build knows, for an error message that says what the
// valid values are rather than merely that this one was not.
std::string knownRecordNames()
{
    std::string out;
#define GSOF_RECORD_LIST(id, Name, snake) \
    out += out.empty() ? "" : ", ";       \
    out += snake;
    GSOF_RECORD_TABLE(GSOF_RECORD_LIST)
#undef GSOF_RECORD_LIST
    return out;
}

bool recordFromName(const std::string& name, gsof::RecordType& out)
{
#define GSOF_RECORD_MATCH(id, Name, snake)  \
    if (name == snake)                      \
    {                                       \
        out = gsof::RecordType::Name;       \
        return true;                        \
    }
    GSOF_RECORD_TABLE(GSOF_RECORD_MATCH)
#undef GSOF_RECORD_MATCH

    return false;
}

// The frequency table is not ordered and not contiguous, so the names are the
// only sane way to write one in a config file. Matched against the same
// to_string() the status message uses, so the two cannot disagree.
constexpr gsof::appfile::Frequency kFrequencies[] = {
    gsof::appfile::Frequency::Off,            gsof::appfile::Frequency::Hz100,
    gsof::appfile::Frequency::Hz50,           gsof::appfile::Frequency::Hz20,
    gsof::appfile::Frequency::Hz10,           gsof::appfile::Frequency::Hz5,
    gsof::appfile::Frequency::Hz2,            gsof::appfile::Frequency::Hz1,
    gsof::appfile::Frequency::Every2Seconds,  gsof::appfile::Frequency::Every5Seconds,
    gsof::appfile::Frequency::Every10Seconds, gsof::appfile::Frequency::Every15Seconds,
    gsof::appfile::Frequency::Every30Seconds, gsof::appfile::Frequency::Every60Seconds,
    gsof::appfile::Frequency::Every5Minutes,  gsof::appfile::Frequency::Every10Minutes,
    gsof::appfile::Frequency::OnceImmediately,
};

std::string knownRateNames()
{
    std::string out;
    for (const gsof::appfile::Frequency rate : kFrequencies)
    {
        out += out.empty() ? "" : ", ";
        out += gsof::appfile::to_string(rate);
    }
    return out;
}

bool rateFromName(const std::string& name, gsof::appfile::Frequency& out)
{
    for (const gsof::appfile::Frequency rate : kFrequencies)
    {
        if (name == gsof::appfile::to_string(rate))
        {
            out = rate;
            return true;
        }
    }
    return false;
}

void parseReceiver(const YAML::Node& node, ReceiverConfig& out, Context& context)
{
    if (!node)
    {
        context.fail("receiver: section is required");
        return;
    }

    readString(node, "host", out.host, context, "receiver");
    readUint(node, "stream_port", out.streamPort, context, "receiver");
    readUint(node, "control_port", out.controlPort, context, "receiver");
    readUint(node, "connect_timeout_ms", out.connectTimeoutMs, context, "receiver");

    if (node["reconnect_backoff_ms"])
    {
        try
        {
            std::vector<std::uint32_t> backoff;
            for (const YAML::Node& entry : node["reconnect_backoff_ms"])
            {
                backoff.push_back(entry.as<std::uint32_t>());
            }
            if (backoff.empty())
            {
                context.fail("receiver.reconnect_backoff_ms: must not be empty");
            }
            else
            {
                out.reconnectBackoffMs = std::move(backoff);
            }
        }
        catch (const YAML::Exception& e)
        {
            context.fail(std::string("receiver.reconnect_backoff_ms: ") + e.what());
        }
    }

    // host is NOT required here: --replay runs the whole decode and publish
    // path with no receiver, and demanding a host it will never connect to
    // would mean a placeholder in the replay config that reads like a mistake.
    // main() requires it when it is actually about to connect.
    if (out.streamPort == 0)
    {
        context.fail("receiver.stream_port: must not be zero");
    }
}

void parseConfiguration(const YAML::Node& node, ConfigurationConfig& out, Context& context)
{
    if (!node)
    {
        // Entirely optional: a node that only reads is a legitimate setup, and
        // it is what --replay uses.
        return;
    }

    if (node["mode"])
    {
        const auto mode = node["mode"].as<std::string>("");
        if (mode == "enforce")
        {
            out.mode = ConfigMode::Enforce;
        }
        else if (mode == "report_only")
        {
            out.mode = ConfigMode::ReportOnly;
        }
        else
        {
            context.fail("configuration.mode: '" + mode + "' is not one of enforce, report_only");
        }
    }

    if (node["port_policy"])
    {
        const auto policy = node["port_policy"].as<std::string>("");
        if (policy == "additive")
        {
            out.portPolicy = bd992::PortPolicy::Additive;
        }
        else if (policy == "exclusive")
        {
            out.portPolicy = bd992::PortPolicy::Exclusive;
        }
        else
        {
            context.fail("configuration.port_policy: '" + policy + "' is not one of additive, exclusive");
        }
    }

    readUint(node, "port_index", out.portIndex, context, "configuration");
    readUint(node, "appfile_index", out.applicationFileIndex, context, "configuration");
    readUint(node, "recheck_interval_s", out.recheckIntervalS, context, "configuration");
    readUint(node, "reply_timeout_ms", out.replyTimeoutMs, context, "configuration");
    readBool(node, "allow_raw_commands", out.allowRawCommands, context, "configuration");

    if (!node["outputs"])
    {
        return;
    }

    std::size_t index = 0;
    for (const YAML::Node& entry : node["outputs"])
    {
        const std::string where = "configuration.outputs[" + std::to_string(index) + "]";
        ++index;

        OutputEntry output;
        // Tracked per entry: an entry that did not parse must not reach the
        // list, or it lands there with the default record and the duplicate
        // check below then reports a collision that is not in the file.
        bool entryOk = true;

        const auto recordName = entry["record"] ? entry["record"].as<std::string>("") : std::string();
        if (recordName.empty())
        {
            context.fail(where + ".record: required");
            entryOk = false;
        }
        else if (!recordFromName(recordName, output.record))
        {
            context.fail(where + ".record: '" + recordName + "' is not a known GSOF record. Known: " +
                         knownRecordNames());
            entryOk = false;
        }

        const auto rateName = entry["rate"] ? entry["rate"].as<std::string>("") : std::string();
        if (rateName.empty())
        {
            context.fail(where + ".rate: required");
            entryOk = false;
        }
        else if (!rateFromName(rateName, output.rate))
        {
            context.fail(where + ".rate: '" + rateName + "' is not a known rate. Known: " + knownRateNames());
            entryOk = false;
        }

        readUint(entry, "offset_seconds", output.offsetSeconds, context, where);

        if (entryOk)
        {
            out.outputs.push_back(output);
        }
    }

    // Two entries for the same record on the same port are a contradiction:
    // whichever is written last wins, silently.
    for (std::size_t i = 0; i < out.outputs.size(); ++i)
    {
        for (std::size_t j = i + 1; j < out.outputs.size(); ++j)
        {
            if (out.outputs[i].record == out.outputs[j].record)
            {
                context.fail(std::string("configuration.outputs: '") +
                             gsof::record_name(out.outputs[i].record) + "' is listed twice");
            }
        }
    }
}

void parsePublish(const YAML::Node& node, PublishConfig& out, Context& context)
{
    if (!node)
    {
        return;
    }

    readString(node, "topic_prefix", out.topicPrefix, context, "publish");
    readString(node, "status_key", out.statusKey, context, "publish");
    readUint(node, "status_interval_ms", out.statusIntervalMs, context, "publish");
    readBool(node, "publish_unknown_records", out.publishUnknownRecords, context, "publish");
}

} // namespace

bool parse_node_config(const std::string& yaml, NodeConfig& out)
{
    Context context;

    YAML::Node root;
    try
    {
        root = YAML::Load(yaml);
    }
    catch (const YAML::Exception& e)
    {
        SPDLOG_ERROR("[config] cannot parse: {}", e.what());
        return false;
    }

    if (!root || !root.IsMap())
    {
        SPDLOG_ERROR("[config] the top level must be a mapping");
        return false;
    }

    parseReceiver(root["receiver"], out.receiver, context);
    parseConfiguration(root["configuration"], out.configuration, context);
    parsePublish(root["publish"], out.publish, context);

    return context.ok;
}

bool load_node_config(const std::string& path, NodeConfig& out)
{
    // Read the file as text and hand it to the string-taking parser, so that
    // parser is the only implementation and the tests exercise the real one.
    std::ifstream file(path);
    if (!file)
    {
        SPDLOG_ERROR("[config] cannot open {}", path);
        return false;
    }

    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse_node_config(text, out);
}

std::vector<bd992::OutputMessage> desired_outputs(const ConfigurationConfig& config)
{
    std::vector<bd992::OutputMessage> outputs;
    outputs.reserve(config.outputs.size());

    for (const OutputEntry& entry : config.outputs)
    {
        outputs.push_back(bd992::gsof_output(static_cast<gsof::appfile::PortIndex>(config.portIndex),
                                             entry.record, entry.rate, entry.offsetSeconds));
    }

    return outputs;
}

} // namespace bd992_node
