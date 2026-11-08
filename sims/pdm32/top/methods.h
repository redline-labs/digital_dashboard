// The agent methods: the session (hello, status, run, step, finish, reset), the connector (inputs, outputs,
// input.set, load.set, battery.set, set), CAN (can.send, can.recv, signals), the debugger (break, watch, clear, list,
// records, read, write, names, backtrace, disasm, memory) and snapshots.
#pragma once

#include <functional>

#include "agent_control/dispatcher.h"
#include "pdm32/top/control.h"
#include "pdm32/top/sim_session.h"

namespace sim::sys {

struct ServerHooks {
    std::function<void()> quit;
    std::function<json(bool pause)> pause_continue;  // realtime only (null in stepped mode)
    bool realtime = false;
};

void register_methods(agent_control::Dispatcher& dispatcher, SimSession& session, ServerHooks hooks);

}  // namespace sim::sys
