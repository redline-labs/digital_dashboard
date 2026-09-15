---
title: bag (library)
parent: Libraries
---

# bag

## Overview

Recording and replaying the bus, as MCAP: a writer that rolls parts and keeps
an index, a reader that presents the parts as one time-ordered stream, the
bounded queue between zenoh callbacks and the writer thread, a structural MCAP
validator written from the spec, an index rebuilder, and the playback key
rules. The `bag` tool ([bag](../nodes/bag.html)) is a thin set of verbs over
this; that page covers the format, the two timestamps and the verbs.

It is a library rather than code inside `nodes/bag` for two reasons. The tests
are pure file I/O with no zenoh session anywhere, which is only possible if
reading and writing are separable from the tool that drives them, and every one
of them exercises a failure that is silent in production. And scope's
`DataSource` seam was designed for a recorded source; scope links this rather
than shelling out to a binary. Nothing here opens a session or touches Qt.

## Public headers

| Header | |
| --- | --- |
| `bag/metadata.h` | `bag_metadata_t`, `bag_part_t`, `bag_topic_t` as reflected structs; `loadMetadata()`, `saveMetadata()`, `metadataPath()`. |
| `bag/writer.h` | `BagWriter` and `WriterOptions` (zstd, 4 MiB chunks, 2 GiB parts by default): `write()`, `noteAdvertised()`, `noteDropped()`, `close()`. |
| `bag/reader.h` | `BagReader` and `BagMessage`: `forEach()` over a time range, `problems()`, `descriptorFor()`. |
| `bag/queue.h` | `MessageQueue` and `QueuedMessage`: bounded, drop-oldest, counted. |
| `bag/validate.h` | `validateMcapFile()`, `validateBag()`, `hasCompleteEnding()`, `ValidationReport`. |
| `bag/rebuild.h` | `rebuildMetadata()`: an index from the parts on disk. |
| `bag/playback.h` | `parseRemaps()` and `resolvePlaybackKey()`: `--remap` and `--prefix`. |

## Using it

Link the CMake target `bag`. `reflection` comes with it publicly because the
index is a reflected struct. The recorder's shape is one queue between the
zenoh callbacks and one writer thread, as in `nodes/bag/record.cpp`:

```cpp
bag::BagWriter writer(directory, options);        // bag::WriterOptions
bag::MessageQueue queue(8192);

pub_sub::RawSubscriber sub("**", pub_sub::RawSubscriber::InfoHandler(
    [&](const std::vector<std::uint8_t>& payload, const pub_sub::RawSubscriber::SampleInfo& info)
    {   // zenoh thread: copy into the queue and return
        queue.push({std::string(info.keyexpr), std::string(info.schema_name),
                    std::string(info.origin_zid), payload, nowNanos(), info.publish_time_nanos});
    }));

while (auto m = queue.pop())                       // writer thread
    writer.write(m->key, m->schema, m->payload, m->log_time_ns, m->publish_time_ns, m->origin_zid);
writer.noteDropped(queue.dropped());
writer.close();
```

Reading is `bag::BagReader reader(directory)` and `reader.forEach(callback)`,
or `forEach(start_ns, end_ns, callback)` for a window; return false from the
callback to stop early.

## Behaviour worth knowing

A bag is a directory: `metadata.yaml` plus `.mcap` parts rolled at a size or
duration limit. `BagReader` finds parts through the index and nowhere else, so
a recording whose recorder died before writing one is unreadable until
`rebuildMetadata()` runs. The index is rewritten after every roll and written
by temporary-and-rename, so a crash mid-write leaves the previous one intact.

`BagWriter` is not thread-safe, deliberately. A zenoh callback must not block
or throw, so the intended shape is a callback that copies into the queue and
one writer thread that drains it; making the writer lockable would invite
calling it from the callback. `write()` returning false means stop: continuing
would produce a file whose index does not describe its contents. `close()` is
safe to call twice and the destructor calls it, but only an explicit call tells
you whether it worked.

