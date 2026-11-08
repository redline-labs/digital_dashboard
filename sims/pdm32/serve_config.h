// What `pdm32_sim serve` reads from -c: where the PDM's bus goes, and what the PDM is running.
//
//   can:                       the PDM's CAN port, on the same topics can_bridge would give the real one
//     name: can0               -> vehicle/can0/rx and vehicle/can0/tx unless given
//     rx_key: vehicle/can0/rx
//     tx_key: vehicle/can0/tx
//     record_trc: run.trc      both directions, as can_bridge records
//   scenario: sims/pdm32/scenarios/short_output.json   loads, inputs, configuration and timed events
//   firmware: /path/to/pdm.hex                           default $PDM_FIRMWARE_HEX
//   engine: jit                                          interp | block | jit
//   stepped: false             true: time moves only when an agent says (run, step); false: real time
//
// Every key is optional, and an unknown one is an error rather than a setting silently left at its default.
#pragma once

#include <optional>
#include <string>

#include "can_zenoh/bridge_config.h"

namespace pdm32 {

struct ServeConfig {
    can_zenoh::BridgeConfig can;
    std::string scenario;
    std::string firmware;
    std::string engine;
    bool stepped = false;
};

// The defaults: the bus named can0 on can_bridge's default keys.
ServeConfig default_serve_config();

// nullopt, having logged every problem, when the YAML is not a valid config.
std::optional<ServeConfig> parse_serve_config(const std::string& yaml);
std::optional<ServeConfig> load_serve_config(const std::string& path);

}  // namespace pdm32
