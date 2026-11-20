// The agent methods every simulator has, over its DebugSession:
//
//   session    hello, run, step, finish, reset, quit, pause, continue
//   debugger   break, watch, clear, list, records, read, write, set, names, backtrace, disasm, memory, registers
//   snapshots  snapshot, restore, snapshots
//
// The simulator adds its own (status, a connector, a console, CAN) beside them; `status` is the device's, and run's
// and reset's answers come from it too.
#pragma once

#include <functional>
#include <string>

#include <nlohmann/json.hpp>

#include "agent_control/dispatcher.h"
#include "host/debug/session.h"
#include "host/served_device.h"

namespace sim::sys {

struct DebugMethodsOptions {
    std::string app;                         // "pdm32_sim"
    std::function<nlohmann::json()> info;    // hello's device part: firmware or image, engine
    std::function<nlohmann::json()> status;  // the device's status (reset answers with it)
    std::function<void()> reset;             // a power-on reset of the whole device (the target's by default)
    bool allow_write = false;                // memory and register writes
};

void register_debug_methods(agent_control::Dispatcher& dispatcher, DebugSession& session, const ServerHooks& hooks,
                            DebugMethodsOptions options);

}  // namespace sim::sys
