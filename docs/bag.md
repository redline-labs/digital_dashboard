# bag — recording and replaying the bus

`bag` captures everything on the zenoh bus into a seekable, compressed file and
plays it back later with its original timing. It is the counterpart to
`nodes/can_replay`, which could only ever replay a PCAN text file it had no way
of producing.

```bash
bag record drives/2026-08-06          # Ctrl-C to stop
bag info   drives/2026-08-06
bag play   drives/2026-08-06 --rate 2
```

## What a bag is

A **directory**, not a file:

```
drives/2026-08-06/
  metadata.yaml            the index: parts, topics, counts, drops
  2026-08-06_0000.mcap     rolled at --max-size (2 GiB) or --max-duration
  2026-08-06_0001.mcap
```

The split exists because a recorder gets killed. A power cut, an OOM, a panic on
the vehicle — and the recording you most want is the one that ended in the event
you were trying to capture. Rolling means a crash damages the last part rather
than the whole capture, and it keeps individual files copyable.

`BagReader` presents the parts as one continuous, time-ordered stream, so the
split is invisible to anything reading a bag.

## The format

[MCAP](https://mcap.dev), which was chosen for three reasons:

- **`capnproto` is a registered encoding** for both `schema_encoding` and
  `message_encoding` in [MCAP's format registry](https://mcap.dev/spec/registry).
  Our payloads go in byte-for-byte — no re-encoding, no envelope — and Foxglove
  Studio opens the files with no work from us.
- **Per-chunk compression** (zstd by default, lz4 available). A typical capture
  compresses to around a quarter of its raw size.
- **A summary section** carrying `ChunkIndex` (time range → file offset) and
  `Statistics`. That is what makes `bag info` instant on a recording of any size
  and `bag play --start-offset` a seek rather than a scan.

Files are ordinary MCAP and validate against Foxglove's own tooling:

```bash
mcap info   drives/2026-08-06/2026-08-06_0000.mcap
mcap doctor drives/2026-08-06/2026-08-06_0000.mcap
```

### What goes in a record

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
what a foreign consumer resolves the root node by — our registry name appears
nowhere in the node graph. The registry name goes on the channel, which is where
our own tools read it from.

**`Schema.data` is the schema as data.** `capnp::Schema` is a handle into code
the compiler generated for us and is meaningless to anyone who does not link it.
The descriptor is a serialized `CodeGeneratorRequest`, pruned to that schema's
transitive closure, emitted at build time by
`schemas/tools/schema_registry_plugin.cpp` — which is the only place in the tree
that has a `CodeGeneratorRequest` to prune. Pruning matters: unpruned, a bag
would carry the whole ~140 KB node graph once per topic.

## The two timestamps

`log_time` is when the recorder saw a message. `publish_time` is when the
publisher's zenoh session stamped it.

They are different, and both are recorded, because each is wrong in a different
way:

- `publish_time` is the publisher's **wall clock**. A unit that boots with a bad
  RTC before any time sync stamps wrong times, and zenoh's HLC re-stamps rather
  than rejects a timestamp too far in the future — so the error arrives looking
  ordinary. See `libs/pub_sub/include/pub_sub/timestamp.h`.
- `log_time` is our clock and is monotone, but includes transport delay.

Playback uses `log_time`. `inspect latency` is the difference.

A sample can arrive unstamped — an older build, or a session configured without
timestamping. `publish_time` then falls back to `log_time`, and **the count goes
in `metadata.yaml` and is printed by `bag info`**. A bag where half the messages
have a synthesised publish time should say so rather than look precise.

## What only liveliness can tell you

`bag record` snapshots the advertisement set as it changes, so `bag info` can
report a topic that was **advertised for the whole recording and never
published**.

That fact is unrecoverable afterwards. In a file that only holds messages,
"this topic produced nothing" and "this node was not running" are identical —
and they are completely different faults. `bag reindex` says so explicitly,
because a rebuilt index cannot restore it.

## Drops

A zenoh callback must not block: stalling an RX thread stalls the session for
everything, including the liveliness traffic other tools depend on. So the
recorder cannot apply backpressure. When the disk cannot keep up there are only
two options — grow without limit until the process is OOM-killed partway through
a recording, or drop.

`bag` drops, keeps the **newest**, and counts. The count lands in
`metadata.yaml`, is printed by `bag info`, and is warned about at the end of a
recording. A reported gap and an unreported one are entirely different things:
the second reads as a publisher that stopped.

If you are dropping: raise `--queue-depth`, try `--compression lz4` (faster,
larger), or write to a faster disk.

## Verbs

### `bag record <dir>`

| Option | |
|---|---|
| `-k, --key` | key expression to record, repeatable; default `**` |
| `--compression` | `none`, `lz4`, `zstd` (default) |
| `--compression-level` | 1 (fastest) – 9 (smallest); 0 = codec default |
| `--chunk-size` | uncompressed bytes per chunk (default 4 MiB) |
| `--max-size` | roll past this many bytes (default 2 GiB; 0 disables) |
| `--max-duration` | roll past this many seconds (0 disables) |
| `--queue-depth` | messages buffered before dropping (default 8192) |
| `-d, --duration` | stop after this many seconds |

### `bag info <dir>`

Reads the index only — no part is opened, no message is read. Reports duration,
counts, per-topic rates, silent topics, drops, unstamped messages, and any
problems (a missing part, a part with no summary). `--json` for scripting.

### `bag play <dir>`

| Option | |
|---|---|
| `-r, --rate` | speed multiplier; 0 = as fast as possible |
| `-l, --loop` | replay from the start when it ends |
| `-s, --start-offset` | begin this many seconds in (a seek, not a scan) |
| `-d, --duration` | play only this many seconds |
| `-k, --key` | only replay these keys, repeatable |
| `--remap old=new` | republish under a different key |
| `--prefix p` | prepend to every key |

Playback uses `detail::BytePublisher`, so a replayed topic **also declares its
liveliness advertisement**. Scope's picker and `inspect list` see a replay
exactly as they see a live publisher, with its schema and owning session:

```bash
bag play drives/2026-08-06 --rate 2 &
inspect list        # the recorded topics, advertised
inspect hz -k 'vehicle/**'   # twice their recorded rate
```

Use `--prefix replay` to run a replay alongside live nodes without both
publishing the same key.

### `bag reindex <dir>`

Rebuilds `metadata.yaml` from the `.mcap` files on disk, for a recording whose
recorder was killed before it could write one. `--dry-run` reports without
writing.

It preserves the drop count from any existing index — a rebuild that reset it to
zero would silently claim a lossy recording was complete.

## Reading a damaged recording

A part whose writer died has no summary: no `ChunkIndex`, no `MessageIndex`, no
footer. `BagReader` handles this, and the way it does is worth knowing about
because the obvious implementation fails silently:

`readSummary(AllowFallbackScan)` **succeeds** on a torn part — it scans the data
section and produces perfectly good `ChunkIndex` records. What it cannot produce
is `MessageIndex` records, and `LogTimeOrder` needs those to merge channels.
Asked for it anyway, the reader yields **zero messages** while reporting success.

So the reader checks for message indexes specifically, and falls back to
`FileOrder` — a sequential read that stops at the first damaged record. Messages
come back slightly out of order and all of them are recovered, which is
unambiguously the better trade. `libs/bag/test_damage.cpp` pins this.

## Testing

```bash
ctest --test-dir build -L bag        # all unit; no zenoh session anywhere
```

`libs/bag` is a library rather than code inside `nodes/bag` precisely so those
tests exist: round trip, splitting, seeking, torn parts, missing parts, garbage
parts, the index, and the drop-counting queue — every one a failure that is
silent in production.

## What is not here yet

`scope::DataSource` (`scope/include/scope/data_source.h`) was designed for a
recorded source — `SourceCaps{seekable, t_begin, t_end}`, `seek()`,
`setPlaying()` are all present and all no-ops on the live source. A
`RecordedSource` over `libs/bag` would let scope scrub a recording directly
rather than replaying it onto the bus. The library exists in the shape that
makes that a small change.
