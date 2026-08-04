# Working in this repository

Redline Labs digital dash: a Qt6 Widgets C++23 instrument cluster, a layout
editor for it, and a set of single-purpose nodes that put vehicle data on a
zenoh bus as Cap'n Proto messages.

## Build and test

```bash
cmake -S . -B build
cmake --build build -j8

ctest --test-dir build -L unit          # fast, deterministic; run this always
ctest --test-dir build -L gui           # constructs Qt widgets, forced offscreen
ctest --test-dir build -LE slow         # everything quick
```

Tests are plain `main()` programs registered with `add_project_test()`
(`cmake/ProjectTest.cmake`). **A test must fail by exit code** — a program that
prints and always returns 0 is a demo, and registering it makes a green run mean
nothing.

New code compiles with `-Werror -Wshadow -Wold-style-cast -Wswitch-enum
-Wsuggest-override`. `-Wswitch-enum` in particular means a `switch` over a large
external enum (`Qt::WindowType`, `capnp::schema::Type::Which`) is impractical —
use an if-chain there.

## How to prove a change works

It depends on what you changed, and the split is sharp.

### Non-GUI code — unit tests, and they are expected

Everything under `libs/` and `nodes/` is protocol parsing, framing, decoding,
maths and state machines. That is exactly what unit tests are good at, and this
tree already has ~30 of them. **A change there is not done without a test**, and
the test should exercise malformed input, not just the happy path: this codebase's
prior differential-testing runs only ever used well-formed input, so bad-input
handling is where the latent bugs still are.

Two habits worth keeping. Name tests `<component>_test_<subject>` and give them a
component label plus one of `unit` / `net` / `gui` / `slow`. And
**mutation-check a regression test** — revert the fix, confirm the test actually
fails, put it back. A test that passes against the bug is worse than none,
because it makes the bug look covered.

### The GUI apps — drive them and look

`dashboard` and `editor` are the two GUI applications, and they are what the
agent control interface exists for. A widget change is not done when it builds;
it is done when you have looked at it.

**The round trip before saying it works:**

1. Make the change and `cmake --build build`.
2. `app_restart(app="dashboard")` — picks up the new binary.
3. `zenoh_publish(...)` to drive the widget to a known state, or
   `widget_set_config(...)` to change how it is configured.
4. `ui_screenshot(target=...)` and **actually look at the image**.
5. `app_logs(...)` if anything is off — usually faster than guessing.

For layout work the editor closes the loop the whole way: build a layout with
`editor_add_widget` / `editor_palette_drag`, `editor_save` it to YAML, then
`app_launch` the dashboard on that file and screenshot the result. If the two
disagree, that is a real bug.

GUI code that is *not* about pixels — selector parsing, message framing, a config
codec — still gets a plain unit test. `libs/agent_control/`'s own suites are the
model: pure logic under the `unit` label, widget-tree behaviour under `gui`.

**Read `docs/agent_control.md` before using the tools.** It has every method, the
selector grammar, the coordinate contract, and the gotchas that otherwise cost an
afternoon — animated widgets defeating single-screenshot comparisons, zenoh
discovery seeing only live traffic, and what `accepted: false` and
`mouse_transparent` actually mean.

## Conventions

- **Adding a widget** is a 5-step registration documented at the top of
  `dashboard/include/editor/widget_registry.h`. Follow it exactly; several
  generated things (the config variant, the palette, the YAML decoder) derive
  from that one macro list. Widgets built this way are automatically inspectable
  and settable through `widget_*_config` — no extra work.
- **Widget identity**: the optional `id:` in a widget's YAML entry becomes its
  `objectName`, falling back to `<type>#<index>`. Set an `id:` on anything worth
  addressing repeatedly; the derived name shifts when widgets are reordered.
- **Configuration** is YAML via reflected structs (`REFLECT_STRUCT` in
  `libs/reflection/`). The editor's properties panel, the YAML codec and the
  agent interface all walk the same field list — add a field once and all three
  follow.
- **Key→schema binding** rides on every zenoh sample as
  `application/capnp;<SchemaName>`. Do not add an out-of-band registry: zenoh has
  no retained messages, so self-description per sample is what lets a
  late-joining tool identify a stream from the first message it sees.
- **Decoding against the wrong capnp schema is silent** — field offsets just land
  on different bytes and you get a plausible wrong number, not an exception.
  That is why publishers stamp the schema and subscribers check it.
- **Threading**: Qt owns exactly one thread. Zenoh callbacks run on zenoh threads
  and must not block; hop with
  `QMetaObject::invokeMethod(obj, lambda, Qt::QueuedConnection)`. See
  `dashboard/include/dashboard/expression_subscription.h` for the established
  shape.
- Logging is `SPDLOG_*`, never `std::cout`. CLI parsing is cxxopts.
- Comments in this codebase explain *why*, especially where a past bug informed
  the shape of the code. Keep that up; it is the most valuable thing in the tree.

## Layout

```
dashboard/          the dashboard app, the editor, and every widget
  include/          shared headers (widget registry, config, agent glue)
  widgets/<name>/   one static lib per widget, each with its own config.h
libs/               reusable: pub_sub (zenoh+capnp), reflection, agent_control,
                    airplay, iap2, apple_usb, plist, canopen, dbc_parser
nodes/              single-purpose executables that bridge hardware to zenoh
schemas/            .capnp definitions; add one line to its CMakeLists to register
configs/dashboard/  runtime YAML layouts
tools/mcp_dashboard/ the MCP server (Python, uv)
docs/               architecture and bring-up notes
```

`docs/carplay_bringup.md` is the deepest doc in the tree and doubles as design
rationale for the zenoh, threading and paint-lock decisions. Read it before
changing anything in the CarPlay path.
