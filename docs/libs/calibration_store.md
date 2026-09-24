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
| `calibration_store/store.h` | `Store` (`open`, `append`, `latest`, `history`), `Row`, `Error`, `Result<T>`, `structuralProblem()`. |

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

**WAL, synchronous NORMAL.** A crash mid-write loses that write, not the file;
a power cut may lose the last write, which the next session's writes replace.
The store makes `-wal` and `-shm` files beside the database.

## Tests

| Target | Label | What it proves |
| --- | --- | --- |
| `calibration_store_test_store` | unit | Round trips exactly; the newest row for a group, hash and version is found even when its timestamp is older; other hashes, versions and groups are kept apart; nothing is replaced; all of it survives a reopen; a row the caller refuses is passed over for the one before. |
| `calibration_store_test_malformed` | unit | Nine kinds of bad row written behind the store's back (torn JSON, an object, a null, empty, not square, asymmetric, not positive definite, an overflow, the wrong shape for the caller) are all skipped for the good one; a text file and a newer schema are refused and left byte-identical; an uncreatable directory is a named error; bad rows are never written. |

Mutation-checked on 2026-09-23: dropping the structural check on read,
ordering by timestamp, and dropping the schema-version check each fail a test.
