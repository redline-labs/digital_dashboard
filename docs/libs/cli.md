---
title: cli
parent: Libraries
---

# cli

## Overview

The shared command layer for the tree's multi-verb tools, `inspect` and `bag`.
A tool is a `cli::Program`: a name, a description and a table of verbs, one row
each. Global options are declared once and parsed together with the verb's own,
results go to stdout through `cli::out()`, and Ctrl-C is one handler.

What it exists to stop: `nodes/inspect` had five verbs, each re-declaring
`-k/--key` and `-h/--help` with different help strings, each re-parsing the
whole argv, and a top-level help string maintained by hand in three places, two
of which were stale. Adding a verb touched four files. Here it is one table row
plus a file. It builds on [core](core.html) for the log pattern and on
[pub_sub](pub_sub.html) for `--connect` and `--mode`.

## Public headers

| Header | |
| --- | --- |
| `cli/program.h` | `Program`, `Verb`, `Context`, and the exit codes `kOk`, `kFailure`, `kUsage`. |
| `cli/output.h` | `out()`, `outPartial()` and `flush()`: tool results on stdout, unadorned. |
| `cli/interrupt.h` | `installInterruptHandler()` and `interrupted()`, for verbs that run until told to stop. |
| `cli/session_options.h` | `applySessionOverrides()`: `--connect` and `--mode` into `SessionManager` config. Called by `Program::run()`; verbs do not call it. |

## Using it

Link the CMake target `cli`. cxxopts is in its public interface because a
verb's `add_options` callback takes a `cxxopts::Options&`. This is
`nodes/bag/main.cpp`, shortened:

{% raw %}
```cpp
#include "cli/program.h"

constexpr std::array<cli::Verb, 5> kBagVerbs{{
    {"record", "Subscribe to the bus and write a recording", bag_tool::addRecordOptions,
     bag_tool::runRecord},
    {"info", "What is in a recording, read from its index", bag_tool::addInfoOptions,
     bag_tool::runInfo},
    // ...
}};

int main(int argc, char** argv)
{
    const cli::Program program("bag", "Record and replay the zenoh bus", kBagVerbs);
    return program.run(argc, argv);
}
```
{% endraw %}

A verb's `run` gets a `Context`: `requireString()` for a required option,
`stringOr()`, `uintOr()`, `doubleOr()` and `flag()` for optional ones, and
`json()` and `debug()` for the globals, resolved once so no verb has to know
how they are spelled.

## Behaviour worth knowing

Exit codes are spelled out because scripts read them and getting them wrong is
silent. `inspect` used to print usage and return 0 when a required option was
missing, so `inspect dump && do_something` ran `do_something` after a command
that did nothing.

| Code | Meaning |
| --- | --- |
| `0` | `kOk` |
| `1` | `kFailure`: the verb tried and could not |
| `2` | `kUsage`: no verb, unknown verb, bad option, leftover argument, missing required option |

`requireString()` returns `std::nullopt` having already printed what is
missing and the verb's usage; the verb returns `kUsage`. The failure stays on
the normal path so a verb can still clean up.

Globals and verb options come out of one parse, so `inspect --debug list` and
`inspect list --debug` are the same command. The globals are `--debug`,
`--json`, `--connect` and `--mode`. A verb must not redeclare them; cxxopts
throws at startup if it does, which is the right time to find out.
`add_options` and `run` are separate so `<prog> <verb> --help` can render a
verb's options without running it.

{: .important }
Tool results go to stdout via `cli::out()`, not through spdlog. Routing an
answer through spdlog stamps it with a timestamp and a source location and makes
`--json | jq` impossible. Diagnostics stay `SPDLOG_*` on stderr, so
`inspect list --json > topics.json` still shows its warnings and still produces
a parseable file.

stdout is block-buffered when it is not a terminal. A verb that prints on a
timer (`watch`, `hz`) must call `cli::flush()` or shows nothing until the
buffer fills when piped; a verb that prints once and exits need not, because
exit flushes.

`interrupted()` reads a `volatile std::sig_atomic_t`, the only thing a signal
handler may write; a `std::atomic<bool>` is not guaranteed async-signal-safe.
A verb that never calls `installInterruptHandler()` sees it stay false forever.

`applySessionOverrides()` runs before any verb, which is the only correct
moment: `SessionManager` caches one session per process and `insertConfig()`
affects the next session opened, so an override applied after a verb's first
`getOrCreate()` would be ignored. It is in its own translation unit so that
only `session_options.cpp` includes `<zenoh.hxx>`, about 89,000 preprocessed
lines that every verb in every tool would otherwise pay for through
`program.h`.

## Tests

| Target | Labels | Proves |
| --- | --- | --- |
| `cli_test_dispatch` | `cli unit` | Verb dispatch and the globals/verb split: a bare invocation does not abort, `--debug` works before and after the verb, and a missing required option exits 2, not 0. Nothing here opens a session. |
