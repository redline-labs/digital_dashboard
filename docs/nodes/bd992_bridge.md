---
title: bd992_bridge
parent: Nodes
redirect_from: /bd992.html
---

# bd992_bridge

## Overview

A Trimble BD992 GNSS receiver on the vehicle's Ethernet, publishing GSOF
records as Cap'n Proto messages and exposing the receiver's own configuration
as zenoh services. The node is the top of three layers:
[gsof](../libs/gsof.html) is the protocol, [bd992](../libs/bd992.html) is the
TCP transport and the read-before-write configuration logic, and
`nodes/bd992_bridge` maps records onto schemas and reads the YAML. The
rationale, the live-hardware findings and the open questions are in the
[design notes](../design/bd992.html).

The receiver is a TCP server and the node connects to it. Nothing leaves the
receiver until something attaches, which keeps the vehicle network quiet and
makes the node the only thing that has to be running.

## Running it

In the receiver's web interface, under I/O Configuration and Port Summary, add
an IP socket in TCP server mode and note its port number and which socket it
is. The first three IP sockets are port indices `20`, `21` and `22` on the
wire; Trimble's own prose numbers them from one. Then point
`configs/bd992/bd992.yaml` at the receiver and probe it before running it:

```bash
./build/nodes/bd992_bridge/bd992_bridge --config configs/bd992/bd992.yaml --probe
./build/nodes/bd992_bridge/bd992_bridge --config configs/bd992/bd992.yaml --check
./build/nodes/bd992_bridge/bd992_bridge --config configs/bd992/bd992.yaml
```

| Option | |
| --- | --- |
| `--config <file>` | The YAML below. |
| `--probe` | Print what each stored application file contains and exit. The first thing to run, because it answers the two questions the ICD does not. |
| `--check` | Diff the receiver against the config and exit non-zero on drift. |
| `--replay <file>` | Replay a captured GSOF byte stream instead of connecting. |
| `--loop` | With `--replay`, start again at the end of the capture. |
| `--replay-delay-ms <n>` | With `--replay`, milliseconds between chunks. `0` replays as fast as the bus will take it. |
| `--dump-gsof <file>` | Write the raw received bytes to a file. |
| `--debug` | Verbose logging. |

The config has three sections. `receiver` names the host and two ports:
`stream_port` is the socket configured to output GSOF, `control_port` the one
that accepts command and report packets. Whether one socket can do both is
still unknown (see the design notes); if you want to try, configure it for
input and output and point both keys at it. `reconnect_backoff_ms` is tried in
order and then the last value repeats.

`configuration` decides what the node does to the receiver. `mode: enforce`
reads the receiver's configuration, compares it against `outputs`, and writes
only what differs; `report_only` stops after the comparison, and is the mode to
use around a receiver somebody else owns. `port_index` is the zero-based wire
index of the socket above. `port_policy: additive` (the default) leaves outputs
the config does not mention alone and reports them; `exclusive` turns them off.
Outputs on other ports are ignored in both directions. `appfile_index` is
which stored application file holds the running configuration, which `--probe`
is how you find out. `recheck_interval_s` re-reads and re-compares on a timer
(`0` checks once, at startup), and `allow_raw_commands` gates the
`send_command` service.

Each entry in `outputs` is a record name from the GSOF record table and a
named rate: `off`, `100hz`, `50hz`, `20hz`, `10hz`, `5hz`, `2hz`, `1hz`,
`2s`, `5s`, `10s`, `15s`, `30s`, `60s`, `5min`, `10min` or `once`. An unknown
record name is refused at load with the full list. The list is what this node
insists on, not everything available; with `additive` the receiver may send
more, and it will be decoded and published either way.

`publish` sets `topic_prefix` (default `nodes/bd992`), `status_key`,
`status_interval_ms` and `publish_unknown_records`, which puts records this
build does not model on `<prefix>/gsof/raw` as bytes.

## Without a receiver

The whole decode and publish path runs over a captured byte stream:

