// Names for everything an agent, a scenario or a test reads or sets: the shared ones (host/debug/names.h: t, the
// CPU's registers, SYMBOL, SYMBOL[k], SYMBOL.w, $addr) and the PDM's own (pdm32/top/target.h: battery, out[N].*,
// in[N].*, can:SIGNAL, DEVICE.REG).
//
// Settable: in[N].v (a number, or "open"), out[N].load ("open", "short", "lamp:R", ohms), battery, and (allow_memory)
// RAM by symbol or address.
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "pdm32/top/instance.h"

namespace sim::sys {

// The value of a name, or nothing with `error` set.
std::optional<double> read_name(Instance& instance, const std::string& name, std::string& error);

// Sets a name. Connector names always; memory only with `allow_memory`. Returns "" or the error.
std::string set_name(Instance& instance, const std::string& name, const nlohmann::json& value, bool allow_memory);

}  // namespace sim::sys
