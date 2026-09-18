---
title: web_console
parent: Nodes
---

# Web console

`nodes/web_console` serves the board's administration UI over HTTP, so that
reflashing, reading the system over, watching node health and calling a service
need nothing on the client but a browser. It is the answer to "everything here
needs ssh, a workstation with the bundle, and someone who knows the
incantations".

Four views, mobile-first: **Info** (the landing page), **Update**, **Health**,
**Services**.

It deliberately does **not**:

- **Mark a slot good.** The console reports what the bootloader says -- slot,
  tries left, whether it has been marked -- and stops there.
  `redline-mark-good.service` does the marking on the next boot, gated on
  `redline-dashboard.service`, so "good" keeps meaning *the cluster painted a
  frame* rather than *the web server started*.
- **Talk to the bus from the browser.** See [below](#why-the-browser-is-not-a-zenoh-peer).
- **Edit configuration.** Deferred: validation is `dashboard --check --config
  <file>`, not a reimplemented YAML validator.

## Authentication

**The API is gated; the page is not.** Every route that reads or changes the
board lives under `/api/` and requires `Authorization: Bearer <token>`. The
static page stays reachable so a browser that cannot authenticate still loads
and can say *why* it was refused, instead of failing blankly -- and the page on
its own discloses nothing.

A header, never a cookie: nothing is attached automatically, so cross-site
request forgery has nothing to forge with.

**The node fails closed.** The two ways of not having a token mean opposite
things:

| `token_file` | Result |
|---|---|
| empty value | No authentication, warned about loudly. An explicit request, for a workstation or a board nobody can reach. |
| a path that is missing, empty or unreadable | **The node refuses to start.** |

The refusal is the point. The shipped config binds `0.0.0.0` and offers a
reflash endpoint; a console that is believed to be protected and is not is worse
than one nobody trusts. Earlier this warned and served anyway, which meant a
board could ship a network-reachable RAUC endpoint with no authentication while
its own config said otherwise.

**The token is per board and generated on first boot**, not shipped in the
image -- a token baked into the rootfs is the same token on every board and
readable by anyone holding a bundle. The unit's `ExecStartPre` writes
`/data/web_console/token` from `/dev/urandom` under `umask 077` if it is absent,
so it survives updates (`/data` is shared by both slots):

```sh
cat /data/web_console/token
```

One exception, and it is narrow: **`EventSource` cannot set headers** -- no
browser offers an API for it -- so the progress stream *alone* also accepts
`?access_token=`. It is confined to that one read-only route; the same value
returns 401 everywhere else, including `/api/update/install`.

## Endpoints

| Route | | |
|---|---|---|
| `GET /` | the page and its assets | not gated |
| `GET /api/system` | os-release, uptime, load, memory, filesystems, interfaces, temperatures | |
| `GET /api/update/status` | RAUC operation, slots, boot entries, what is staged | |
| `POST /api/update/bundle` | uploads a bundle, streamed to disk | |
| `POST /api/update/install` | asks RAUC to install what is staged | |
| `GET /api/update/events` | install progress (SSE) | also takes `?access_token=` |
| `POST /api/update/mark-good` | marks the running slot good | |
| `GET /api/health` | every node's health, classified | |
| `GET /api/services` | services offered on the bus | |
| `GET,POST /api/schema` | describes a schema, for building a request | |
| `POST /api/call` | calls a service | |

`/api/call` reports what actually happened rather than flattening everything to
200:

| | |
|---|---|
| `200` | a responder replied |
| `400` | the fields did not fit the request schema; `errors` names them |
| `502` | the call failed |
| `504` | nobody answered |

## Reflash

Upload is pre-flighted against `std::filesystem::space` (`507` when short),
written to `/data/updates/.incoming-<pid>-<ts>.raucb`, `fsync`ed, then
`rename()`d into place -- same filesystem, so the swap is atomic. A scope guard
unlinks on any early return and startup sweeps stale `.incoming-*`.

RAUC is reached over D-Bus by name (`de.pengutronix.rauc.Installer`), with no
generated interface code: the XML ships only in `rauc-dev`, which is not on the
image.

**A board whose updater is broken still serves the rest of the console** --
system information and health are exactly what someone diagnosing that wants --
so a missing RAUC is reported (`rauc_available: false`) rather than fatal.

The browser *will* lose its connection when the install ends in a reboot. That
is expected, and the page retries rather than erroring.

## Why the browser is not a zenoh peer

The original design had the browser speak zenoh directly: zenoh-pico plus this
tree's own capnp and health code compiled to wasm, talking to `zenohd`'s `ws/`
listener. Half of that is proven and kept -- the module reproduces the C++
layout fingerprint for 212/212 schemas and decodes a `NodeHealth` sample
byte-identically to native.

What is not proven is pico's **emscripten WebSocket transport**. On the wire it
sends a correct upgrade and a correct zenoh `InitSyn`, then no `InitAck` ever
arrives. Upstream's emscripten CI is build-only, so that path has no evidence of
ever having run.

So the node holds the zenoh session and serves `/api/health` and
`/api/services`, classified and called by the same code `inspect` uses --
`node_health::HealthMonitor` and `pub_sub::callServiceBlocking`. The browser
renders a verdict it was given rather than one it invented, which was the
property that actually mattered. When the transport is proven the wasm path
returns; the routes stay useful regardless, since they are what a browser sees
before the module loads.

## Config

`configs/web_console/web_console.yaml`:

```yaml
bind_address: 0.0.0.0             # the code's default is 127.0.0.1
port: 8080
asset_dir: ""                     # empty: next to the executable
token_file: "${REDLINE_DATA_DIR}/web_console/token"
upload_dir: "${REDLINE_DATA_DIR}/updates"
rauc_bus: system                  # "session" only for tools/rauc_stub
```

`bind_address` is widened here rather than in the code so that a developer
running this on a laptop is not exposed by accident.

## On the target

An instance of the `redline-node@.service` template, with a drop-in
(`10-data-and-token.conf`) that carries `RequiresMountsFor=/data`, creates
`/data/updates`, and generates the token. Enabled through
`REDLINE_ENABLED_NODES` on the machine.

Asset-only iteration needs no image rebuild: `scp` `web/` to `/data` and point
`asset_dir` at it through `/data/nodes/web_console.args`.

## Running it without the hardware

```sh
./build/nodes/web_console/web_console --config configs/web_console/web_console.yaml
```

- **RAUC**: `tools/rauc_stub` is a fake `de.pengutronix.rauc.Installer` in C,
  with good/fail/busy modes, reached by setting `rauc_bus: session`.
- **The bus**: `tools/bus-sandbox.sh` runs a command in a network namespace
  where `lo` has multicast, which this workstation's does not.
- **A service to call**: `tools/fake_service` offers
  `nodes/fake_backlight/set_brightness`, since every real service-offering node
  wants hardware.

## Not done yet

- **Never built under Yocto, never run on the bench.** Everything above is
  verified on a desktop; the recipe changes are reasoned-about.
- **No TLS, no firewall, and it runs as root** like everything else on this
  image. `User=redline-web` with `ProtectSystem=strict` is cheap functionally
  (RAUC's D-Bus policy allows `context="default"` with no polkit) and is the
  obvious next step.
- **A plain node cannot be aimed at a router without recompiling.**
  `PUB_SUB_NO_DISCOVERY` is the only environment hook, and `--connect`/`--mode`
  live in `libs/cli`, which plain nodes do not use. Fine while multicast works
  on `lo`; not fine once `zenohd` binds one interface under an ACL.
- **Live plotting** (what `scope` does) is not here. The architecture reaches it
  without rework: a subscriber and a canvas.
