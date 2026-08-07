# Working in this repository

Redline Labs digital dash: a Qt6 Widgets C++23 instrument cluster, a layout
editor for it, a live time-series visualizer, and a set of single-purpose nodes
that put vehicle data on a zenoh bus as Cap'n Proto messages.

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
-Wsuggest-override`. **Spell out every case in a `switch` over an enum.** Do not
dodge `-Wswitch-enum` by rewriting the switch as an if-chain, and do not add a
`default:` that swallows the cases you did not want to name — both throw away
the only thing that tells you where to look when a value is added to the enum.
If a trailing fallback is genuinely needed, put it after the last `case`, not in
a `default:`.

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

`dashboard`, `editor` and `scope` are the GUI applications, and they are what the
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

For `scope`, reach for `scope_sample_stats` before the screenshot. A picture
shows a line; that says what the line is made of -- sample counts, drops, and
the min/max actually received -- which is a far stronger statement and the one
a test can assert. `docs/scope.md` has the rest.

Scope can also be driven over a **recording** rather than the bus, which is the
faster loop when the data matters more than the timing: `bag record` a few
seconds, `scope.open_recording`, then seek to an exact instant and assert on
`scope_sample_stats`. Seeking **backwards** is the case worth checking -- a
buffer that kept the position it came from stays perfectly ordered and is
completely wrong, so the assertion is that `t_last` moved back, not that the
samples are in order.

GUI code that is *not* about pixels — selector parsing, message framing, a config
codec — still gets a plain unit test. `libs/agent_control/`'s own suites are the
model: pure logic under the `unit` label, widget-tree behaviour under `gui`.

**Read `docs/agent_control.md` before using the tools.** It has every method, the
selector grammar, the coordinate contract, and the gotchas that otherwise cost an
afternoon — animated widgets defeating single-screenshot comparisons, zenoh
discovery seeing only live traffic, and what `accepted: false` and
`mouse_transparent` actually mean.

## Conventions

- **Where samples come from is one interface.** `scope::DataSource` has two
  implementations -- the live bus and a recording -- and nothing above it knows
  which it has. Swapping between them (`ScopeWindow::setSource()`) has ONE
  ordering rule: panels release their handles against the OLD source before the
  pointer moves, because a handle means nothing to a source that did not issue
  it. The window destroys the old source only after that, precisely so the
  releases have somewhere to go.
- **A plotted buffer's times must be non-decreasing.**
  `SampleHistory::lowerBound()` is a binary search that assumes it and cannot
  detect otherwise -- it returns a plausible wrong index, and the autoscale, the
  decimation and the cursor readout all compute from the wrong samples with
  nothing logged. Scrubbing backwards is the operation that breaks it, which is
  why a seek clears before it refills.
- **Adding a scope panel** is a 3-step registration documented at the top of
  `scope/include/scope/panel_registry.h`, and works the same way: one line in
  `SCOPE_PANEL_TABLE` and everything else follows. Panels decide for themselves
  what they will plot via `acceptsBinding()`, so the signal browser and the drag
  need no knowledge of panel types. A panel declares a reflected `config_t` AND
  a reflected `stats_t`; the second is not optional, because `scope.stats` and
  `scope.describe_stats` are served by visiting it and a panel without one would
  answer `{}` — which looks exactly like a working panel that has received
  nothing.
- **Not every stream is a number.** `DataSource::bind()` turns a message into a
  `double`; `bindRaw()` hands over the bytes. The raw path stays
  schema-agnostic — the consumer supplies a `RawClassifier` and the source stores
  its answer uninterpreted — because the moment that interface knows what a
  `CarPlayVideo` is, one panel's schema is in the interface every panel shares.
  Two flag bits are reserved: `kSeekPoint` ("you can start here") and `kPreamble`
  ("replay me before the seek point after me"). See `docs/scope.md`.
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
- **DBC tests own their DBCs.** `libs/dbc_parser/tests/dbcs/` holds three
  synthetic files generated by `tests/gen_golden.py` as a systematic sweep over
  byte order, signedness, length, alignment and scaling; the expected values are
  cantools' and are embedded in `tests/golden_data.h`. Nothing under `tests/`
  reads `dbcs/`, so editing a vendor DBC cannot break a parser test. cantools is
  not a build dependency — regenerate only when changing the sweep:
  `python3 -m venv /tmp/ct && /tmp/ct/bin/pip install cantools && /tmp/ct/bin/python libs/dbc_parser/tests/gen_golden.py`.
  Dropping product-DBC round-trip coverage is only safe while that sweep stays
  *generated*; hand-picking cases would break the argument.
- **Key→schema binding** rides on every zenoh sample as
  `application/capnp;<SchemaName>`. Do not add an out-of-band registry: zenoh has
  no retained messages, so self-description per sample is what lets a
  late-joining tool identify a stream from the first message it sees.
- **Topics also advertise themselves** via a zenoh liveliness token declared in
  `detail::BytePublisher`, so a picker can list a topic before it has published
  anything. That is additive, not a replacement: the per-sample encoding stays
  authoritative, and both are derived from the same constructor arguments so
  they cannot disagree. See `docs/scope.md`.
- **Three liveliness key spaces**, all under a leading `@` segment so zenoh
  treats them as verbatim and no `**` subscriber ever sees them as topics:
  `@redline/adv/<Schema>/<topic>/<zid>` (per publisher),
  `@redline/node/<zid>/<name>` (per process, from `pub_sub::NodeIdentity` —
  declare one in every `main()`), and
  `@redline/svc/<zid>/<Req>/<Resp>/<key>` (per `ZenohService`). They join on the
  zid. **Every parser accepts extra trailing segments and ignores them.** That
  rule is not optional: a directory drops what it cannot parse, so a reader that
  rejected an unknown longer form would make the first added field a silent,
  total outage for every build that predates it — an empty picker looks exactly
  like a bus with no publishers.
- **Samples carry a publish timestamp**, because `SessionManager::buildConfig()`
  enables `timestamping` (zenoh only stamps in router mode by default, and every
  session here is a peer). `SampleMeta` exposes it plus the origin zid — the
  session that actually sent the bytes. Convert with
  `pub_sub::ntp64ToUnixNanos()`; NTP64 is seconds<<32 | fraction, so a naive read
  is off by 2^32 and yields a *plausible* wrong time. It is the publisher's wall
  clock and can be quietly wrong — see `pub_sub/timestamp.h` before relying on it.
- **CLI tools are `cli::Program`** (`libs/cli/`): a table of verbs, one row each.
  Adding a verb is that row plus a file. Global options (`--debug`, `--json`,
  `--connect`, `--mode`) are declared once and work before or after the verb.
  Exit codes are 0 / 1 (failure) / **2 (usage)** — a missing required option must
  not report success.
- **Tool *results* go to stdout via `cli::out()`**, not through spdlog. The
  "never `std::cout`" rule below is about *logging*; routing an answer through
  spdlog stamps it with a timestamp and a source location and makes
  `--json | jq` impossible. Diagnostics stay `SPDLOG_*` on stderr, so
  `inspect list --json > topics.json` still shows its warnings and still produces
  a parseable file.
- **Zenoh keys are `[A-Za-z0-9_-/]`, enforced.** `%` is the mangling separator,
  `@` makes a segment verbatim and therefore invisible to every wildcard
  subscription, and `* $ ? #` are rejected by zenoh outright. Each fails
  silently, so the charset is checked in the editor, at config load, and in the
  publisher. Use `pub_sub::topicKeyProblem()` rather than writing another check.
