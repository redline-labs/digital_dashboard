// The names every simulator's agent methods, conditions and record actions read and set:
//
//   t                    simulated time, seconds
//   pc sp r3 lr ...      the CPU's registers, by their assembler names (DebugTarget::registers lists them)
//   SYMBOL  SYMBOL[k]    a byte of a firmware symbol, k bytes on
//   SYMBOL.w .l .f       a 16-bit, 32-bit or single-precision value there (big-endian, as both CPUs are)
//   $0100  0x0100        a byte at an address (and .w .l .f after it)
//
// and the device's own (DebugTarget::read_device_name): a connector's pins, decoded CAN signals, device registers.
// Memory is set only when the caller allows it; a register likewise.
#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "host/debug/target.h"

namespace sim::sys {

// A memory name resolved: where, and how wide (1, 2, 4; `real`: a float).
struct MemoryName {
    uint64_t address = 0;
    unsigned size = 1;
    bool real = false;
};

std::optional<MemoryName> memory_name(const DebugTarget& target, const std::string& name);

// The value of a name, or nothing with `error` set.
std::optional<double> read_name(DebugTarget& target, const std::string& name, std::string& error);

// Sets a name: the device's always; registers and memory only with `allow_write`. Returns "" or the error.
std::string set_name(DebugTarget& target, const std::string& name, const nlohmann::json& value, bool allow_write);

}  // namespace sim::sys
