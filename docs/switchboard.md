# switchboard — call the services on the bus

The fourth GUI application. `inspect call <key> --data '{json}'` can call any
advertised service, but only if you already know its fields and can type the
JSON. `switchboard` lists every advertised service. Pick one, and it builds a
form from the request schema. Submit, and the reply comes back as fields.

```bash
cmake --build build --target switchboard
./build/apps/switchboard/switchboard                 # attaches to the bus at startup
./build/apps/switchboard/switchboard --timeout 5000  # default reply wait, changeable in the window
./build/apps/switchboard/switchboard --mcp           # headless, agent-driven
```

```
┌─────────────┬──────────────────────┬──────────────────────┐
│ 🔎 filter    │ map/route            │ ✔ Replied · 42 ms    │
│ ▾ map_server│ MapRouteRequest →    │ ──────────────────── │
│  ● map/tile │   MapRouteResponse   │ ok        ✔ true     │
│  ● map/route│ ⓘ struct doc comment │ distanceM   18234.5  │
│ ▾ grayhill  │ fromLatitudeDeg [..] │ ▸ legs (3)           │
│  ● set_ind. │ profile  [fastest ]  │ ── History ───────── │
│ ▸ offline(1)│ timeout [2000] ms    │ 12:01:03 ✔ 42 ms     │
│             │ [Reset] [Submit ⌘⏎]  │ 12:00:51 ✖ no reply  │
└─────────────┴──────────────────────┴──────────────────────┘
```

## What each pane does

**Services.** Everything `pub_sub::ServiceDirectory` has seen, grouped by the
node offering it (the name from `NodeIdentity`, or a short zid).
- **Offline services:** a node that went away leaves its services under
  "offline (N)". They are greyed out, never removed.
- **`⚠ ×2` on a row:** two nodes offer that key. A call reaches both, and both
  replies are shown.
- **Filter:** matches key, schema and node name.

**Request.** A form built at runtime from the request schema (`SchemaForm`).
Each field shows its name and type. Where the schema has a `#` doc comment, an
ⓘ shows it.

| Schema type | Editor |
|---|---|
| Bool | checkbox |
| Int\*/UInt\* | text: decimal, or hex with `0x` |
| Float\* | text |
| Text | text |
| Enum | drop-down of enumerant names |
| Data | hex (`01 ff 7a`), with a live byte count |
| struct / group | inset sub-form |
| union | drop-down of arms plus the chosen arm's form |
| List | rows with **+ Add** and **✕**; a `$fixedLength` list has exactly that many rows |

**Validation is the send path's.** On every edit the form runs its own value
through `pub_sub::jsonToCapnp`, the function that builds the request. Whatever
that rejects is marked on the field it names. The form cannot call something
valid that the send path would refuse.

**Response.** The status reads `✔ Replied · 42 ms`, `⚠ Service error`,
`✖ No reply after 2000 ms`, or `✖ Request rejected — nothing was sent`. Below
it, each reply gets a tree with field, value and type columns.
- **Data** shows as grouped hex.
- **Error banner:** a red banner appears when a reply has `ok = false`, showing
  its `error` or `message`. For a reply with no `ok` field, the banner appears
  when a top-level `status` enum is anything but `ok`, or when a top-level
  `error` is non-empty. That is how the map services report failure:
  `map/route` with a missing graph answers `status = noSuchGraph` beside a
  zero-metre route. These are conventions responses here follow; nothing
  enforces them.

**History.** Every call this session, per service key, newest first. Clicking
one puts its request back in the form and shows its result. Selecting a service
again restores its last request.

History is in memory only, on purpose. A persisted log would let yesterday's
"set the bitrate" be replayed into today's car with one click.

## How a call works

`pub_sub::callService` (`libs/pub_sub/include/pub_sub/dynamic_service_call.h`)
does all the bus work, and `inspect call` uses the same function. It:
1. builds the request from JSON against the advertised schema;
2. sends it to **every** queryable on the key with consolidation off;
3. collects every reply until zenoh reports the query finished.

zenoh's defaults would do otherwise: replies on the same key are merged, so a
second node's answer would be dropped without a trace.

**Things that are not what they look like:**

- **"No reply" means one of two things.** Either no node serves the key any
  more, or none answered within the timeout. zenoh cannot tell these apart.
- **With two replies, you cannot tell which node sent which.** zenoh's replier
  id is behind `Z_FEATURE_UNSTABLE_API`, which this build does not enable.
- **A service handler that throws answers with an error reply.** The reply
  carries the exception's message, and switchboard shows it as
  `⚠ Service error`. Before this, the exception reached zenoh's Rust frame and
  aborted the node.
- **A malformed request still gets a default-constructed response** from
  `ZenohService`. switchboard never sends one, because the form will not submit
  while invalid.
- **Only schemas compiled into this build can be called.** For a service whose
  request schema is not in the registry, the header says so and there is no
  form.

## Agent control

`switchboard` is in the redline MCP server's app list, so start it with
`app_launch(app="switchboard")`. It has no typed tools. Use `app_call` with the
methods below, which drive the window the way a person would.

| Method | Kind | Does |
|---|---|---|
| `switchboard.services` | read | every row: key, schemas, owner, reachable, offered_by |
| `switchboard.select {key, owner_zid?}` | mutating | selects a service and builds its form |
| `switchboard.form` | read | the form's fields as JSON, `valid`, `problems` |
| `switchboard.set_fields {fields}` | mutating | partial update, all or nothing; unknown fields are errors |
| `switchboard.reset` | mutating | back to schema defaults |
| `switchboard.submit {timeout_ms?}` | mutating | returns `{call_id}` immediately |
| `switchboard.result {call_id}` | read | `pending`, or the full result plus what the response pane reads |
| `switchboard.history {key?}` | read | past calls, newest first |

A call is two steps because handlers run on the GUI thread and a reply can take
the whole timeout. Submit, then poll `result` until `pending` is false:

```
app_launch(app="map_server", config="configs/map_server.yaml")
app_launch(app="switchboard")
app_call(app="switchboard", method="switchboard.select", params={"key": "map/route"})
app_call(app="switchboard", method="switchboard.set_fields", params={"fields": {...}})
app_call(app="switchboard", method="switchboard.submit")          # -> {"call_id": 1}
app_call(app="switchboard", method="switchboard.result", params={"call_id": 1})
```

## Tests

- **`switchboard_test_schema_form` (gui):**
  - builds a form for **every** schema in the registry and checks the send path
    accepts its defaults, so a new schema with a shape the form cannot represent
    fails here;
  - checks `setValue` → `value` is an identity over pub_sub's capnp_json
    fixture (unions, nested lists, Data, 64-bit extremes).
- **`switchboard_test_response_view` (gui):** covers the `ok = false` banner,
  error replies, Data as hex, and multiple replies.
- **`switchboard_test_window` (gui, net):** runs against in-process services.
  It covers discover → select → fill → submit → reply, a throwing handler, and
  history replay.
- **Library tests** in `libs/pub_sub`:
  - `pub_sub_test_capnp_json` covers the conversions;
  - `pub_sub_test_dynamic_service_call` covers the call itself, including the
    throwing handler and duplicate responders;
  - `schemas_test_doc_comments` covers the doc comments the form shows.
