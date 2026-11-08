"""Typed tools for the simulators under sims/ -- today the PDM32.

Each is a thin wrapper over one of the simulator's agent methods (app_methods
lists them all; app_call reaches any without a wrapper here). The simulator is
launched like anything else -- app_launch(app="pdm32_sim", ...) -- and, like
everything else, at most one runs: its CAN port is a bus topic, and a second
PDM on vehicle/can0 would be two devices answering as one.
"""

from __future__ import annotations

import json
from typing import Annotated, Any, Literal

from pydantic import Field

from .client import AgentError
from .supervisor import LaunchError

SIM = "pdm32_sim"

# The server's _call and _fail, set by register(): this module is imported by it, not the other way round.
_server: dict[str, Any] = {}


def sim(method: str, params: dict[str, Any] | None = None, timeout_ms: int | None = None) -> str:
    body = {k: v for k, v in (params or {}).items() if v is not None}
    if timeout_ms is not None:
        body["_timeout_ms"] = timeout_ms
    try:
        return json.dumps(_server["call"](SIM, method, body), indent=2)
    except (AgentError, LaunchError, OSError) as exc:
        return _server["fail"](exc)


def sim_status() -> str:
    """The simulated PDM32: sim time, pc and function, CPU and firmware run mode, resets, CAN counters."""
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
    over: Annotated[bool, Field(description="Run a JSR/BSR through to its return.")] = False,
) -> str:
    """Execute instructions one at a time (stepped mode)."""
    return sim("step", {"count": count, "over": over})


def sim_pause() -> str:
    """Real time: stop the clock. Agent calls still answer."""
    return sim("pause")


def sim_continue() -> str:
    """Real time: restart the clock, from where it stopped (a breakpoint or watch also stops it)."""
    return sim("continue")


def sim_reset() -> str:
    """Power-on reset of the PDM (what is wired to it, breakpoints and watches stay)."""
    return sim("reset")


def sim_set_input(
    input: Annotated[int, Field(ge=1, le=23, description="Input number at the connector.")],
    volts: Annotated[
        float | Literal["open"],
        Field(description="Volts driven on the pin, or 'open' (the PDM's pull-up to battery)."),
    ],
) -> str:
    """Drive (or open) an input pin."""
    return sim("input.set", {"input": input, "volts": volts})


def sim_set_load(
    output: Annotated[int, Field(ge=1, le=32, description="Output number at the connector.")],
    load: Annotated[str, Field(description="'open', 'short', 'lamp:<hot ohms>' or ohms ('6').")],
) -> str:
    """Change what is wired to an output: a short to ground, an open circuit, a resistor, a lamp."""
    return sim("load.set", {"output": output, "load": load})


def sim_set_battery(volts: Annotated[float, Field(ge=0, le=40)]) -> str:
    """The battery voltage."""
    return sim("battery.set", {"volts": volts})


def sim_outputs() -> str:
    """Every output: pin, volts, amps, junction temperature, thermal shutdown, load, firmware status."""
    return sim("outputs")


def sim_inputs() -> str:
    """Every input: connector volts, open or driven, the firmware's 8-bit reading."""
    return sim("inputs")


def sim_read(
    names: Annotated[
        list[str],
        Field(
            description=(
                "t, pc, a; out[N].pin/v/i/tj/load/status, in[N].v/raw; can:SIGNAL (the decoded data stream); "
                "a firmware symbol SYMBOL, SYMBOL[k], SYMBOL.w; $addr; dev.REG."
            )
        ),
    ],
) -> str:
    """Values by name; an unknown name comes back null with the reason under errors."""
    return sim("read", {"names": names})


def sim_write(
    name: Annotated[str, Field(description="in[N].v, out[N].load, battery, or (--allow-write) a memory name.")],
    value: Annotated[Any, Field(description="A number, or 'open'/'short'/'lamp:R' where it applies.")],
) -> str:
    """Set a name."""
    return sim("write", {"name": name, "value": value})


def sim_can_send(
    id: Annotated[int, Field(ge=0, le=0x1FFFFFFF, description="Identifier; above 0x7FF is extended.")],
    data: Annotated[str, Field(description="Up to 8 bytes as hex, e.g. '0102A0'.")] = "",
) -> str:
    """Put a frame on the PDM's bus directly, at the current simulated instant. (Publishing on the tx topic --
    vehicle/can0/tx by default -- does the same from the bus side.)"""
    return sim("can.send", {"id": id, "data": data})


def sim_can_recv(
    since: Annotated[int, Field(ge=0, description="First frame index (the previous call's 'next').")] = 0,
    limit: Annotated[int, Field(ge=1, le=10000)] = 100,
    id: Annotated[int | None, Field(description="Only this identifier.")] = None,
) -> str:
    """Frames logged on the PDM's bus, both directions (from_pdm, t, id, data), and the next index."""
    return sim("can.recv", {"since": since, "limit": limit, "id": id})


def sim_signals(filter: Annotated[str | None, Field(description="A substring of the signal names.")] = None) -> str:
    """The latest decoded values of the PDM's data stream (PDM Generic Output DBC), with their age."""
    return sim("signals", {"filter": filter})


def sim_break(
    function: Annotated[str | None, Field(description="A firmware function name (sim_names lists them).")] = None,
    pc: Annotated[str | None, Field(description="Or an address, '0xA99C'.")] = None,
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
    name: Annotated[str, Field(description="A memory name: SYMBOL, SYMBOL[k], $addr.")],
    on: Annotated[Literal["write", "read", "change"], Field()] = "write",
    condition: Annotated[str | None, Field(description="NAME OP NUMBER [&& ...].")] = None,
    action: Annotated[Literal["stop", "record"], Field()] = "stop",
    record: Annotated[list[str] | None, Field(description="Names to record at each hit.")] = None,
) -> str:
    """A data watchpoint; it stops after the accessing instruction. Returns its id."""
    return sim("watch", {"name": name, "on": on, "condition": condition, "action": action, "record": record})


def sim_clear(id: Annotated[int | None, Field(description="A breakpoint or watch id; without it, all.")] = None) -> str:
    """Remove a breakpoint or watch, or all of them."""
    return sim("clear", {"id": id} if id is not None else {"all": True})


def sim_names(
    filter: Annotated[str | None, Field(description="A substring, case-insensitive.")] = None,
    limit: Annotated[int, Field(ge=1, le=1000)] = 50,
) -> str:
    """Firmware symbols: functions, RAM variables, configuration fields, registers."""
    return sim("names", {"filter": filter, "limit": limit})


def sim_backtrace() -> str:
    """The pc and the return addresses on the stack (heuristic)."""
    return sim("backtrace")


def sim_disasm(
    at: Annotated[str | None, Field(description="A function name or address; default the pc.")] = None,
    count: Annotated[int, Field(ge=1, le=500)] = 16,
) -> str:
    """Disassembly with symbolic locations."""
    return sim("disasm", {"at": at, "count": count})


def sim_memory(
    address: Annotated[str, Field(description="'0x0293' or '$0293'.")],
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
    sim_disasm,
    sim_memory,
    sim_snapshot,
    sim_restore,
)


def register(mcp, call, fail) -> None:
    """Adds the sim_* tools to `mcp`. `call(app, method, params)` and `fail(exc)` are the server's."""
    _server["call"] = call
    _server["fail"] = fail
    for tool in TOOLS:
        mcp.tool()(tool)
