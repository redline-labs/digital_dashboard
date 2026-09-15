---
title: Adding a node
parent: Developing
nav_order: 4
---

# Adding a node

A node is a single-purpose program that puts one device (or one file) on the
bus. Most are under two hundred lines: parse a config, open the hardware,
translate what comes out onto Cap'n Proto schemas, publish. Everything that can
be tested without the hardware belongs in a library under `libs/`; the node is
the part that cannot.

## The shape of one

`nodes/motec_m1/main.cpp` is a good model for a CAN decoder, and
`nodes/mti610_bridge/` for a node that owns a serial port and answers services.
The pieces every node has:

```cpp
int main(int argc, char** argv)
{
    core::setupLogging({.program = "my_node"});     // spdlog, REDLINE_LOG_DIR, the pattern
    // ... cxxopts for the command line ...

    pub_sub::NodeIdentity node_identity("my_node"); // announces the process on the bus

    pub_sub::ZenohPublisher<MyReading> pub("nodes/my_node/reading");
    pub_sub::ZenohTypedSubscriber<CanFrame> sub("vehicle/engine/rx", [&](const CanFrame::Reader& f) { ... });
    // ... run until signalled ...
}
```

`core::setupLogging` is where the runtime environment variables are honoured,
so a node that calls it behaves the same on a desktop and under systemd on the
target. Logging is `SPDLOG_*`, never `std::cout`.

`pub_sub::NodeIdentity` is declared explicitly, once, in `main()`. It is the
only way a process that subscribes but never publishes appears on the bus at
all, and it maps the session id stamped on every sample to a name a person
recognises in `inspect nodes`. The name goes through the key validator, so a
name with a `/` in it is refused at startup.

## Schemas

Messages are Cap'n Proto. To add one, drop a `.capnp` file in `schemas/` and
build; the registry that every tool uses to identify a stream is generated from
the schema files themselves, and the doc comments in the schema are what
switchboard's forms and the editor's signal browser display, so write them for a
reader.

Publishers stamp the schema name on every sample as
`application/capnp;<SchemaName>` and subscribers check it. Do not add an
out-of-band registry: zenoh has no retained messages, and self-description per
sample is what lets a late-joining tool identify a stream from the first
message it sees. The key rules, the liveliness tokens and the timestamp are on
the [bus conventions](../reference/bus-conventions.html) page.

{: .warning }
Decoding against the wrong schema is silent. Field offsets land on different
bytes and you get a plausible wrong number, not an exception. That is why the
stamp exists; keep it.

## Configuration

Node configuration is YAML through reflected structs: declare a `NodeConfig`
with `REFLECT_STRUCT`, load it with `config_codec`, and put the file under
`configs/<node>/`. A file that names a path should expand it with
`core::paths::expand()` so `${REDLINE_DATA_DIR}` and `~/` work the way they do
everywhere else.

A node with several verbs (`inspect`, `bag`) is a `cli::Program`
(`libs/cli/`): a table of verbs, one row each, with the global options
(`--debug`, `--json`, `--connect`, `--mode`) declared once and accepted before
or after the verb. Exit codes are 0, 1 for failure and 2 for usage; a missing
required option must not report success. Results go to stdout through
`cli::out()` so `--json | jq` works; diagnostics stay on stderr through spdlog.

## Build and tests

A node's `CMakeLists.txt` is an `add_executable` plus its link line, added
from `nodes/CMakeLists.txt` with a one-line comment saying what it bridges.
The decoding, framing and state-machine code goes in a library so it can be
tested without the hardware, and a change there is not done without a test.
Register tests with `add_project_test(TARGET <component>_test_<subject> LABELS
<component> unit)`; a test must fail by exit code. Exercise malformed input,
not only the happy path, and mutation-check a regression test: revert the fix,
confirm the test fails, put it back.

Where a device exists that no hardware has yet confirmed, say so in the test
and on the node's page, with the date. A capture from a real device beats a
hand-written vector, because a vector authored from the same reading of the
spec as the parser agrees with the parser even where both are wrong.

## Documenting it

Every node has a page under `docs/nodes/` named after the node, following the
shape in [Writing docs](writing-docs.html), and a row in the nodes index. A
node whose page says "not written yet" is still listed.
