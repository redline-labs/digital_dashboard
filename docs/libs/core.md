---
title: core
parent: Libraries
---

# core

## Overview

The tree's runtime bring-up, shared by the dashboard, the tools and every node:
how logs are formatted and where they go, where data and shipped resources
live, and how a service tells systemd it is ready. Nothing here assumes a
target. On a developer machine no `REDLINE_*` variable is set and every
function falls back to something sensible for a checkout; on the LattePanda
image the units set `REDLINE_DATA_DIR=/data` and systemd provides
`NOTIFY_SOCKET`.

Command-line parsing is deliberately gone from here. This used to parse argv
under a hardcoded program name as well as set up logging; parsing now lives in
[cli](cli.html) for multi-verb tools or in each node's own `main()`.

## Public headers

| Header | |
| --- | --- |
| `core/core.h` | `setupLogging` and `LoggingOptions`; `paths::logDir`, `dataDir`, `executableDir`, `resource`, `expand`; `systemd::notifyReady`, `notifyStatus`. |

## Using it

Link the CMake target `core`. The dashboard's `main()` is the shape every
program follows:

```cpp
#include "core/core.h"

core::setupLogging({.program = "dashboard", .debug = args->debug_enabled, .stderr_only = agent_mode});

const std::string archive = core::paths::expand("${REDLINE_DATA_DIR}/maps/socal.mbtiles");
const std::string eds = core::paths::resource("eds/grayhill/DS401_3K_C.eds");

// after the first window is up
core::systemd::notifyReady();
```

## Behaviour worth knowing

There is one log pattern, identical everywhere because logs from the dashboard,
the nodes and the tools end up interleaved in one terminal during bring-up:

```
[%Y/%m/%d %H:%M:%S.%e%z] [%^%l%$] [%t:%s:%#] %v
```

A rotating file sink, `<REDLINE_LOG_DIR>/<program>.txt` at 5 MiB times 3
files, is added only when `REDLINE_LOG_DIR` is set. Unset means no file: under
systemd stderr is already the journal, and the old default of
`logs/rotating.txt` relative to the working directory aborted the cluster on a
read-only rootfs. `REDLINE_LOG_DIR=logs` restores the old behaviour. A log file
that cannot be opened is a warning on the console, never a failure.
`stderr_only` replaces every sink with stderr; the apps set it under `--mcp`
because stdout is the protocol channel there.

The two-argument `setupLogging(bool)` overload sets pattern and level only and
leaves sinks alone. Prefer the options form.

`dataDir()` is `REDLINE_DATA_DIR` when set, otherwise `$XDG_DATA_HOME/redline`
or `~/.local/share/redline` on Linux and
`~/Library/Application Support/redline` on macOS.

`resource()` looks for `<executable dir>/../<relative>` first, the install
layout of `/opt/redline/bin` beside `/opt/redline/eds`; then walks up to six
directories from the executable to find a developer checkout; then tries
`REDLINE_REPO_ROOT`. No build path is compiled in, because a binary that
carries its build directory fails the image's QA and is wrong on any other
machine. A path it cannot find comes back unchanged.

`expand()` substitutes `${NAME}` from the environment, with
`${REDLINE_DATA_DIR}` expanding to `dataDir()` even when the variable is unset
and any other unset variable expanding to nothing. `~/` is the home directory.
A relative path is resolved against `relative_to` when one is given (a config
file's directory) and otherwise left alone.

`notifyReady()` speaks `sd_notify` over a datagram socket with no libsystemd,
handling the abstract-socket form that starts with `@`. Without
`NOTIFY_SOCKET` it returns false silently, which is every run outside a
`Type=notify` unit.

## Tests

`core_test` is registered with a plain `add_test`, so it has no labels and
`ctest -L unit` does not select it; run it by name. It covers `dataDir()`
precedence, every `expand()` rule, `executableDir()` and `resource()` against
a real file in the checkout, and the notification against a socket it opens
itself, without a target.
