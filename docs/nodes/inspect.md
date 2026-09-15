---
title: inspect
parent: Nodes
---

# inspect

## Overview

`inspect` is the command-line window onto the zenoh bus: it lists what is
advertised, prints a stream decoded against its schema, measures rates and
delays, describes schemas, and calls services or publishes messages built from
JSON. It needs nothing but a zenoh session on the same network as the nodes it
is looking at; the `schema` verb needs no bus at all, because the schema
registry is compiled in. It does not record or replay the bus, which is
[bag](bag.html)'s job, and it does not open any hardware.

## Running it

The binary is `build/nodes/inspect/inspect`. It takes only command-line
options; there is no configuration file. The first bare argument is the verb,
and every option can go before or after it, so `inspect --json list` and
`inspect list --json` are the same command.

```bash
./build/nodes/inspect/inspect                      # the verb list, exit code 2
./build/nodes/inspect/inspect list
./build/nodes/inspect/inspect echo vehicle/can0/rx -n 5
./build/nodes/inspect/inspect help call            # same as `call --help`
./build/nodes/inspect/inspect list --json | jq '.[].key'
```

Results go to stdout with no log decoration, and diagnostics go to stderr, so a
piped `--json` stays parseable while warnings still reach the terminal.

| Global option | Meaning |
|---|---|
| `--json` | Emit the result as JSON on stdout instead of aligned text |
| `-v`, `--debug` | Debug logging, on stderr |
| `--connect <endpoint>` | Connect to a router, for example `tcp/192.168.1.10:7447`; repeatable. The default is peer-mode discovery |
| `--mode peer\|client` | The zenoh session mode |
| `-h`, `--help` | Usage for the program, or for the verb it follows |

The verbs, in the order the program lists them:

| Verb | What it does | Options that matter |
|---|---|---|
| `list [key]` | Topics on the bus, from liveliness advertisements, so a topic appears the moment its publisher starts whether or not it has sent anything | `-k` filter with `*` and `**` wildcards (default `**`), `-o <ms>` also sample traffic for that long, `--by-node`, `--all` to include publishers that have gone away |
| `info <key>` | One topic: schema, fields, owning node, and traffic with `-o` | `-o <ms>` |
| `echo <key>` | Messages as they arrive, decoded against the schema the publisher named | `-n` stop after that many, `-t` show publish time and origin session, `--hex` never decode |
| `watch [key]` | A live table of the whole bus: rate, bandwidth, owner | `-i` seconds between refreshes (default `1.0`), `-n` stop after that many, `--no-clear` |
| `hz [key]` | Message rate, with the mean, min, max and standard deviation of the gaps | `-i`, `-n` |
| `bw [key]` | Payload bytes per second per key | `-i`, `-n` |
| `latency [key]` | Arrival minus the publisher's timestamp, over samples that carried one | `-i`, `-n` |
| `nodes` | Our processes by name, joined against zenoh's session list | `--all` |
| `health` | What every node says about itself: state, heartbeat age, uptime, restarts, first problem | `--all` include exited, `--checks` list every check, `-w` keep refreshing with `-i` and `--no-clear`, `--wait` seconds to collect first (default `1.5`) |
| `services` | Callable services and their request and response schemas | `--all` |
| `call <key>` | Call a service with a JSON request | `-d` JSON object, `-` for stdin, or `@file` (default `{}`); `-s` request schema when the service is not advertised; `-t` reply timeout in ms (default `2000`) |
| `schema [name]` | The compiled-in registry: list it, or describe one schema's fields | `-f` substring filter, case-insensitive |
| `publish <key>` | Publish a message built from JSON | `-s` schema name (required), `-d` as for `call`, `-r` republish this many times per second, `-n` how many (default `1`) |

`echo`, `watch`, `hz`, `bw` and `latency` run until Ctrl-C or their `-n`
count. `bw` counts capnp payload bytes only; zenoh's own framing is not visible
to a subscriber and is not counted.

{: .note }
`latency` measures the publisher's clock as much as the link. Samples without a
timestamp are excluded, and a key where none carried one prints no latency
figures rather than a zero.

**Exit codes** are stable so scripts can branch on them:

| Code | Meaning |
|---|---|
| `0` | The command ran. This includes `info` on a topic nobody advertises, which is an answer, not a failure |
| `1` | It tried and could not: no zenoh session, a subscription that would not declare, a service that did not reply in time, a publish that failed, or a stream interrupted before `-n` was reached |
| `2` | You asked wrongly: no verb, an unknown verb, an unrecognised or leftover argument, a missing required option, JSON that does not parse, a schema not in the registry, a request the schema rejected, or a service not advertised and no `--schema` given |

A malformed request is caught before anything is sent: `publish` and `call`
validate the JSON against the schema and return `2` with the field errors
listed, having sent nothing.

## Topics

`inspect` publishes nothing of its own. It subscribes to whatever key
expression a verb is given, and it reads the liveliness advertisement space
that every publisher and service declares into. `publish` puts on the key you
name, with the schema you name, exactly as a node would.

## Services

None. `call` is a client for other nodes' services; `services` lists them with
their request and response schemas, and `schema <name>` shows the fields a
request needs.

## Health

`health` reads what every node publishes about itself and prints one row each:
its state, how old its heartbeat is, how long it has been up, how many times it
has restarted, and the first check that is not ok. It waits `--wait` seconds
first, because a node that has just started has not published yet and would
otherwise read as silent.

```
NODE                 STATE         AGE   UPTIME RESTARTS  PROBLEM
can_bridge           ok           0.3s    4m12s        0  -
megasquirt           degraded     0.2s      31s        0  can_rx: nothing for 2.4 s
map_server           gone            -    1m02s        1  -
```

It exits non-zero when any node is not ok, so a script can gate on it. `--all`
includes nodes that have exited or gone, `--checks` lists every check under its
node, and `-w` keeps refreshing. [node_health](../libs/node_health.html)
explains the verdicts, in particular `late` (its heartbeat stopped, the process
did not) against `gone` (the process did).

## Troubleshooting

**`list` prints nothing and exits 0.** Either nothing is running, or this
process cannot see the peers. Peer discovery is multicast on the local link, so
a bus on another subnet or behind a router needs `--connect` pointing at it.
The tell is `nodes`: a healthy bus lists our processes by name, and an empty
`nodes` alongside an empty `list` is a discovery problem rather than an idle
system. A failure to open a session at all is reported and exits `1`, so a
silent empty list is never that.

**`echo` prints hex instead of fields.** The publisher named a schema this
build of `inspect` does not have in its registry, and `echo` says so on stderr
once per key before falling back to a hex dump. Rebuild `inspect` from the same
tree as the publisher, or check `schema -f <name>` to see whether the registry
knows it under a different name. A payload whose length is not a whole number
of eight-byte capnp words is also hex-dumped, with a warning; that is a
publisher bug, not a registry gap.

**`hz` says `(no traffic)` while `list` shows the topic.** The publisher is
alive and advertised but idle. `list` reads advertisements and cannot tell an
idle topic from a busy one; `hz` reads traffic and cannot tell an idle topic
from an absent one. Use `info <key> -o 2000` to get both answers at once. The
same split explains a `list -k` filter that omits a topic: the local wildcard
matcher only supports `*` and `**`, so a pattern using other zenoh forms
matches nothing rather than erroring.

**`call` returns `1` with "No reply within the timeout".** Nothing answered on
that key in `-t` milliseconds. Check `services` for the exact key, and raise
`-t` for a node whose own wait is longer than two seconds, because a client
that gives up first reports a transport failure instead of the node's answer.