`MessageQueue` is bounded because the recorder cannot apply backpressure. When
the disk cannot keep up it drops the oldest and counts; the count lands in
`metadata.yaml` as `dropped_messages`, and a gap that is reported is a recorder
problem where an unreported one reads as a publisher that stopped. `pop()`
returns `nullopt` only when stopped and drained, so a writer loop cannot lose a
message that was queued before the stop.

{: .warning }
`BagMessage` fields are views into buffers the reader owns and are valid only
inside the callback; the payload points into a decompressed chunk the next
message may replace. Copy anything you keep. `BagMessage::schema` is the
registry name (`EngineRpm`), not an encoding string, so it must not go to
`ExpressionEvaluator::checkPublishedSchema()`, which would match neither of its
branches and check nothing.

`publish_time_ns` equals `log_time_ns` when a sample arrived unstamped, and
`unstamped_messages` in the index says how often that happened; a bag where it
is large has a publish time that is really an arrival time.

**A torn last part is the expected state after a crash**, not an edge case,
and the obvious reader code fails silently on it. `McapReader::readSummary()`
returns success on a truncated file and produces good `ChunkIndex` records but
no `MessageIndex`, and `LogTimeOrder` then yields zero messages while reporting
success. The reader checks for message indexes and falls back to `FileOrder`,
recovering everything up to the tear slightly out of order. `hasCompleteEnding()`
reads the trailing magic, the one thing a killed writer cannot have written;
`part.complete = summary.ok()` would call every torn part complete.

`forEach()` opens an `mcap::McapReader` per part per call and scans a torn
part's whole data section. It is not a per-frame call; scope decodes each
signal once on a background thread and scrubs a vector.

`descriptorFor()` returns the schema as data, a serialized
`CodeGeneratorRequest` loadable with `capnp::SchemaLoader`, so a consumer that
does not link this build's generated headers can still decode. It is empty for
a message whose schema this build did not know; the bytes are stored verbatim.

`validate.cpp` re-implements an MCAP parser from the spec with no mcap code,
because our reader sits on mcap's lenient one and the pair once round-tripped
a malformed file cleanly; only Foxglove's `mcap doctor` disagreed. It checks
the invariants a writer can plausibly get wrong, not every field of every
record. `rebuildMetadata()` preserves the drop count from any existing index,
since a rebuild that reset it would claim a lossy recording was complete, and
cannot restore advertised-but-silent topics, which only the live recorder knew.

`resolvePlaybackKey()` applies a remap before a prefix. A remap names a key as
recorded, and the other order would make `--remap` stop matching the moment
`--prefix` was added, dropping those messages from a replay with nothing
logged.

## Tests

All `unit`; nothing here opens a zenoh session.

| Target | Labels | Proves |
| --- | --- | --- |
| `bag_test_roundtrip` | `bag unit` | Contents, order, splitting and seeking: the four properties the format was chosen for. |
| `bag_test_damage` | `bag unit` | A torn recording yields every message up to the tear, says it is torn, and does not hang. |
| `bag_test_metadata` | `bag unit` | The index round-trips through YAML, is written by rename, and a malformed or newer one is handled. |
| `bag_test_queue` | `bag unit` | Drops are counted exactly, the newest is kept, and `pop()` drains before reporting stopped. |
| `bag_test_format` | `bag unit` | What we write is valid MCAP against the spec, not against our own reader. The only coverage lz4 has. |
| `bag_test_decode` | `bag unit` | A message decodes from the recording alone, through `SchemaLoader` on the stored descriptor. |
| `bag_test_rebuild` | `bag unit` | An index rebuilt from the parts carries over the drop count. |
| `bag_test_playback` | `bag unit` | Remap before prefix, and a malformed remap is reported without discarding the others. |
| `bag_test_edges` | `bag unit` | Empty and oversized payloads, duration rolling, write after close, an unwritable directory, backwards ranges, reader reuse. |
