---
title: node_health
parent: Libraries
---

# node_health

## Overview

How a node says it is working, in one shape every node shares, and how a monitor
reads all of them at once. A node declares a `HealthReporter`, names the things
it checks, and the reporter publishes a heartbeat on `nodes/<node>/health`. A
`HealthMonitor` subscribes to all of them, joins them against the node
directory, and reports one verdict per node. `inspect health` is the
command-line reader.

Detailed device status stays where it was: `CanBridgeStatus`, `Bd992Status`,
`Mti610Status` and the rest say what a particular device is doing, and a tool
that knows the device reads them. Health is the summary a monitor can read
without knowing any of them, so "is anything wrong" is answerable in one place.

The library is separate from [pub_sub](pub_sub.html) because pub_sub is
transport; this adds a thread, a policy about what counts as healthy, and the
systemd notifications. Its pure parts -- the state folding, the codec and the
classification -- need no bus and carry the logic worth testing.

## Public headers

| Header | Declares |
|---|---|
| `node_health/state.h` | `State`, `severity()`, `worst()`, `Check`, `CheckReport`, `HealthSnapshot`. |
| `node_health/reporter.h` | `HealthReporter`, `ReporterOptions`, `ActivityCheck`. |
| `node_health/monitor.h` | `HealthMonitor`, `HealthRow`. |
| `node_health/classify.h` | `Verdict`, `Observation`, `ClassifyOptions`, `classify()`, `activityState()`, `Continuity`, `account()`. |
| `node_health/codec.h` | `encode()`, `decode()`, `decodePayload()` for the `NodeHealth` schema. |

## Using it from a node

Declare the reporter before anything whose callbacks touch its checks, so those
are destroyed first:

```cpp
node_health::HealthReporter health("megasquirt");
auto& can_rx = health.addActivityCheck("can_rx", std::chrono::seconds(1));

pub_sub::ZenohTypedSubscriber<CanFrame> subscriber(key, [&](CanFrame::Reader m) {
    can_rx.touch();
    ...
});

health.markReady();
while (!cli::interrupted())
{
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    health.kick();
}
```

`setCheck(name, state, detail)` is the general form, for a node that already has
a status tick: call it from there and the reporter publishes the change. An
activity check covers the common case, where something has to keep happening;
`touch()` is lock-free and safe from a zenoh thread.

The overall state is `starting` until `markReady()`, `stopping` once the
reporter is being destroyed, and otherwise the worst check. A check nobody could
evaluate counts as `degraded`, never as ok.

## What goes on the wire

`nodes/<node>/health`, schema `NodeHealth`: the node's name and session, the
overall state, a sequence number, uptime, the heartbeat period, the pid, and one
entry per check. A heartbeat goes out every period (one second by default), and
a state change is published at once, coalesced to at most one sample per 100 ms.
The reporter's thread sleeps until the next of those, the next systemd watchdog
feed, or, when the node has activity checks, half the shortest check's window;
a node with no activity checks wakes only for its heartbeat.
A reporter being destroyed publishes one last sample saying `stopping`.

{: .note }
The key is an ordinary topic, not one of the `@redline/` liveliness spaces, so
`bag record` captures it, scope plots it and `inspect echo` prints it. A health
history inside a recording is exactly what you want after a failure.

## Reading it

`HealthMonitor` keys rows by session id, so two instances of the same node are
two rows. Each row's verdict comes from `classify()`:

| Verdict | Means |
|---|---|
| `ok`, `starting`, `degraded`, `fault`, `stopping` | what the node itself reported |
| `late` | alive by its identity, but no sample for three of its periods: hung |
| `silent` | alive by its identity and has never published health at all |
| `exited` | its identity went away after it said `stopping`: a clean exit |
| `gone` | its identity went away without one: it died |

{: .warning }
`revision()` moves when a sample arrives or the directory changes, never as time
passes. `late` is purely a function of time, so a consumer that redraws only on
a changed revision will never show a node going late. Poll `snapshot()` on a
timer.

The split between `gone` and `late` is the reason health is a heartbeat rather
than a liveliness token. A token stays up for a process whose main loop is
stuck, which is the failure a dashboard most needs to see.

## systemd

With `ReporterOptions::systemd` (the default), `markReady()` sends `READY=1`,
every state change sends a `STATUS=` line naming the first failing check, and
the reporter sends `WATCHDOG=1` at half the interval systemd asked for. Once a
node has called `kick()`, the watchdog is only fed while kicks keep arriving --
so a hung main loop stops the pings and the unit gets restarted. Outside a unit
every one of these is a silent no-op.

{: .note }
The unit files live in the Yocto layer, not here. Until they set `Type=notify`
and `WatchdogSec=`, the watchdog side does nothing.

## Tests

```bash
ctest --test-dir build -L node_health
```

| Target | Labels | Proves |
|---|---|---|
| `node_health_test_state` | `node_health unit` | Severity order and how checks fold, including that an unknown check never reads as ok. |
| `node_health_test_codec` | `node_health unit` | The round trip; an enumerant from a newer node reads as unknown; an oversized check list and an overlong detail are bounded, and a multi-byte character is never cut in half. |
| `node_health_test_classify` | `node_health unit` | Every verdict rule at its boundary: exactly three periods is not late and one tick later is, a period of zero counts as a second, `stopping` then gone is `exited`, the silent grace, the activity states and the restart and gap counters. |
| `node_health_test_end_to_end` | `node_health net` | A reporter and a monitor on a real session: a fault arrives long before the five-second heartbeat would, and a clean shutdown reads as `exited`. |
