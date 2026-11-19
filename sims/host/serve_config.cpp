#include "host/serve_config.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include <spdlog/spdlog.h>
#include <yaml-cpp/yaml.h>

#include "node_config/reader.h"

namespace sim::sys {

namespace {

// node_config::rejectUnknownKeys for a key list known only at run time (the schema's), with the same wording.
void reject_unknown(const YAML::Node& node, const std::vector<std::string>& known, node_config::Context& context,
                    const std::string& where) {
    if (!node || !node.IsMap()) {
        return;
    }
    for (const auto& entry : node) {
        const std::string key = entry.first.as<std::string>();
        if (std::find(known.begin(), known.end(), key) == known.end()) {
            context.fail((where.empty() ? std::string("(top level)") : where) + ": unknown key '" + key + "'");
        }
    }
}

can_zenoh::BridgeConfig bridge_on(const ServeSchema& schema, const std::string& bus) {
    can_zenoh::BridgeConfig bridge;
    bridge.name = bus;
    bridge.device = "sim:" + schema.device;
    bridge.rxKey = "vehicle/" + bus + "/rx";
    bridge.txKey = "vehicle/" + bus + "/tx";
    bridge.recordGeneratedBy = "Redline " + schema.app;
    return bridge;
}

// One port's binding mapping, at `where` ("can" or "can.CAN1").
void read_binding(const YAML::Node& node, CanBinding& binding, node_config::Context& context, const std::string& where) {
    if (node.IsScalar() && node.as<std::string>() == "off") {
        binding.enabled = false;
        return;
    }
    if (!node.IsMap()) {
        context.fail(where + ": expected a mapping or 'off'");
        return;
    }
    binding.enabled = true;
    can_zenoh::BridgeConfig& bridge = binding.bridge;
    node_config::rejectUnknownKeys(node, {"name", "rx_key", "tx_key", "record_trc"}, context, where);
    node_config::readString(node, "name", bridge.name, context, where);
    // Keys follow the name unless given, as can_bridge's do.
    bridge.rxKey = "vehicle/" + bridge.name + "/rx";
    bridge.txKey = "vehicle/" + bridge.name + "/tx";
    node_config::readString(node, "rx_key", bridge.rxKey, context, where);
    node_config::readString(node, "tx_key", bridge.txKey, context, where);
    node_config::readString(node, "record_trc", bridge.recordTrcPath, context, where);
}

void check_binding(const CanBinding& binding, node_config::Context& context, const std::string& where) {
    if (!binding.enabled) {
        return;
    }
    const can_zenoh::BridgeConfig& bridge = binding.bridge;
    if (bridge.name.empty()) {
        context.fail(where + ".name: must not be empty");
    }
    if (bridge.rxKey == bridge.txKey) {
        context.fail(where + ": rx_key and tx_key are the same topic; the device would hear its own frames");
    }
    node_config::checkTopicKey(bridge.rxKey, where + ".rx_key", context);
    node_config::checkTopicKey(bridge.txKey, where + ".tx_key", context);
}

}  // namespace

ServeConfig default_serve_config(const ServeSchema& schema) {
    ServeConfig config;
    for (const CanPortSpec& spec : schema.ports) {
        CanBinding binding;
        binding.enabled = !spec.default_name.empty();
        binding.bridge = bridge_on(schema, spec.default_name.empty() ? spec.port : spec.default_name);
        config.can.push_back(binding);
    }
    return config;
}

std::optional<ServeConfig> parse_serve_config(const ServeSchema& schema, const std::string& yaml) {
    ServeConfig out = default_serve_config(schema);
    node_config::Context context;

    YAML::Node root;
    try {
        root = YAML::Load(yaml);
    } catch (const YAML::Exception& error) {
        SPDLOG_ERROR("[config] not valid YAML: {}", error.what());
        return std::nullopt;
    }
    if (root.IsNull()) {
        root = YAML::Node(YAML::NodeType::Map);
    }
    if (!root.IsMap()) {
        SPDLOG_ERROR("[config] the file is not a mapping");
        return std::nullopt;
    }

    std::vector<std::string> keys{"can", "stepped"};
    keys.insert(keys.end(), schema.device_keys.begin(), schema.device_keys.end());
    reject_unknown(root, keys, context, "");
    node_config::readBool(root, "stepped", out.stepped, context, "");
    for (const std::string& key : schema.device_keys) {
        if (root[key]) {
            std::string value;
            node_config::readString(root, key.c_str(), value, context, "");
            out.device[key] = value;
        }
    }

    if (const YAML::Node can = root["can"]) {
        if (schema.ports.size() == 1) {
            read_binding(can, out.can[0], context, "can");
        } else if (!can.IsMap()) {
            context.fail("can: expected a mapping of ports");
        } else {
            std::vector<std::string> port_names;
            for (const CanPortSpec& spec : schema.ports) {
                port_names.push_back(spec.port);
            }
            reject_unknown(can, port_names, context, "can");
            for (size_t k = 0; k < schema.ports.size(); k++) {
                if (const YAML::Node port = can[schema.ports[k].port]) {
                    out.can[k].bridge = bridge_on(schema, schema.ports[k].port);
                    read_binding(port, out.can[k], context, "can." + schema.ports[k].port);
                }
            }
        }
    }

    for (size_t k = 0; k < out.can.size(); k++) {
        check_binding(out.can[k], context, schema.ports.size() == 1 ? "can" : "can." + schema.ports[k].port);
        for (size_t j = 0; j < k; j++) {
            if (out.can[k].enabled && out.can[j].enabled && out.can[k].bridge.rxKey == out.can[j].bridge.rxKey) {
                context.fail("can: " + schema.ports[j].port + " and " + schema.ports[k].port +
                             " are on the same topics; two ports would be one bus");
            }
        }
    }

    if (!context.ok) {
        return std::nullopt;
    }
    return out;
}

std::optional<ServeConfig> load_serve_config(const ServeSchema& schema, const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        SPDLOG_ERROR("[config] cannot read '{}'", path);
        return std::nullopt;
    }
    std::stringstream text;
    text << in.rdbuf();
    return parse_serve_config(schema, text.str());
}

}  // namespace sim::sys
