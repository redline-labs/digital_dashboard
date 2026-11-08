"""The sim_* tools against a real pdm32_sim, through the supervisor as an agent reaches it.

Skipped unless the simulator is built and the PDM firmware image is present: the
image is MoTeC's and not in the tree. Run with
`uv run --with pytest pytest tests` from tools/mcp_dashboard.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

from redline_mcp import server as srv
from redline_mcp import sim_tools as sim
from redline_mcp.supervisor import LaunchError, binary_for

FIRMWARE = os.environ.get("PDM_FIRMWARE_HEX") or os.path.expanduser("~/src/motec/MoTeC/PDM Manager/1.9/pdm.hex")


def _built() -> bool:
    try:
        binary_for("pdm32_sim")
    except LaunchError:
        return False
    return True


needs_sim = pytest.mark.skipif(
    not _built() or not Path(FIRMWARE).exists(), reason="pdm32_sim not built, or no firmware image"
)


@pytest.fixture
def stepped():
    srv.supervisor.launch(
        "pdm32_sim",
        extra_args=[
            "--stepped",
            "--firmware", FIRMWARE,
            "--scenario", "sims/pdm32/scenarios/short_output.json",
            # Its own topics, so a test run never lands on a bench's vehicle/can0.
            "-c", str(Path(__file__).with_name("pdm32_test.yaml")),
        ],
    )
    yield
    srv.supervisor.quit("pdm32_sim")


def j(text: str):
    return json.loads(text)


def test_no_simulator_is_said_plainly():
    srv.supervisor.shutdown()
    assert sim.sim_status().startswith("LaunchError: No pdm32_sim is running")


@needs_sim
def test_input_drives_output_then_short_faults(stepped):
    assert j(sim.sim_status())["firmware_run_mode"] == 0
    j(sim.sim_run(until=1.5))
    values = j(sim.sim_read(["out[1].pin", "can:PDM_Output_Status_1"]))["values"]
    assert values["out[1].pin"] == 1
    assert values["can:PDM_Output_Status_1"] == 1
    j(sim.sim_set_load(1, "short"))
    j(sim.sim_run(seconds=0.5))
    out1 = j(sim.sim_outputs())[0]
    assert out1["status"] & 0x02
    assert "fault" in out1["status_text"]


@needs_sim
def test_breakpoint_by_function_and_backtrace(stepped):
    bp = j(sim.sim_break(function="EvaluateCompiledLogicChannels"))["id"]
    stop = j(sim.sim_run(seconds=1.0))
    assert stop["reason"] == "break" and stop["where"] == "EvaluateCompiledLogicChannels"
    assert any("MainLoop" in f["where"] for f in j(sim.sim_backtrace()))
    j(sim.sim_clear(id=bp))


@needs_sim
def test_bad_parameters_come_back_with_their_reason(stepped):
    assert sim.sim_can_send(0x123, "zz").startswith("BAD_PARAMS")
    assert sim.sim_write("g_runMode", 1).startswith("BAD_PARAMS")  # memory needs --allow-write


@needs_sim
def test_a_simulator_is_never_the_default_app(stepped):
    # With only the simulator running, a call that names no app has nothing to mean.
    with pytest.raises(LaunchError):
        srv.supervisor.get(None)
    info = j(srv.app_info(app="pdm32_sim"))
    assert info["app"] == "pdm32_sim" and info["time"] == "stepped"
    logs = j(srv.app_logs(app="pdm32_sim", grep="firmware"))
    assert logs["records"]
