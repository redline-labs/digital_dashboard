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

## No authentication

**Anything that can reach the port can drive the console, reflash included.**
The board sits on a trusted private network, so there is no token and no login.
An earlier version had a bearer token; it was removed rather than left as a
setting nobody used. `token_file` is now an unknown key, so a config that still
asks for authentication stops the node instead of serving an open console its
operator believes is protected. If the network stops being trusted, the token
code is in git history.

## Endpoints

| Route | | |
|---|---|---|
| `GET /` | the page and its assets | |
| `GET /api/system` | os-release, uptime, load, memory, filesystems, interfaces, temperatures | |
| `GET /api/update/status` | RAUC operation, slots, boot entries, what is staged | |
| `POST /api/update/bundle` | uploads a bundle, streamed to disk | |
| `POST /api/update/install` | asks RAUC to install what is staged | |
| `GET /api/update/events` | install progress (SSE) | |
| `POST /api/update/mark-good` | marks the running slot good | |
| `GET /api/health` | every node's health, classified | |
| `GET /api/services` | services offered on the bus | |
| `GET /api/schema` | every schema name | |
| `GET /api/schema/<name>` | describes one, for building a request | |
| `POST /api/call` | calls a service | |

`/api/call` reports what actually happened rather than flattening everything to
200:

| | |
|---|---|
| `200` | a responder replied |
| `400` | the body was malformed, or the fields did not fit the request schema; `error` or `errors` says which |
| `502` | the call failed |
| `504` | nobody answered |

`timeout_ms` is optional (2000 by default) and clamped to 100..10000.

## Reflash

One upload at a time: a second while one is in flight gets `409`, as does an
upload while RAUC is installing. The upload is pre-flighted against
`std::filesystem::space` (`507` when short, with the numbers); the bundle
already staged counts as free, since the upload replaces it, and is removed
first only when that is what makes room. The body is written to
`/data/updates/.incoming-XXXXXX.raucb` (`mkstemps`, so `O_EXCL`), `fsync`ed and
closed -- either failing fails the upload -- then `rename()`d into place and the
directory `fsync`ed. Running out of space mid-upload is `507`, not `400`. The
temporary is unlinked on any failure, and startup sweeps stale `.incoming-*`.

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
upload_dir: "${REDLINE_DATA_DIR}/updates"
rauc_bus: system                  # "session" only for tools/rauc_stub
```

`bind_address` is widened here rather than in the code so that a developer
running this on a laptop is not exposed by accident.

## On the target

An instance of the `redline-node@.service` template, with a drop-in that
carries `RequiresMountsFor=/data` and creates `/data/updates`. Enabled through
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
- **No authentication, no TLS, no firewall, and it runs as root** like
  everything else on this image. `User=redline-web` with `ProtectSystem=strict` is cheap functionally
  (RAUC's D-Bus policy allows `context="default"` with no polkit) and is the
  obvious next step.
- **A plain node cannot be aimed at a router without recompiling.**
  `PUB_SUB_NO_DISCOVERY` is the only environment hook, and `--connect`/`--mode`
  live in `libs/cli`, which plain nodes do not use. Fine while multicast works
  on `lo`; not fine once `zenohd` binds one interface under an ACL.
- **Live plotting** (what `scope` does) is not here. The architecture reaches it
  without rework: a subscriber and a canvas.
