---
title: pdm32_sim
parent: Simulators
---

# pdm32_sim

## Overview

`pdm32_sim` runs a MoTeC PDM32's application firmware on an emulated
MC68HC908GZ60, with the PDM's 32 output switches, 23 inputs, the loads on its
connector, and its CAN port. `pdm32_sim serve` puts that CAN port on zenoh
exactly where `can_bridge` would put a real PDM's bus, so
[motec_pdm](../nodes/index.html), scope, the dashboard and `inspect` see a PDM
and cannot tell that it is not one.

It needs the PDM32 application image (`pdm.hex`) that ships with PDM Manager
1.9. The image is MoTeC's and is not in the tree.

## Running it

```bash
export PDM_FIRMWARE_HEX="$HOME/src/motec/MoTeC/PDM Manager/1.9/pdm.hex"
build/sims/pdm32/pdm32_sim serve --scenario sims/pdm32/scenarios/short_output.json
```

The PDM boots, reads its configuration, and starts its data stream: messages
`0x500` to `0x505` on `vehicle/can0/rx`, every 10 ms with the scenario's
configuration. `nodes/motec_pdm` decodes them as it would a real unit's.

| Verb | What it does |
|---|---|
| `serve` | Runs the PDM in real time with its CAN on the bus, and answers agent calls with `--mcp`. |
| `run SCENARIO.json` | Runs a scenario as fast as the emulator can, prints its trace and expectations, and exits 0 only if every expectation passed. |
| `images SCENARIO.json --out DIR` | Writes the flash blocks a scenario builds (configuration, current calibration, factory data) for inspection. |

`serve`'s options:

| Option | Meaning |
|---|---|
| `-c FILE` | The serve configuration, below. Optional. |
| `--scenario FILE` | What is wired to the connector, the configuration it builds, and timed events. |
| `--firmware FILE` | The application image. Default `$PDM_FIRMWARE_HEX`, then PDM Manager's install path. |
| `--engine interp\|block\|jit` | The CPU engine. Default `jit`. |
| `--stepped` | Simulated time moves only when an agent calls `run` or `step`, not with the wall clock. |
| `--allow-write` | Lets agents write firmware memory. |
| `--mcp[=SOCKET]` | The agent control socket; prints `AGENT_READY` when serving. |

The global `--connect` and `--mode` options choose the zenoh session, as for
every tool built on `cli::Program`.

## Configuration

Every key is optional; an unknown key is an error.

```yaml
can:
  name: can0                 # topics follow the name unless given
  rx_key: vehicle/can0/rx    # the PDM's frames, as can_bridge publishes a bus
  tx_key: vehicle/can0/tx    # frames published here reach the PDM
  record_trc: pdm.trc        # both directions, as can_bridge records them
scenario: sims/pdm32/scenarios/short_output.json
firmware: /path/to/pdm.hex
engine: jit
stepped: false
```

## Topics

| Key | Schema | Direction |
|---|---|---|
| `vehicle/can0/rx` | `CanFrame` | The PDM's frames. `channel` is the configured name; `timestampUs` is simulated time since power-on. |
| `vehicle/can0/tx` | `CanFrame` | Frames for the PDM. Classic CAN only: an FD frame, or more than 8 bytes, is refused. |
| `nodes/pdm32_sim/health` | `NodeHealth` | As every node publishes. |

## Scenarios

A scenario is a JSON file that says what is wired to the connector and what
happens when: the battery voltage, a load on each output (`"open"`, `"short"`,
`"lamp:R"` or ohms), the voltage on each input, a configuration built from
simple rules ("output 1 follows input 1 above 6 V"), timed events that change
any of those, and expectations checked against the firmware's state and its data
stream. `sims/pdm32/scenarios/short_output.json` is the worked example: an input
turns an output on, the output is then shorted, and the firmware reports the
fault on its stream.

```bash
build/sims/pdm32/pdm32_sim run sims/pdm32/scenarios/short_output.json
```

## What is real and what is not

The application firmware is MoTeC's own, unmodified. The bootloader is not in
the image, so the simulator performs what the application expects of it
directly. The configuration is built by the simulator from the scenario's rules
rather than compiled by PDM Manager; a configuration read off a unit can be
loaded instead. The current calibration compiled in is one real unit's, read off
it with [pdm_flash_dump](../tools/pdm_flash_dump.html). The simulated unit's
serial number is 1.

The emulated microcontroller covers what this firmware uses. When the firmware
touches a register or a mode the model does not implement, the simulator logs a
`model gap:` warning once, so a run says what it went past.

## Driving it from an agent

Under the [agent control](../developing/agent-control.html) MCP server it is
launched like any app:

```text
app_launch(app="pdm32_sim",
           extra_args=["--scenario", "sims/pdm32/scenarios/short_output.json"])
```

The `sim_*` tools change what is wired (`sim_set_input`, `sim_set_load`,
`sim_set_battery`), read it back (`sim_outputs`, `sim_inputs`, `sim_read`,
`sim_signals` for the decoded data stream, `sim_can_recv` for the frames), and
debug the firmware (`sim_break`, `sim_watch`, `sim_step`, `sim_backtrace`,
`sim_memory`, `sim_snapshot`). A breakpoint hit in real time pauses
the clock until `sim_continue`.

{: .note }
At most one `pdm32_sim` runs at a time, like every app: a second PDM on the same
topics would be two devices answering as one.

## Troubleshooting

**It exits at once with "cannot open".** The firmware image is not where it
looked. Set `PDM_FIRMWARE_HEX` or pass `--firmware`.

**It logs that it is behind the wall clock.** The emulator could not keep up, or
an agent call held it. It carries on from where it is rather than replaying the
missed time as a burst, so the stream has a gap at that point; a release build
with the `jit` engine normally keeps up.

**A frame on the tx topic never reaches the PDM.** The PDM only accepts the
identifiers its acceptance filters are set for; `sim_can_recv` shows every frame
on its bus, accepted or not.