```bash
./build/nodes/bd992_bridge/bd992_bridge \
    --config configs/bd992/replay.yaml \
    --replay mock_data/data/bd992_gsof_capture.bin --loop --replay-delay-ms 20
```

The bytes go through the same framer, page assembler, record parsers and
publishers a live receiver drives, so the topics are the real thing.
`--dump-gsof <file>` writes the raw stream, so a minute from a vehicle becomes
a fixture. Services are not offered in replay mode: there is no receiver to
answer with, and a service that could only ever fail is worse than none.

### bd992_mock

The capture above is 837 bytes from a receiver that was sitting still. For
anything downstream of position, `nodes/bd992_mock` asks `nodes/map_server`
where to drive and then drives it:

```bash
./build/nodes/map_server/map_server --config configs/map_server.yaml   # first

./build/nodes/bd992_mock/bd992_mock --route 33.6866,-117.8558 --to 33.7701,-118.1937
./build/nodes/bd992_mock/bd992_mock --track "Willow Springs"
```

| Option | |
| --- | --- |
| `--route <lat,lon> --to <lat,lon>` | Drive a road route from `map/route`, with posted speed limits from `map/nearest`. |
| `--track <id or name>` | Drive a race track's centreline from `map/track_catalog` and `map/track_detail`. A circuit laps; a point-to-point course runs once. |
| `--profile <name>` | Routing cost profile. Empty means the graph's default. |
| `--loop` | Start again on reaching the end. |
| `--no-speed-limits` | Skip the `map/nearest` pass and drive at the cruise speed. |
| `--check` | Resolve the path, report it and exit without publishing. The fastest way to find out whether map_server can answer at all. |
| `--config <file>` | Optional. `configs/bd992/mock.yaml` documents every default. |

The vehicle is a point mass on the line: position, speed and heading all come
from one distance-along-path and one speed, so they cannot disagree. Speed is
capped by the local curvature (`v = sqrt(a_lat * R)`) and by the posted limit,
and braking starts before a corner rather than at it. Set
`vehicle.lateral_accel_mps2` to `8` to `12` for a circuit; the `3.0` default is
a road car and makes for a slow lap. There is no elevation in either source, so
`ellipsoidHeightM` is a constant and vertical velocity is zero, and the fix is
always RTK-fixed with a fixed correction age.

The mock publishes five of the topics below (`position_time`,
`lat_long_height`, `velocity`, `position_type`, `position_sigma`) on the real
`nodes/bd992` prefix, which is what makes `nodes/map_match` and the dashboard
map widget work with no configuration change. It announces itself as
`bd992_mock`, so `inspect nodes` always says which one you are looking at.

{: .warning }
Do not run `bd992_mock` alongside a real `bd992_bridge`. Two publishers on one
key interleave, consumers take whichever sample arrives first, and nothing is
logged. The mock warns at startup if it sees another publisher, but it cannot
see one that happens to be quiet just then.

## Topics

One topic per GSOF record type, under `<topic_prefix>/gsof/`. A record named
`some_record` in the table is published on `<prefix>/gsof/some_record` with
schema `GsofSomeRecord`; unmodelled records go on `<prefix>/gsof/raw` as
`GsofRawRecord`, and `status_key` carries a `Bd992Status`.

```
position_time  lat_long_height  ecef_position  ecef_delta  tangent_plane_delta
velocity  dop_info  clock_info  position_vcv  position_sigma  sv_brief
sv_detailed  receiver_serial  current_time_utc  attitude_info
receiver_diagnostics  all_sv_brief  all_sv_detailed  received_base
battery_memory  position_type  lband_status  base_position  all_sv_detailed_page
ins_full_nav  ins_rms  code_position  lat_long_msl_height  second_antenna_sigma
nav_message_auth  ionoguard_info  ionoguard_summary
```

A consumer wanting position and fix quality subscribes to two topics. Nothing
in the node decides which fields belong together, and nothing in it depends on
which messages the receiver has enabled or at what rate: records are published
as they are parsed, publishers are created on first sight of their record, and
`status.seen[]` reports which record types have arrived and how long ago.

