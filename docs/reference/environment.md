---
title: Runtime environment
parent: Reference
redirect_from: /environment.html
---

# Runtime environment

Every binary in this tree runs on a developer machine with nothing set, and on
the LattePanda image with a few variables the systemd units provide. `libs/core`
is where they are read (`core::paths`, `core::setupLogging`,
`core::systemd`); nothing else in the tree reads them directly.

| Variable | Set by | Meaning |
|---|---|---|
| `REDLINE_DATA_DIR` | the image: `/data` | Where deployment data lives: map archives, road graphs, recordings. Configs write `${REDLINE_DATA_DIR}/maps/socal.mbtiles`. Unset on a desktop, which means `$XDG_DATA_HOME/redline` or `~/.local/share/redline` (Linux) and `~/Library/Application Support/redline` (macOS). Set it to point at your own data directory. |
| `REDLINE_LOG_DIR` | nobody by default | When set, every program also writes a rotating log `<dir>/<program>.txt`. Unset means console only: under systemd stderr is already the journal, and a tool writing into the checkout by default was a surprise. `REDLINE_LOG_DIR=logs` restores the old `logs/` files. |
| `REDLINE_DISPLAY_<ROLE>_CONNECTOR` `_MODE` `_PROFILE` | `redline-display-setup` on the image | The detected display modules; the dashboard binds windows to them. See [displays.md](../apps/dashboard/windows.md). |
| `NOTIFY_SOCKET` | systemd, for `Type=notify` units | The dashboard sends `READY=1` after its first frame is on screen, so the unit becomes active when the cluster is visible rather than when the process starts. No libsystemd; unset means no-op. |

| `REDLINE_MFI_I2C_DEV` | the image: `/dev/i2c-13` (the `redline-node@carplay` drop-in) | The I2C adapter the Apple MFi coprocessor is on. Unset on a desktop, where the driver auto-detects an MCP2221A bridge (and otherwise takes the first adapter, which on a board with a GPU is its DDC bus -- so the image names it). `carplay --mfi-i2c-device` and `apple_mfi_demo /dev/i2c-N` override it. |
| `PUB_SUB_NO_DISCOVERY` | tests and tools | `1` keeps a zenoh session off the machine's bus. |
| `REDLINE_REPO_ROOT`, `REDLINE_BUILD_DIR` | developers | Where the MCP supervisor finds the checkout and its binaries. See [agent_control.md](../developing/agent-control.md). |

An operator's dashboard config lives at `${REDLINE_DATA_DIR}/dashboard/config.yaml`; see
[config-on-target.md](../apps/dashboard/on-target.md).

Config files may use `${REDLINE_DATA_DIR}`, any `${VARIABLE}`, and `~/`; relative
paths resolve against the config file's own directory. Today the map server's
tilesets, graphs and tracksets go through this; a new config key that names a
file should call `core::paths::expand()` too.

Shipped resources (the `eds/` directory, for one) are found with
`core::paths::resource()`: next to the installed binaries first
(`/opt/redline/bin/../eds`), then by walking up from the binary to the checkout a
developer build sits in, then `REDLINE_REPO_ROOT`. No build path is compiled in.
