// Names for everything an agent, a scenario or a test reads or sets:
//
//   t                         sim time, seconds
//   pc a x h hx sp ccr        CPU registers
//   out[N].pin .v .i .tj .shutdown .load .status   output N (1-32): gate, volts, amps, junction degC, thermal
//                             shutdown, load ohms (inf: open), firmware status byte
//   in[N].v  in[N].raw        input N (1-23): connector volts; the firmware's reading (0.2 V/bit)
//   battery                   battery volts
//   can:SIGNAL  can_age:SIGNAL   the latest decoded value of a DBC signal from the PDM's stream; its age in seconds
//   SYMBOL  SYMBOL[k]  SYMBOL.w   a byte (word, big-endian) of a firmware symbol (pdm_v191.symbols.json)
//   $0100  0x0100             a byte at an address
//   DEVICE.REG                a device register without side effects (mscan.CTFLG, tim1.TSC, adc.ADRL, ...)
//
// Settable: in[N].v (a number, or "open"), out[N].load ("open", "short", "lamp:R", ohms), battery, and (allow_write)
// RAM bytes by symbol or address.
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "pdm32/top/instance.h"

namespace sim::sys {

// The value of a name, or nothing with `error` set.
std::optional<double> read_name(Instance& instance, const std::string& name, std::string& error);

// The address of a memory name (SYMBOL, SYMBOL[k], $0100, 0x0100), or nothing.
std::optional<uint16_t> address_of(const Instance& instance, const std::string& name);

// Sets a name. Connector names always; memory only with `allow_memory`. Returns "" or the error.
std::string set_name(Instance& instance, const std::string& name, const nlohmann::json& value, bool allow_memory);

// Parses "0x1A", "$1A", "26" or a JSON number.
std::optional<double> parse_number(const nlohmann::json& value);

}  // namespace sim::sys
