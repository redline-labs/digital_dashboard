---
title: calibration_store
parent: Libraries
---

# calibration_store

## Overview

What the state estimator has learned about the car's installation, kept
between sessions: a SQLite file of rows, one per group per write, never
updated and never deleted, so how a mounting or a lever arm was learned can be
read back with the `sqlite3` shell.

It is generic on purpose. A row is a group name, a hash of the priors it was
learned against, a model version, and a mean and covariance as numbers. What
those numbers mean, when a row is worth writing and when a stored one still
applies are all [vehicle_estimator](vehicle_estimator.html)'s business
(`calibration.h`), and this library links nothing of it. The node that joins
the two is [state_estimator](../nodes/state_estimator.html). The reasoning is
in the [design note](../design/state-estimation.html#learned-calibration).

## Public headers

| Header | |
| --- | --- |
| `calibration_store/store.h` | `Store` (`open`, `append` one row or a batch, `latest`, `history`, `recovered`), `Row`, `Recovery`, `Error`, `Result<T>`, `structuralProblem()`. |

## Using it

Link `calibration_store`. SQLite comes from the amalgamation in
`third_party/sqlite3.cmake`, and like `libs/track_store` this stays out of the
GUI apps, which reach SQLite through Qt's own copy.

```cpp
auto store = calibration_store::Store::open(path);   // makes the directory and file
if (!store) { /* store.error().kind, .message */ }

store->append(row);                                   // returns the new id

auto row = store->latest("mounting", prior_hash, model_version,
                         [](const calibration_store::Row& r) -> std::optional<std::string> {
                             return r.mean.size() == 4 ? std::nullopt
                                                       : std::optional<std::string>("not a quaternion");
                         });
```

`latest` returns the newest row for that group, prior hash AND model version
that passes both the structural checks and the caller's own. `history` returns
every row of a group, oldest first, whatever its hash.

The table is `calibration(id, written_at_ns, session, grp, prior_hash,
model_version, mean, cov, summary, reason, evidence_s, drive_s)`, with `mean`
and `cov` as JSON arrays (`cov` row-major) so the shell shows them, and a view
`calibration_latest` of the newest row per group, hash and version:

```bash
sqlite3 calibration.sqlite 'select id, grp, reason, summary from calibration'
```

## Behaviour worth knowing

**Newest means highest id, not latest timestamp.** `written_at_ns` is the
wall clock, for a person reading the history; a clock that steps back must not
change which row is current.

**A bad row costs one row.** A row whose numbers do not parse, are not finite,
or whose covariance is not square, symmetric and positive definite (checked by
Cholesky) is skipped, `latest` returns the next older good one, and the reason
goes into its `skipped` list. `append` refuses to write such a row in the
first place.

**A file that is not ours is left as it was.** Nothing is written until the
file is known to be a database of a schema this version reads. A text file at
the path is refused with `NotADatabase`, byte for byte unchanged; a database
whose `meta.schema_version` is newer is refused with `NewerSchema` and not
upgraded or touched.

**Power loss: WAL with `synchronous=FULL`.** A commit is appended to the
`-wal` log and copied into the database only at a checkpoint, so a power cut
mid-write, or mid-checkpoint, leaves the last complete commit and never a torn
one: SQLite replays the complete transactions from the log at the next open.
`FULL` fsyncs the log at every commit, so a row `append` reported written
survives the power cut too. (`NORMAL` would skip that fsync and could lose the
last few commits; writes here are minutes apart, so the fsync costs nothing
that matters.) `append` of several rows is one transaction: the groups written
at one moment are kept together or not at all.

All of that rests on the storage honouring a flush. Keep `/data` on ext4 with
barriers (the default; never `nobarrier` or `data=writeback`); a device that
acknowledges a flush and keeps the data in a volatile cache can corrupt any
database, and only hardware fixes that.

**A damaged database is moved aside, not refused.** At open, a database of
ours that fails `PRAGMA quick_check` is renamed to
`<path>.corrupt-<unix time>`, with its `-wal` and `-shm`, and a fresh one is
started; `recovered()` says where, and the node reports it in status and
health. Nothing is written to the damaged file first -- checkpoint-on-close is
off until the checks pass, so its log is kept unapplied beside it. A file
that is not SQLite at all is still refused untouched, since it may be somebody
else's.

**Copying it off the car.** Recent rows may still be in the `-wal`, so copy
all three files together, or use `sqlite3 calibration.sqlite ".backup out.sqlite"`.

## Tests

| Target | Label | What it proves |
| --- | --- | --- |
| `calibration_store_test_store` | unit | Round trips exactly; the newest row for a group, hash and version is found even when its timestamp is older; other hashes, versions and groups are kept apart; nothing is replaced; all of it survives a reopen; a row the caller refuses is passed over for the one before; `synchronous` is FULL; a batch with one bad row writes none. |
| `calibration_store_test_malformed` | unit | Nine kinds of bad row written behind the store's back (torn JSON, an object, a null, empty, not square, asymmetric, not positive definite, an overflow, the wrong shape for the caller) are all skipped for the good one; a text file and a newer schema are refused and left byte-identical; an uncreatable directory is a named error; bad rows are never written; a database with a damaged table page and a committed transaction still in its WAL is moved aside byte-identical with its log unapplied, and a fresh store opens; a batch that fails in SQLite on its second row leaves no trace of its first. |

Mutation-checked on 2026-09-23: dropping the structural check on read,
ordering by timestamp, dropping the schema-version check, `synchronous=NORMAL`,
checkpointing on close while checking, skipping `quick_check`, and a batch
outside a transaction each fail a test. No test simulates the power cut
itself (that needs a VFS that drops unsynced writes); what is tested is the
configuration and the recovery.
