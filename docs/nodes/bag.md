---
title: bag
parent: Nodes
redirect_from: /bag.html
---

# bag

## Overview

`bag` captures everything on the zenoh bus into a seekable, compressed
recording and plays it back later with its original timing.

It records the bus, which is the difference between it and `can_bridge`'s
`record_trc:`. A bag holds every topic in whatever schema each carries; a PCAN
`.trc` holds the frames of one CAN channel, in a text format PCAN-Explorer and
PCAN-View can open. Reach for a bag to replay a session into the dashboard or
open it in scope, and for a trace to hand a bus to someone else's tooling.

A bag is a directory, not a file:

```
drives/2026-08-06/
  metadata.yaml            the index: parts, topics, counts, drops
  2026-08-06_0000.mcap     rolled at --max-size (2 GiB) or --max-duration
  2026-08-06_0001.mcap
```

The parts are ordinary [MCAP](https://mcap.dev) files that Foxglove's tooling
opens unchanged, split so that a recorder killed mid-capture damages the last
part rather than the whole recording. Anything reading a bag sees them as one
time-ordered stream. The format, what each record carries, the two timestamps
and why drops are counted rather than prevented are on the design page
[bag format](../design/bag-format.html).

## Running it

The build target is `bag_tool` (the name `bag` belongs to `libs/bag`) and the
binary is `./build/nodes/bag/bag`.

```bash
./build/nodes/bag/bag record drives/2026-08-06          # Ctrl-C to stop
./build/nodes/bag/bag info   drives/2026-08-06
./build/nodes/bag/bag play   drives/2026-08-06 --rate 2
```

`--json` and `--debug` are global and work in either position.

## Verbs

### `bag record <dir>`

Subscribes to the bus and writes a recording until Ctrl-C or `--duration`. At
the end it prints what it wrote and warns, loudly, if any message was dropped
or arrived without a publish timestamp.

| option | default | |
|---|---|---|
| `-k, --key` | `**` | key expression to record, repeatable |
| `--compression` | `zstd` | chunk codec: `none`, `lz4` or `zstd` |
| `--compression-level` | `0` | 1 (fastest) to 9 (smallest); 0 is the codec's default |
| `--chunk-size` | 4 MiB | uncompressed bytes buffered before a chunk is flushed |
| `--max-size` | 2 GiB | roll to a new part past this many bytes; 0 disables |
| `--max-duration` | `0` | roll to a new part past this many seconds; 0 disables |
| `--queue-depth` | `8192` | messages buffered between the bus and the writer thread |
| `-d, --duration` | `0` | stop after this many seconds; 0 means until Ctrl-C |
| `--quiet` | | do not print the progress line |

### `bag info <dir>`

Reads the index only, so it is instant on a recording of any size. Reports when
and by what it was recorded, span, duration, message count, size and parts,
drops, unstamped messages, per-topic counts and rates, topics advertised and
never published (`SILENT`), and problems such as a missing part or a part with
no summary. `--json` for scripting.

### `bag play <dir>`

Republishes the recording with its original timing.

| option | default | |
|---|---|---|
| `-r, --rate` | `1.0` | speed multiplier; 0 means as fast as possible |
| `-l, --loop` | | replay from the start when the recording ends |
| `-s, --start-offset` | `0` | begin this many seconds in (a seek, not a scan) |
| `-d, --duration` | `0` | play only this many seconds; 0 means to the end |
| `-k, --key` | everything | only replay these keys, repeatable |
| `--remap old=new` | | republish `old` as `new`, repeatable |
| `--prefix p` | | prepend to every key |

A replayed topic also declares its liveliness advertisement, so scope's picker
and `inspect list` see a replay exactly as they see a live publisher, with its
schema and owning session:

```bash
./build/nodes/bag/bag play drives/2026-08-06 --rate 2 &
inspect list                 # the recorded topics, advertised
inspect hz -k 'vehicle/**'   # twice their recorded rate
```

To play a recording into the dashboard, start the dashboard as usual and run
`bag play` against the same bus; the widgets cannot tell a replay from a live
node. Use `--prefix replay` to run a replay alongside live nodes without both
publishing the same key.

### `bag verify <dir|file>`

Structural validation against the MCAP spec: magic, record framing, section
ordering, chunk CRCs, and whether the summary describes records the data
section contains. For a directory it also cross-checks `metadata.yaml` against
the files: a part on disk the index does not list, or a count that disagrees.
`-q, --quiet` reports only through the exit code, 0 when clean and 1 otherwise,
so it is usable from a script and from CI. It needs nothing installed;
Foxglove's own `mcap info` and `mcap doctor` are a cross-check the build does
not depend on.

### `bag reindex <dir>`

Rebuilds `metadata.yaml` from the `.mcap` files on disk, for a recording whose
recorder was killed before it could write one. `-n, --dry-run` reports without
writing. The index is written on every roll as well as at close, so a recorder
killed after its first roll leaves a readable bag; reindex is the answer for
one killed before that. It preserves the drop count from any existing index.

## Opening one in scope

`scope` links `libs/bag` rather than shelling out to the binary, so a recording
is a `scope::DataSource` and every panel scrubs it:

```bash
./build/apps/scope/scope --bag drives/2026-08-06
```

`--bag` and `--online` are mutually exclusive; start with `--bag` and go online
from the toolbar. Scope also writes bags: File > Save Recording drains its live
capture into an ordinary bag that `bag info`, `bag verify` and `bag play` all
accept. See [scope](../apps/scope.html).

## Troubleshooting

**Drops.** `bag record` warns at the end that messages were dropped, `bag info`
shows a non-zero `dropped` line, or a topic in a recording appears to stop and
resume. The recorder cannot apply backpressure to the bus, so when the disk
cannot keep up it drops the oldest queued messages and counts them. Raise
`--queue-depth`, try `--compression lz4` (faster, larger), or write to a faster
disk.

**Unstamped messages.** `bag info` reports messages that arrived without a
publish timestamp; their `publish_time` was taken from arrival. The publisher is
an older build or a session configured without timestamping. Playback is
unaffected, because it uses `log_time`.

**The recorder was killed.** If there is a `metadata.yaml`, `bag info` still
works and lists the parts that finished; if there is not, run `bag reindex`. A
part whose writer died has no summary, so the reader recovers its messages in
file order rather than log-time order, and a replay of that part may be
slightly out of order across topics. A reindexed bag cannot say which topics
were `SILENT`, because only the live recorder knew.

**`bag verify` fails on a file `bag info` was happy with.** That is what it is
for: `info` reads the index and the reader is lenient, while `verify` walks the
raw bytes independently. Trust `verify`.

## Tests

```bash
ctest --test-dir build -L bag        # all unit; no zenoh session anywhere
```
