// What a simulator's `serve` reads from -c: where each of the device's CAN ports goes on zenoh, the time mode, and the
// device's own settings.
//
// A device with one CAN port binds it with a mapping, as can_bridge's channels are configured:
//
//   can:
//     name: can0               -> vehicle/can0/rx and vehicle/can0/tx unless given
//     rx_key: vehicle/can0/rx  the device's frames, as can_bridge publishes a bus
//     tx_key: vehicle/can0/tx  frames published here reach the device
//     record_trc: run.trc      both directions, as can_bridge records
//
// A device with several names each port; a port can be left off the bus:
//
//   can:
//     CAN1: {name: can0}
//     CAN2: {name: can1}
//     CAN3: off
//
//   stepped: false             true: time moves only when an agent says (run, step); false: real time
//
// and the device's own keys (scenario, firmware, engine, ...) as text. Every key is optional; an unknown one is an
// error rather than a setting silently left at its default.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "can_zenoh/bridge_config.h"

namespace sim::sys {

// A device's CAN port and where it goes when the config says nothing.
struct CanPortSpec {
    std::string port;          // "CAN", "CAN1"
    std::string default_name;  // the bus it is on by default ("can0"); empty: off the bus by default
};

// What a device's config may contain.
struct ServeSchema {
    std::string app;    // "pdm32_sim": names the channel's device ("sim:pdm32") and the trace's generator
    std::string device;  // "pdm32"
    std::vector<CanPortSpec> ports;
    std::vector<std::string> device_keys;  // the device's own top-level keys
};

struct CanBinding {
    bool enabled = true;
    can_zenoh::BridgeConfig bridge;
};

struct ServeConfig {
    std::vector<CanBinding> can;                // one per port, in the schema's order
    bool stepped = false;
    std::map<std::string, std::string> device;  // the device keys that were given

    std::string device_value(const std::string& key, const std::string& fallback = {}) const {
        const auto it = device.find(key);
        return it == device.end() ? fallback : it->second;
    }
};

// The defaults: each port on its default bus, on can_bridge's keys for it.
ServeConfig default_serve_config(const ServeSchema& schema);
// nullopt, having logged every problem, when the YAML is not a valid config for the schema.
std::optional<ServeConfig> parse_serve_config(const ServeSchema& schema, const std::string& yaml);
std::optional<ServeConfig> load_serve_config(const ServeSchema& schema, const std::string& path);

}  // namespace sim::sys