- **Decoding against the wrong capnp schema is silent** — field offsets just land
  on different bytes and you get a plausible wrong number, not an exception.
  That is why publishers stamp the schema and subscribers check it.
- **Threading**: Qt owns exactly one thread. Zenoh callbacks run on zenoh threads
  and must not block; hop with
  `QMetaObject::invokeMethod(obj, lambda, Qt::QueuedConnection)`. See
  `dashboard/include/dashboard/expression_subscription.h` for the established
  shape.
- Logging is `SPDLOG_*`, never `std::cout` -- see the stdout carve-out above for
  tool *results*. CLI parsing is cxxopts, through `libs/cli` for multi-verb tools.
- Comments in this codebase explain *why*, especially where a past bug informed
  the shape of the code. Keep that up; it is the most valuable thing in the tree.

## Layout

```
dashboard/          the dashboard app, the editor, and every widget
  include/          shared headers (widget registry, config, agent glue)
  widgets/<name>/   one static lib per widget, each with its own config.h
scope/              the time-series visualizer app
  panels/<name>/    one panel type per directory, each with its own config.h
                    and stats.h (time_series, video)
libs/               reusable: pub_sub (zenoh+capnp), reflection, agent_control,
                    config_codec, qt_helpers, airplay, iap2, apple_usb, plist,
                    canopen, dbc_parser, cli (verb dispatch), bag (MCAP record/replay),
                    can + can_pcan/can_socketcan/can_trc/can_backends (CAN channels)
nodes/              single-purpose executables that bridge hardware to zenoh,
                    plus the two tools: inspect (look at the bus) and bag
                    (record and replay it -- see docs/bag.md)
schemas/            .capnp definitions; add one line to its CMakeLists to register
configs/dashboard/  runtime YAML layouts
configs/scope/      runtime YAML workspaces
tools/mcp_dashboard/ the MCP server (Python, uv)
docs/               architecture and bring-up notes
```

`config_codec` and `qt_helpers` are the shared layer between the GUI apps.
`config_codec` is the reflected-struct machinery -- YAML both ways, JSON both
ways plus a self-description for tooling, validation by field path, and range
clamping -- and has no Qt in it, deliberately, because the headless config tests
are its main consumer. `qt_helpers` is the Qt-using sibling of the Qt-free
`helpers`: the layered paint-cache widget base, colour conversion, resource
fonts. Anything an app needs that is not about *that* app belongs in one of
these two, not in the app's include tree.

`docs/carplay_bringup.md` is the deepest doc in the tree and doubles as design
rationale for the zenoh, threading and paint-lock decisions. Read it before
changing anything in the CarPlay path.
