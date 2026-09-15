---
title: tracks
parent: Tools
redirect_from: /tracks.html
---

# tracks

## Overview

The tracks tileset is 994 race tracks worldwide, circuits and point-to-point
courses, in a 42 MB `.mbtiles` that holds both the tiles the map draws and a
catalogue of each track's outline, centreline and start/finish gate. It is
built by `map_build tracks`, the one verb of [map_build](map_build.html) that
never opens a PBF, and it is rebuilt independently of the basemap: a new drop
of track maps does not rebuild the 383 MB SoCal basemap, and a new OSM extract
does not touch the tracks. It is global where the basemap is regional, which is
affordable because 994 circuits are 4 MB of tiles.

842 tracks have a centreline; the rest render as an outline with a verdict
saying why there is no lap distance, and 23 whose source files are corrupt are
catalogued but not drawn. What the source data turned out to be and how the
centreline and QA gate came out of it are on the design page
[Race tracks](../design/tracks.html).

## Building it

```bash
./build/tools/map_build/map_build tracks \
    --input ${REDLINE_DATA_DIR}/maps/tracks \
    --output ${REDLINE_DATA_DIR}/maps/tracks.mbtiles \
    --report /tmp/tracks_qa.csv
```

That takes about a second. The input is a directory of GeoJSON files, one per
layout, each a `FeatureCollection` of a `Polygon` (the outline, with `name`,
`closed` and `degenerate`) and a `Point` named `Start / Finish` carrying
`circuit`, `length_m` (the published lap length), `gatewidth_m` and `combo`.

| option | default | |
|---|---|---|
| `--name` | `tracks` | tileset name written into the metadata |
| `--min-zoom`, `--max-zoom` | `8`, `14` | zoom range built |
| `--samples` | `2000` | centreline samples per track |
| `--venue-expand-m` | `300` | how far apart two layouts may be and still share a venue |
| `--length-tolerance` | `0.05` | fractional agreement required against the published lap length |
| `--width-min`, `--width-max` | `3`, `30` | median track width accepted, in metres |
| `--report` | — | write a per-track CSV of what happened |

The output is one SQLite file: the tiles in the ordinary mbtiles tables, the
catalogue in `track_catalog`, `track_geometry` and `track_meta` alongside them
(`libs/track_store`). A `build_id` is written into both the mbtiles `metadata`
table and `track_meta`, and the file is refused at open if the two differ.

The verb prints tracks read, skipped and grouped into venues, then a count per
centreline verdict and per gate result. Over all 994 today (962 of the 992
files with a point get a gate):

| verdict | count | |
|---|---|---|
| `ok` | 842 | 827 annulus + 15 ribbon; median length error 0.64%, p95 2.7% |
| `width-out-of-range` | 108 | figure-8 crossovers (Suzuka), odd geometry |
| `length-mismatch` | 20 | the centreline disagrees with the outline it came from |
| `multiple-loops` | 23 | ovals + infields, `*_Combo` layouts |
| `degenerate` | 1 | source-flagged |

`--report` writes one CSV row per track, and one per skipped file with its load
status in the `quality` column, so nothing disappears between the log and the
report:

```
id,name,circuit,venue_id,quality,topology,gate,combo,closed,outline_points,
outline_length_m,centerline_length_m,published_length_m,length_error_pct,
published_disagrees,median_width_m,seam_index,seam_gap_m,seam_returns,
west,south,east,north
```

`published_disagrees` is a warning, not a veto (36 accepted tracks carry it),
`seam_returns` is what catches a file holding several loops, and the report as
a whole is the metric to measure any change to the derivation against.

## Serving it

```yaml
tilesets:
  - name: socal
    path: ${REDLINE_DATA_DIR}/maps/socal.mbtiles
  - name: tracks
    path: ${REDLINE_DATA_DIR}/maps/tracks.mbtiles
tracksets:
  - name: tracks
    path: ${REDLINE_DATA_DIR}/maps/tracks.mbtiles
```

The same path appears twice on purpose: `mbtiles::Archive` reads the tiles,
`track_store::Store` reads the catalogue. Different libraries, different
questions, and either half can be present without the other; an ordinary
basemap has no catalogue at all. Do not tidy it into one entry.

`map_server --config configs/map_server.yaml --check` reports the trackset with
its path, its build id and counts by quality.

Beyond the tiles, two query services on `schemas/map_tracks.capnp`:
`map/track_catalog` answers what tracks exist, where, and their quality,
filterable by venue or by a point and radius; `map/track_detail` returns one
track's full-resolution outline, centreline, per-point distance and half width,
and its gate. `nodes/bd992_mock` is the first caller of either: `bd992_mock
--track "Willow Springs"` fetches the centreline and drives a car round it,
publishing the GNSS topics as it goes; see
[bd992_bridge](../nodes/bd992_bridge.html).

## Checking it looks right

The dashboard asks for the tracks as an overlay alongside the basemap; the
widget keeps one tile source per archive, so a missing archive and no coverage
stay distinguishable:

```yaml
config:
  tileset: "socal"
  overlay_tilesets: ["tracks"]
```

Three layers: `track` (the surface, a fill with the infield as a hole for a
circuit and solid for a point-to-point course, z11+), `track_centerline` (z12+),
and `track_label` (z8+, gathered alongside the basemap's `place` labels and
placed by the same collision pass).

{: .note }
`MapConfig_t` already uses `track_*` for the vehicle's own breadcrumb trail
(`show_track`, `track_points`, `track_width`). Style fields for circuits are
`racetrack_*`.

```
app_launch(app="map_server", config="configs/map_server.yaml")
app_launch(app="dashboard", config="configs/dashboard/map_demo.yaml")
```

then point the map at Willow Springs (34.87255, -118.2645) at z14. Buttonwillow,
Willow Springs and Streets of Willow are all inside the SoCal basemap, so
basemap and circuit coexist there.

Check the infield is not filled. That is the winding trap described on the
design page, a visual failure with no error behind it. Setting
`racetrack_surface` to something bright makes it unmistakable: the surface
should be a ribbon with the basemap's roads visible straight through the middle.
