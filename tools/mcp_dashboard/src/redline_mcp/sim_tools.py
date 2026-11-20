"""Typed tools for the simulators under sims/: pdm32_sim and m150_sim.

Each is a thin wrapper over one of the simulator's agent methods (app_methods
lists them all; app_call reaches any without a wrapper here). Every simulator
has the same debugger (sims/host/debug): run, step, finish, breakpoints, watches,
read/write by name, registers, disassembly, backtrace, memory, snapshots. The
connector tools are the PDM's own; the console is the M150's.

The tools act on the simulator that is running. A simulator is launched like
anything else -- app_launch(app="pdm32_sim", ...) -- and at most one of each
runs: its CAN ports are bus topics, and a second device on vehicle/can0 would be
two devices answering as one. With both simulators running, the tools ask which
(app_call reaches either by name).
"""

from __future__ import annotations

import json
from typing import Annotated, Any, Literal

from pydantic import Field

from .client import AgentError
from .supervisor import LaunchError

SIMULATORS = ("pdm32_sim", "m150_sim")

# The server's _call, _fail and the running apps, set by register(): this module is imported by it, not the other
# way round.
_server: dict[str, Any] = {}


def target() -> str:
    """The simulator the tools act on: the one running."""
    running = [app for app in _server["running"]() if app in SIMULATORS]
    if not running:
        raise LaunchError(
            f"No simulator is running ({', '.join(SIMULATORS)}). Call app_launch(app='pdm32_sim' or 'm150_sim') first."
        )
    if len(running) > 1:
        raise LaunchError(
            f"Both {' and '.join(sorted(running))} are running; the sim_* tools need one. "
            "Quit one, or use app_call(app=..., method=...)."
        )
    return running[0]


