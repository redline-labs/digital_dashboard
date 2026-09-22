---
title: map_match
parent: Nodes
---

# map_match

## Overview

`map_match` says which road the vehicle is on and what is ahead of it. It
subscribes the GNSS record topics from [bd992_bridge](bd992_bridge.html),
matches each fix onto the road graph, and publishes an electronic horizon: the
segment the vehicle is on plus its unambiguous continuation, as far as the road
goes without a choice. Where the road forks, the path ends; guessing which way
a driver will go is what branch probabilities are for, and a horizon that
guesses wrong is worse than one that stops. Branches are the next thing the
horizon grows, as extra `HorizonPath` entries rather than a change to anything
already published. It runs on the vehicle, entirely offline.

It opens the graph directly rather than asking [map_server](map_server.html).
A matcher makes dozens of lookups per fix at 10 Hz, and round-tripping those
over zenoh would buy latency and a hard cross-process dependency for what is a
pointer dereference into an mmap. A read-only mmap is shareable, so both
processes hold the same file with no coordination. The graph itself comes from
`map_build graph` ([Building the map](../tools/map_build.html)). On the
dashboard, the `road_info` widget reads the horizon for the road name and the
map widget's `highlight_zenoh_key` lights the matched road up.

The matcher is online: it sees one fix at a time and must answer immediately,
carrying a short beam of candidates forward, each scored by how well it
explains the new fix and how plausibly it follows from the last. The one thing
it has that an off-the-shelf matcher does not is that the emission width comes
from the receiver: a BD992 with an RTK fix reports centimetres and, coasting on
a lost correction link, metres. A matcher tuned for one silently jumps roads on
the other, which is why the horizon publishes the sigma it used.

{: .important }
The horizon does not correct the position. It says which segment the vehicle
is on; the map keeps drawing the receiver's own fix. An RTK fix is accurate to
centimetres while OSM road geometry is routinely several metres from the real
centreline, so snapping the displayed position to the map would make it worse.

## Running it

```bash
# open the graph, report what is in it, probe a query, exit -- no bus
./build/nodes/map_match/map_match --config configs/map_match.yaml --check

# match
./build/nodes/map_match/map_match --config configs/map_match.yaml
```

`--check` prints the segment, junction and edge counts, the coverage box and
the build time, then proves a query comes back by asking for the nearest road
to the centre of the coverage. A graph that opens and answers nothing is
otherwise a clean bill of health.

Unlike `map_server`, a graph that will not open is fatal: a matcher with no
graph has nothing to say, and a node publishing an empty horizon forever is
worse than one that does not start.

Without a receiver, `nodes/bd992_mock` drives a real route from `map_server`'s
road graph and publishes GNSS on the real `nodes/bd992` prefix, so `map_match`
works against it with no configuration change. Launch `map_server` first, then
the mock, then this node:

```bash
./build/nodes/map_server/map_server --config configs/map_server.yaml
./build/nodes/bd992_mock/bd992_mock --route 33.6866,-117.8558 --to 33.7701,-118.1937
./build/nodes/map_match/map_match --config configs/map_match.yaml
```

`configs/dashboard/map_demo.yaml` then shows the result: a `road_info` widget
naming the road and the map highlighting it. Do not run the mock alongside a
real `bd992_bridge`; two publishers on one key interleave and nothing is logged.

## Configuration

`configs/map_match.yaml` documents every key. The ones that matter:

| Key | Default | What it does |
|---|---|---|
| `graph` | none | Path to the `.graph` file, taken literally. The checked-in value points at a developer's home directory; change it |
| `position.position_key` | `nodes/bd992/gsof/lat_long_height` | The trigger: a fix is matched when a position arrives |
| `position.velocity_key` | `nodes/bd992/gsof/velocity` | Heading and speed, held as latest-known |
| `position.sigma_key` | `nodes/bd992/gsof/position_sigma` | Horizontal RMS, held as latest-known |
| `position.pair_within_ms` | `200` | How fresh a velocity or sigma must be to be paired with a position |
| `position.stale_after_ms` | `3000` | A gap longer than this drops the candidate beam |
| `match.search_radius_m` | `50.0` | How far to look for candidate segments |
| `match.beam_width` | `6` | Candidates carried forward; each costs a bounded search per fix |
| `match.min_sigma_m` / `max_sigma_m` | `4.0` / `30.0` | The receiver's sigma is clamped into this range |
| `match.default_sigma_m` | `8.0` | Used when the receiver reports nothing |
| `match.transition_beta_m` | `15.0` | How much a detour costs: a `2*beta` detour is `e^-2` as likely |
| `match.heading_valid_above_mps` | `1.5` | Below this speed the course over ground is ignored |
| `match.penalty_beta_m` | `15.0` | How much the graph's ranking penalty (road class, heading disagreement, wrong way down a one-way) costs: `penalty / beta` is the exponent, like the transition term |
| `match.lookahead_m` | `2000` | How far ahead the path is built |
| `services.horizon_key` | `nodes/map_match/horizon` | Where the horizon goes, every `horizon_interval_ms` (100) |
| `services.status_key` | `nodes/map_match/status` | Node status, every `status_interval_ms` (5000) |