Everything published is in degrees. The wire is radians in records 2, 27 and
41 and degrees in record 49; the conversion happens once, in the node. Heights
are above the WGS-84 ellipsoid everywhere except `lat_long_msl_height`, which
is the one record that carries an orthometric height and the geoid model
behind it. The difference is tens of metres (about 34.5 m in southern
California, with MSL the larger), so feed `lat_long_msl_height.mslHeightM` to
an altimeter and `lat_long_height` to a map. GPS time is published as week plus
milliseconds, not converted to Unix time; consumers wanting wall-clock time
have the zenoh sample stamp.

| Field | |
| --- | --- |
| `position_type.positionFixType` | The whole ICD list, including the RTX and INS forms. `positionFixTypeRaw` always carries the wire byte, so a firmware newer than this build is reported rather than lost. |
| `position_type.rtkFixed` | Clear is RTK float, set is RTK fixed. Not a general "is the fix good" flag: an RTX fix holds decimetre accuracy with this bit clear. |
| `position_type.correctionAgeS` | Climbing means the correction link has gone and the fix is coasting. |
| `attitude_info.yawDeg` | Where the vehicle points. `velocity.headingDeg` is a course over ground and is meaningless when stopped. |
| `status.seen[].ageMs` | Distinguishes "the receiver is fine" from "the receiver quietly stopped sending record 27". |
| `nav_message_auth.anyFailed` | A satellite's navigation message failed authentication, which is a spoofer. Nothing else in the stream shows it. |

## Services

Siblings of the topics under `<topic_prefix>`:

| Key | |
| --- | --- |
| `nodes/bd992/get_output_config` | What the receiver is configured to emit. GETAPPFILE (65h). |
| `nodes/bd992/set_output_config` | Change it. Reads first, writes only the difference. `dryRun` reports the plan without sending anything. |
| `nodes/bd992/apply_config` | Re-run the pass from the node's own YAML, now. |
| `nodes/bd992/get_receiver_info` | Serial number and installed options. GETOPT (4Ah). |
| `nodes/bd992/send_command` | Any packet type, any payload. |

```bash
inspect call nodes/bd992/get_output_config --data '{}'
inspect call nodes/bd992/set_output_config --data '{"dryRun":true,"outputs":[...]}'
```

`send_command` is refused unless `allow_raw_commands` is set, because an
arbitrary command can leave a receiver unreachable. `set_output_config` will
not write while the node is in `report_only` mode.

## Troubleshooting

**`--probe` times out on every application file index** while the stream port
is delivering reports. The commands are not reaching a listener, whatever the
port number says. The socket is most likely configured output-only; configure
one for input and output and point `control_port` at it.

**A topic is missing.** Publishers are created on first sight of their record,
so a topic that has never published looks identical, in every picker in this
tree, to one whose receiver went quiet. Read `status.seen[]` to see what is
arriving, and check the receiver's own output list with
`get_output_config`.

**`status.outputsCorrected` keeps climbing.** Something else keeps changing the
receiver back. Correcting a drift is meant to be rare; if it is not, find the
other writer or move to `port_policy: exclusive` on a port this node owns.

**The height is tens of metres off.** You are comparing an ellipsoid height
with a sea-level one. Only `lat_long_msl_height` is orthometric.

**An unknown fix type.** `positionFixTypeRaw` has the wire byte. The enum was
once trimmed to what a vehicle receiver "can" produce, and a live receiver
reported `rtxFastLowLatency` (33) within the hour.

**Two nodes on one prefix.** If the map widget stutters between two positions,
a `bd992_mock` is running alongside the bridge. `inspect nodes` tells them
apart.

**The raw topic is busy.** The receiver is emitting a record type this build
does not model. The bytes are on `<prefix>/gsof/raw`; adding the record is one
row in the record table plus a struct in `libs/gsof`.
