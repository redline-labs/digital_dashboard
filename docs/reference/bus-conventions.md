---
title: Bus conventions
parent: Reference
nav_order: 3
---

# Bus conventions

The bus is zenoh, in peer mode, carrying Cap'n Proto messages. These are the
rules every publisher, subscriber and tool in the tree follows, collected in one
place. `libs/pub_sub` implements them; nothing else should reimplement them.

## Keys

Topic keys use `[A-Za-z0-9_-/]` and nothing else, and this is enforced in the
editor, at config load and in the publisher through `pub_sub::topicKeyProblem()`.
The reasons each other character is out: `%` is the mangling separator, `@`
makes a segment verbatim and therefore invisible to every wildcard subscription,
and `* $ ? #` are rejected by zenoh outright. Each of those fails silently, so
the charset is checked rather than trusted.

Node topics follow `nodes/<node>/<stream>`. CAN frames from `can_bridge` are
`vehicle/<channel>/rx` and `vehicle/<channel>/tx`, where the channel name is the
one in its config, deliberately separate from the device behind it; its own
status and bit-rate service sit under `vehicle/can/`.

## The schema stamp

Every sample carries its schema as the zenoh encoding
`application/capnp;<SchemaName>`. Subscribers check it before decoding.
There is no out-of-band registry of key to schema, on purpose: zenoh has no
retained messages, and per-sample self-description is what lets a tool that
joins late identify a stream from the first message it sees.

{: .warning }
Decoding against the wrong schema does not throw. The field offsets land on
different bytes and produce a plausible wrong number. The stamp is the only
defence.

## Liveliness

Three key spaces announce what is on the bus, all under a leading `@` segment so
zenoh treats them as verbatim and no `**` subscriber ever sees them as topics:

| Key | Declared by | Meaning |
|---|---|---|
| `@redline/adv/<Schema>/<topic>/<zid>` | every publisher (`detail::BytePublisher`) | this session offers this topic with this schema |
| `@redline/node/<zid>/<name>` | `pub_sub::NodeIdentity`, one per process | this session is the node called `<name>` |
| `@redline/svc/<zid>/<Req>/<Resp>/<key>` | every `ZenohService` | this session answers this service |

They join on the zid. A picker can therefore list a topic before it has
published anything, and `inspect nodes` can put a name to a session. The
advertisement is additive: the per-sample stamp stays authoritative, and both
are derived from the same constructor arguments so they cannot disagree.

Every parser of these keys accepts extra trailing segments and ignores them. A
directory drops what it cannot parse, so a reader that rejected an unknown
longer form would turn the first added field into a silent, total outage for
every build that predates it, and an empty picker looks exactly like a bus with
no publishers.

## Timestamps

Samples carry a publish timestamp because `SessionManager::buildConfig()`
enables `timestamping`; zenoh only stamps in router mode by default and every
session here is a peer. `SampleMeta` exposes it with the origin zid, the session
that actually sent the bytes. Convert with `pub_sub::ntp64ToUnixNanos()`: NTP64
is `seconds << 32 | fraction`, and a naive read is off by 2^32 and yields a
plausible wrong time. It is the publisher's wall clock and can be quietly wrong;
`pub_sub/timestamp.h` says when to trust it.

## Services

A service is a zenoh queryable with a request and a response schema, declared
with `ZenohService`. `inspect call <key> --data '{json}'` calls one from the
command line and [switchboard](../apps/switchboard.html) from a form; both go
through `pub_sub::callService`, so they cannot disagree about what a request
means. A handler that throws answers with an error reply carrying the
exception's message rather than taking the node down, and a malformed request
still gets a default-constructed response. "No reply" means either that no node
serves the key any more or that none answered within the timeout; zenoh cannot
tell those apart.

## Threads

Zenoh callbacks run on zenoh threads and must not block. In a Qt program, hop to
the GUI thread with `QMetaObject::invokeMethod(obj, lambda, Qt::QueuedConnection)`;
`libs/dashboard_widgets/include/dashboard/expression_subscription.h` is the
established shape. Qt owns exactly one thread.

## Discovery and tests

`PUB_SUB_NO_DISCOVERY=1` keeps a session off the machine's bus. Every `net`
test runs with it set, so tests neither find nor are found by anything else on
the machine, and two test runs cannot perturb each other.
