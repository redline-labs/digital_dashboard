// The agent methods: the shared debugger's (host/debug/methods.h: hello, run, step, finish, reset, quit, pause,
// continue, break, watch, clear, list, records, read, write, set, names, backtrace, registers, disasm, memory,
// snapshots), and the PDM's own: status, the connector (inputs, outputs, pins, input.set, load.set, battery.set) and
// CAN (can.send, can.recv, signals).
#pragma once

#include "agent_control/dispatcher.h"
#include "host/control.h"
#include "host/served_device.h"
#include "pdm32/top/sim_session.h"

namespace sim::sys {

void register_methods(agent_control::Dispatcher& dispatcher, SimSession& session, ServerHooks hooks);

}  // namespace sim::sys
