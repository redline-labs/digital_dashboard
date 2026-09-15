---
title: Developing
nav_order: 8
---

# Developing

For someone changing the tree. The repository's `AGENTS.md` is the contributor
guide proper and is kept short enough to read in full; this section holds the
pieces that want more room.

## Build and test

```bash
cmake -S . -B build
cmake --build build -j8

ctest --test-dir build -L unit          # fast, deterministic; run this always
ctest --test-dir build -L gui           # constructs Qt widgets, forced offscreen
ctest --test-dir build -LE slow         # everything quick
```

Tests are plain `main()` programs registered with `add_project_test()` in
`cmake/ProjectTest.cmake`. A test must fail by exit code. Each carries its
component as a label plus one of `unit`, `net`, `gui` or `slow`.

Our code compiles with `-Werror` and a block of warnings chosen so clang on a
Mac and GCC on the Yocto builder diagnose the same set. A warning in our own
code gets fixed, never downgraded; only system and third-party headers are
waived.

## How to prove a change works

Code under `libs/` and `nodes/` is protocol parsing, framing, decoding, maths
and state machines: unit tests, and a change there is not done without one.
The GUI apps are driven and looked at: build, restart the app under the agent
control interface, put the widget in a known state, screenshot it, and read the
image. [Agent control](agent-control.html) has the whole loop.

## Pages

| Page | What is in it |
|---|---|
| [Agent control (--mcp)](agent-control.html) | Every method, the selector grammar, the coordinate contract, and the gotchas. |
| [Adding a widget](adding-a-widget.html) | The widget class, the table, the CMake helper, and how to check it. |
| [Adding a scope panel](adding-a-scope-panel.html) | The panel class, the stats struct that is not optional, the table, the data source seam. |
| [Adding a node](adding-a-node.html) | The shape of a node, node identity, schemas, configuration, `cli::Program`, tests. |
| [Writing docs](writing-docs.html) | How this site builds, the front matter every page needs, which section a page belongs in, and the voice. |
