---
title: bag format
parent: Design notes
---

# bag format

What a bag recording is made of and why, and the places where the obvious
implementation is wrong. How to record, inspect and replay one is on
[bag](../nodes/bag.html).

## The format

A bag is a directory of [MCAP](https://mcap.dev) parts plus a `metadata.yaml`
index. The split exists because a recorder gets killed. A power cut, an OOM, a
panic on the vehicle, and the recording you most want is the one that ended in
the event you were trying to capture. Rolling means a crash damages the last
part rather than the whole capture, and it keeps individual files copyable.
`BagReader` presents the parts as one continuous, time-ordered stream, so the
split is invisible to anything reading a bag.

MCAP was chosen for three reasons.

`capnproto` is a registered encoding for both `schema_encoding` and
`message_encoding` in [MCAP's format registry](https://mcap.dev/spec/registry).
Our payloads go in byte-for-byte, with no re-encoding and no envelope, and
Foxglove Studio opens the files with no work from us.

Per-chunk compression, zstd by default with lz4 available. A typical capture
compresses to around a quarter of its raw size.

A summary section carrying `ChunkIndex` (time range to file offset) and
`Statistics`. That is what makes `bag info` instant on a recording of any size
and `bag play --start-offset` a seek rather than a scan.

The index holds the things MCAP has no slot for: how many messages were dropped,
and which topics were advertised but never published. It is rewritten on every
roll as well as at close, so an in-progress recording has an index describing
every part that has finished.

## What goes in a record

| MCAP | Ours |
|---|---|
| `Channel.topic` | the zenoh key |
| `Channel.messageEncoding` | `capnproto` |
| `Channel.metadata["redline/schema"]` | our registry name, e.g. `EngineRpm` |
| `Schema.name` | capnp's qualified name, e.g. `engine_rpm.capnp:EngineRpm` |
| `Schema.encoding` / `.data` | `capnproto` / a serialized `CodeGeneratorRequest` |
| `Message.data` | the exact bytes zenoh carried |
| `Message.logTime` | arrival at the recorder |
| `Message.publishTime` | the publisher's zenoh timestamp |

**Two schema names, deliberately.** `Schema.name` is capnp's, because that is
what a foreign consumer resolves the root node by; our registry name appears
nowhere in the node graph. The registry name goes on the channel, which is where
our own tools read it from.

**`Schema.data` is the schema as data.** `capnp::Schema` is a handle into code
the compiler generated for us and is meaningless to anyone who does not link it.
The descriptor is a serialized `CodeGeneratorRequest`, pruned to that schema's
transitive closure, emitted at build time by
`schemas/tools/schema_registry_plugin.cpp`, which is the only place in the tree
that has a `CodeGeneratorRequest` to prune. Pruning matters: unpruned, a bag
would carry the whole ~140 KB node graph once per topic.

## The two timestamps

`log_time` is when the recorder saw a message. `publish_time` is when the
publisher's zenoh session stamped it. They are different, and both are recorded,
because each is wrong in a different way.

`publish_time` is the publisher's wall clock. A unit that boots with a bad RTC
before any time sync stamps wrong times, and zenoh's HLC re-stamps rather than
rejects a timestamp too far in the future, so the error arrives looking
ordinary. See `libs/pub_sub/include/pub_sub/timestamp.h`. `log_time` is our
clock and is monotone, but includes transport delay.

Playback uses `log_time`. `inspect latency` is the difference.

A sample can arrive unstamped: an older build, or a session configured without
timestamping. `publish_time` then falls back to `log_time`, and the count goes in
`metadata.yaml` and is printed by `bag info`. A bag where half the messages have
a synthesised publish time should say so rather than look precise.

## What only liveliness can tell you

`bag record` snapshots the advertisement set as it changes, so `bag info` can
report a topic that was advertised for the whole recording and never published.

That fact is unrecoverable afterwards. In a file that only holds messages, "this
topic produced nothing" and "this node was not running" are identical, and they
are completely different faults. `bag reindex` says so explicitly, because a
rebuilt index cannot restore it.

## Drops

A zenoh callback must not block: stalling an RX thread stalls the session for
everything, including the liveliness traffic other tools depend on. So the
recorder cannot apply backpressure. When the disk cannot keep up there are only
two options: grow without limit until the process is OOM-killed partway through
a recording, or drop.

`bag` drops, keeps the newest, and counts. The count lands in `metadata.yaml`,
is printed by `bag info`, and is warned about at the end of a recording. A
reported gap and an unreported one are entirely different things: the second
reads as a publisher that stopped. `bag reindex` preserves the drop count from
any existing index, because a rebuild that reset it to zero would silently claim
a lossy recording was complete.

Scope's in-memory capture has the same property from the file's point of view:
when File > Save Recording drains it through `BagWriter`, the capture's
eviction count is recorded as `dropped_messages`, because that is exactly what
an evicted message is.

## Verifying independently

`bag verify` deliberately re-implements an MCAP parser (`libs/bag/validate.cpp`),
walking the raw bytes with no reference to mcap's own code. That independence is
the whole point: our reader is a thin layer over mcap's, which is lenient, so the
two can agree with each other about a malformed file. They did.

Rolling a part emitted a summary listing schema and channel ids the data
section never contained. The round-trip, splitting and seeking tests all passed,
and `bag info` reported correct counts. The only thing that noticed was `mcap
doctor`, a Go binary that is not installed, not in CI, and not run by habit.

`bag verify` now catches that same file, with the same diagnosis, as part of
`ctest -L bag`.

## Reading a damaged recording

A part whose writer died has no summary: no `ChunkIndex`, no `MessageIndex`, no
footer. `BagReader` handles this, and the way it does is worth knowing about
because the obvious implementation fails silently.

`readSummary(AllowFallbackScan)` succeeds on a torn part. It scans the data
section and produces perfectly good `ChunkIndex` records. What it cannot produce
is `MessageIndex` records, and `LogTimeOrder` needs those to merge channels.
Asked for it anyway, the reader yields zero messages while reporting success.

So the reader checks for message indexes specifically, and falls back to
`FileOrder`, a sequential read that stops at the first damaged record. Messages
come back slightly out of order and all of them are recovered, which is
unambiguously the better trade. `libs/bag/test_damage.cpp` pins this.

## Reading one in scope

`scope` links `libs/bag` rather than shelling out to the binary, so a recording
is a `scope::DataSource`. Two things about that path are worth knowing from
here.

**`BagReader::forEach` is not a per-frame call.** It constructs and opens an
`mcap::McapReader` per part per call, and on a part with no summary it falls
back to scanning the whole data section. Scope therefore decodes each signal
once, on a background thread, into a flat sample vector, and scrubbing is a
slice out of that. Driving `forEach` from a slider would re-open files thirty
times a second and re-scan a torn recording every one of them.

**`BagMessage::schema` is the registry name**, not an encoding string, so it
does not go to `ExpressionEvaluator::checkPublishedSchema()`. That takes
`application/capnp;EngineRpm` and would match neither of its branches, silently
checking nothing. Compare it against `enum_traits<schema_type_t>::to_string()`
instead.

## Why libs/bag is a library

`libs/bag` is a library rather than code inside `nodes/bag` precisely so that
`ctest -L bag` exists without a zenoh session anywhere: round trip, splitting,
seeking, torn parts, missing parts, garbage parts, the index, and the
drop-counting queue. Every one is a failure that is silent in production. What
is left in `nodes/bag` is argument handling, the subscribe/publish loops, and
presentation.

## What is not here yet

Trimming or exporting a sub-range of a recording, and spilling a scope capture
to disk when it outgrows its memory cap.