def sim(method: str, params: dict[str, Any] | None = None, timeout_ms: int | None = None) -> str:
    body = {k: v for k, v in (params or {}).items() if v is not None}
    if timeout_ms is not None:
        body["_timeout_ms"] = timeout_ms
    try:
        return json.dumps(_server["call"](target(), method, body), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _server["fail"](exc)


def sim_status() -> str:
    """The simulated device: sim time, pc and function, and its own state (the PDM: CPU and firmware run mode,
    resets, CAN counters; the M150: MSR, engine counters, each CAN controller's bit rate and frames)."""
    return sim("status")


def sim_run(
    seconds: Annotated[float | None, Field(ge=0, description="Advance by this much simulated time.")] = None,
    until: Annotated[float | None, Field(ge=0, description="Or advance to this simulated time (s).")] = None,
) -> str:
    """Advance time, in stepped mode only (launched with extra_args=["--stepped"]). Stops early at a breakpoint
    or a stopping watch and says why: reason (time/break/watch), t, pc, where. In real time the clock runs by
    itself; use sim_pause / sim_continue instead."""
    # A long run is the simulator working, not hanging: give it the time it asked for, and then some.
    budget = int(((seconds or until or 0) * 2 + 10) * 1000)
    return sim("run", {"seconds": seconds, "until": until}, timeout_ms=budget)


def sim_step(
    count: Annotated[int, Field(ge=1, le=100000, description="Instructions to execute.")] = 1,
    over: Annotated[bool, Field(description="Run a call (JSR/BSR; bl) through to its return.")] = False,
) -> str:
    """Execute instructions one at a time (stepped mode)."""
    return sim("step", {"count": count, "over": over})


def sim_finish() -> str:
    """Run until the current function returns (stepped mode); stops early at a breakpoint or watch."""
    return sim("finish", timeout_ms=60_000)


def sim_pause() -> str:
    """Real time: stop the clock. Agent calls still answer."""
    return sim("pause")


def sim_continue() -> str:
    """Real time: restart the clock, from where it stopped (a breakpoint or watch also stops it)."""
    return sim("continue")


def sim_reset() -> str:
    """Power-on reset of the device (what is wired to it, breakpoints and watches stay)."""
    return sim("reset")


def sim_set_input(
    input: Annotated[int, Field(ge=1, le=23, description="Input number at the connector.")],
    volts: Annotated[
        float | Literal["open"],
        Field(description="Volts driven on the pin, or 'open' (the PDM's pull-up to battery)."),
    ],
) -> str:
    """PDM32: drive (or open) an input pin."""
    return sim("input.set", {"input": input, "volts": volts})


def sim_set_load(
    output: Annotated[int, Field(ge=1, le=32, description="Output number at the connector.")],
    load: Annotated[str, Field(description="'open', 'short', 'lamp:<hot ohms>' or ohms ('6').")],
) -> str:
    """PDM32: change what is wired to an output: a short to ground, an open circuit, a resistor, a lamp."""
    return sim("load.set", {"output": output, "load": load})


def sim_set_battery(volts: Annotated[float, Field(ge=0, le=40)]) -> str:
    """PDM32: the battery voltage."""
    return sim("battery.set", {"volts": volts})


def sim_outputs() -> str:
    """PDM32: every output: pin, volts, amps, junction temperature, thermal shutdown, load, firmware status."""
    return sim("outputs")


def sim_inputs() -> str:
    """PDM32: every input: connector volts, open or driven, the firmware's 8-bit reading."""
    return sim("inputs")


def sim_read(
    names: Annotated[
        list[str],
        Field(
            description=(
                "t; a register by its assembler name (pc, sp; PDM a x hx ccr; M150 r0..r31 lr ctr cr msr); "
                "a firmware symbol SYMBOL, SYMBOL[k], SYMBOL.w/.l/.f (16/32-bit, float); $addr; and the device's "
                "own: PDM out[N].pin/v/i/tj/load/status, in[N].v/raw, can:SIGNAL, dev.REG; "
                "M150 can1.bit_rate/sent/received, interrupts, waiting."
            )
        ),
    ],
) -> str:
    """Values by name; an unknown name comes back null with the reason under errors."""
    return sim("read", {"names": names})


def sim_write(
    name: Annotated[
        str,
        Field(description="The device's settable names (PDM in[N].v, out[N].load, battery), or with --allow-write "
                          "a memory name or a register."),
    ],
    value: Annotated[Any, Field(description="A number, or 'open'/'short'/'lamp:R' where it applies.")],
) -> str:
    """Set a name."""
    return sim("write", {"name": name, "value": value})


def sim_can_send(
    id: Annotated[int, Field(ge=0, le=0x1FFFFFFF, description="Identifier; above 0x7FF is extended.")],
    data: Annotated[str, Field(description="Up to 8 bytes as hex, e.g. '0102A0'.")] = "",
    port: Annotated[str | None, Field(description="M150: CAN1, CAN2 or CAN3 (default CAN1).")] = None,
) -> str:
    """Put a frame on the device's bus directly, at the current simulated instant. (Publishing on the tx topic --
    vehicle/can0/tx by default -- does the same from the bus side.)"""
    return sim("can.send", {"id": id, "data": data, "port": port})


def sim_can_recv(
    since: Annotated[int, Field(ge=0, description="First frame index (the previous call's 'next').")] = 0,
    limit: Annotated[int, Field(ge=1, le=10000)] = 100,
    id: Annotated[int | None, Field(description="Only this identifier.")] = None,
) -> str:
    """Frames logged on the device's buses, both directions (t, id, data; the PDM's from_pdm, the M150's port and
    from_ecu), and the next index."""
    return sim("can.recv", {"since": since, "limit": limit, "id": id})


def sim_signals(filter: Annotated[str | None, Field(description="A substring of the signal names.")] = None) -> str:
    """PDM32: the latest decoded values of its data stream (PDM Generic Output DBC), with their age."""
    return sim("signals", {"filter": filter})


def sim_break(
    function: Annotated[str | None, Field(description="A firmware function name (sim_names lists them).")] = None,
    pc: Annotated[str | None, Field(description="Or an address, '0xA99C' / '0x00100108'.")] = None,
    condition: Annotated[str | None, Field(description="NAME OP NUMBER [&& ...].")] = None,
    action: Annotated[Literal["stop", "record"], Field(description="Stop, or record and carry on.")] = "stop",
    record: Annotated[list[str] | None, Field(description="Names to record at each hit.")] = None,
    ignore: Annotated[int, Field(ge=0, description="Hits to pass before stopping.")] = 0,
) -> str:
    """A breakpoint or tracepoint at a function's entry or an address. Returns its id."""
    return sim(
        "break",
        {"function": function, "pc": pc, "condition": condition, "action": action, "record": record,
         "ignore": ignore},
    )


def sim_watch(
    name: Annotated[str, Field(description="A memory name: SYMBOL (all its bytes), SYMBOL[k], $addr, .w/.l.")],
    on: Annotated[Literal["write", "read", "change"], Field()] = "write",
    condition: Annotated[str | None, Field(description="NAME OP NUMBER [&& ...].")] = None,
    action: Annotated[Literal["stop", "record"], Field()] = "stop",
    record: Annotated[list[str] | None, Field(description="Names to record at each hit.")] = None,
) -> str:
    """A data watchpoint; it stops after the accessing instruction (one hit per access). Returns its id."""
    return sim("watch", {"name": name, "on": on, "condition": condition, "action": action, "record": record})


def sim_clear(id: Annotated[int | None, Field(description="A breakpoint or watch id; without it, all.")] = None) -> str:
    """Remove a breakpoint or watch, or all of them."""
    return sim("clear", {"id": id} if id is not None else {"all": True})


def sim_names(
    filter: Annotated[str | None, Field(description="A substring, case-insensitive.")] = None,
    limit: Annotated[int, Field(ge=1, le=1000)] = 50,
) -> str:
    """Firmware symbols: functions, variables (and the PDM's configuration fields and registers)."""
    return sim("names", {"filter": filter, "limit": limit})


def sim_backtrace() -> str:
    """The pc and the return addresses on the stack (heuristic)."""
    return sim("backtrace")


def sim_registers() -> str:
    """Every CPU register by its assembler name."""
    return sim("registers")


def sim_console(lines: Annotated[int, Field(ge=1, le=200)] = 50) -> str:
    """M150: the last lines of its console (the kernel's and the application's log)."""
    return sim("console", {"lines": lines})


def sim_disasm(
    at: Annotated[str | None, Field(description="A function name or address; default the pc.")] = None,
    count: Annotated[int, Field(ge=1, le=500)] = 16,
) -> str:
    """Disassembly with symbolic locations and the instruction bytes, decoded by the same tables the CPU runs."""
    return sim("disasm", {"at": at, "count": count})


def sim_memory(
    address: Annotated[str, Field(description="'0x0293', '$0293', or a symbol.")],
    length: Annotated[int, Field(ge=1, le=4096)] = 16,
) -> str:
    """Bytes as hex; device registers are read without side effects."""
    return sim("memory", {"address": address, "length": length})


def sim_snapshot(name: str) -> str:
    """Save the whole state (CPU, memory, devices, board, what is wired, scheduled events) under a name."""
    return sim("snapshot", {"name": name})


def sim_restore(name: str) -> str:
    """Return to a saved snapshot."""
    return sim("restore", {"name": name})


TOOLS = (
    sim_status,
    sim_run,
    sim_step,
    sim_finish,
    sim_pause,
    sim_continue,
    sim_reset,
    sim_set_input,
    sim_set_load,
    sim_set_battery,
    sim_outputs,
    sim_inputs,
    sim_read,
    sim_write,
    sim_can_send,
    sim_can_recv,
    sim_signals,
    sim_break,
    sim_watch,
    sim_clear,
    sim_names,
    sim_backtrace,
    sim_registers,
    sim_console,
    sim_disasm,
    sim_memory,
    sim_snapshot,
    sim_restore,
)


def register(mcp, call, fail, running) -> None:
    """Adds the sim_* tools to `mcp`. `call(app, method, params)`, `fail(exc)` and `running()` (the apps running)
    are the server's."""
    _server["call"] = call
    _server["fail"] = fail
    _server["running"] = running
    for tool in TOOLS:
        mcp.tool()(tool)