All three position topics must come from the same publisher; two bridges on
one prefix would interleave. Pairing is by arrival age, not by GSOF
transmission, so records the receiver sends together pair whether the shared
rate is 1 Hz or 50 Hz, records on a slower schedule pair while fresh and are
reported absent when not, and changing what the receiver emits needs no change
here. Set `pair_within_ms` by how long a heading stays true: a vehicle turning
at 10 deg/s moves 2 degrees in 200 ms, inside the matcher's tolerance, and a
full second would not be. A stale value is never used; the matcher falls back
to distance alone without a heading and to `default_sigma_m` without an
accuracy.

The sigma floor is a statement about the map's accuracy rather than the
receiver's: OSM centrelines are metres from the real road, so believing a 2 cm
RTK fix completely would match nothing. The ceiling stops a receiver that has
lost its corrections from making every road equally likely. Below
`heading_valid_above_mps` a stationary vehicle's heading wanders through all
360 degrees and would re-match the car onto a different road at every light.

## Topics

It subscribes `GsofLatLongHeight`, `GsofVelocity` and `GsofPositionSigma` on
the three keys above. Record 38 is deliberately not subscribed; the fix quality
the matcher needs comes from record 12's RMS.

It publishes `MapHorizon` on `nodes/map_match/horizon`. Every message is a
complete snapshot rather than an ADASIS-style delta, so a dropped sample costs
nothing. `hasPosition` is false when there is no fix or no match, and the
message is still sent, so "running and lost" can be told from "dead".
`position` carries the segment, offset, heading, confidence (0..100, how sure
this is the right road, not how accurate the fix is), the sigma actually used,
and the fix as received. `paths` holds the root path first; `profiles` are runs
of one attribute along a path, with `roadName`, `roadRef`, `roadClass`,
`speed`, `laneCount` and `segment` today. Consumers must filter the profile
kinds they understand rather than switch over them, so new kinds break nothing.
`horizonEpoch` bumps whenever the tree is re-anchored, and path ids and offsets
mean nothing across an epoch change.

It publishes `MapMatchStatus` on `nodes/map_match/status`: whether the graph
opened, the three keys in use, `fixesReceived`, `fixesMatched`,
`fixesUnmatched`, `horizonsPublished`, `lastFixAgeMs`, the last confidence and
sigma, and `fixesWithoutVelocity` / `fixesWithoutSigma`.

### Health

`nodes/map_match/health` ([NodeHealth](../libs/node_health.html)), once a second and
at once on any change:

| Check | Not ok when |
|---|---|
| `graph` | never after start: a graph that does not open exits instead |

`inspect health` prints them.

## Troubleshooting

A blank road name and a dead node look the same in a screenshot; the status
topic separates them. `lastFixAgeMs` growing without bound means this node is
fine and its input has stopped. `fixesUnmatched` is expected to be non-zero:
car parks and private roads are not in the graph. `fixesWithoutVelocity` or
`fixesWithoutSigma` climbing means the receiver's output configuration changed,
with velocity disabled or running far below the position rate, and the matcher
is falling back rather than silently degrading.

A match that looks wrong is first a question about `sigmaM` in the horizon,
visible in `scope` with no special tool. A confidence that is low beside a
frontage road is the normal case, not a fault.

The map widget's highlight only exists at z13 and deeper, because way ids only
survive in tiles from there; on a zoomed-out map it quietly disappears while
the horizon is still being published.

## Tests

`map_match_test_config` covers the config surface, including a schema name the
build does not know and two topics sharing a key. `map_match_test_fix_assembler`
pins the arrival-age pairing, in particular the cases a change to the
receiver's rates creates. `map_match_test_matcher` drives the matcher and the
horizon over a synthetic drive on a graph it builds: a heading deciding between
parallel roads, a stationary vehicle not re-matched on noise, and an RTK sigma
not treated like a consumer one.
