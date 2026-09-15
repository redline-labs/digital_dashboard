---
title: map_server
parent: Nodes
redirect_from: /map.html
---

# map_server

## Overview

`map_server` puts offline maps on the bus. It opens `.mbtiles` archives, road
graphs and race-track catalogues built by `tools/map_build`
([Building the map](../tools/map_build.html)) and answers zenoh queries for
tiles, tileset catalogs, nearest-road snaps, routes and track geometry. No HTTP
anywhere: the dashboard's map widget fetches every tile from this node over the
bus, so a vehicle with no internet renders the same map it would with one.
`nodes/bd992_mock` asks it where to drive, and `nodes/map_match` shares its
graph file without asking it anything.

This page covers running and configuring the node and the map widget that
draws from it. [map_match](map_match.html) is the node that puts the vehicle
on a road. The renderer behind the widget is [map_render](../libs/map_render.html),
with the tile decoder in [mvt](../libs/mvt.html) and the archive reader in
[mbtiles](../libs/mbtiles.html); why the renderer is ours and what a frame
costs is in [Map renderer](../design/map-renderer.html). `scope`'s map panel,
which reads archives directly and never talks to this node, is in
[scope](../apps/scope.html).

## Running it

```bash
# open every archive, graph and trackset, say what is in it, exit -- no bus
./build/nodes/map_server/map_server --config configs/map_server.yaml --check

# serve
./build/nodes/map_server/map_server --config configs/map_server.yaml
```

`--check` is the fastest way to find out whether a path is right. It probes a
tile out of each archive, reports each graph's build time, size and whether its
routing overlay is present, and counts each track catalogue by quality.

Or drive it through the agent interface, which supervises `map_server` as a
node: launched, read with `app_logs`, quit, but with no control socket, so
nothing that inspects or clicks applies to it.

```python
app_launch(app="map_server", config="configs/map_server.yaml")
app_launch(app="dashboard", config="configs/dashboard/map_demo.yaml")
```

Launch the node first. `app_launch` returns only once the server is serving,
waiting for the log line that means every service is registered, so the pair
is not a race against an archive still being opened. Without a server behind it
the widget draws its background and reports `No reply from map_server`, which
is a screenshot that says nothing about the map. `configs/dashboard/map_demo.yaml`
is a working layout: a street map following the vehicle with the tracks overlay
and the matched road highlighted, an overview at z9, and a `road_info` readout.

From another terminal:

```bash
./build/nodes/inspect/inspect services
./build/nodes/inspect/inspect call map/catalog --data '{}' --json

# Tiles are a BATCH -- `tiles` is a list, not z/x/y at the top level. The reply
# also carries the archive's minzoom/maxzoom, which is what the widget clamps
# its requests to.
./build/nodes/inspect/inspect call map/tile --json \
    --data '{"tileset":"socal","tiles":[{"z":14,"x":2828,"y":6562}]}'
```

That is the Irvine tile: 81958 bytes, gzip, 14 layers. The two the archive
carries that Irvine has none of are `mountain_peak` and `aerodrome_label`.

### Exercising the routing half

The one program that calls these for a living is `nodes/bd992_mock`: it asks
`map/route` for a road route and `map/nearest` for each segment's speed limit,
then drives the result and publishes it as GNSS. `bd992_mock --route ... --check`
exercises the whole routing half in one command and prints what came back; see
[bd992_bridge](bd992_bridge.html#bd992_mock). What
follows is the same thing by hand.

`map/graph` first, because it names the graph and the profiles the other two
calls have to match:

```bash
./build/nodes/inspect/inspect call map/graph --data '{"graph":"socal"}' --json
```

That reports `profiles: ["fastest"]` and the graph's bounds. For the SoCal
build, 5 001 449 segments and 9 501 530 edges.

`map/nearest` is the snap: which segments a coordinate could be on. It is also
where a route's time goes, so it is the one to reach for when routing is slow:

```bash
./build/nodes/inspect/inspect call map/nearest --json \
    --data '{"graph":"socal","latitudeDeg":33.6866,"longitudeDeg":-117.8558,
             "radiusM":50.0,"maxCandidates":3}'
```

`map/route` is the search. Irvine to UC Irvine, about 7.3 km:

```bash
./build/nodes/inspect/inspect call map/route --json \
    --data '{"graph":"socal","profile":"fastest",
             "fromLatitudeDeg":33.6866,"fromLongitudeDeg":-117.8558,
             "toLatitudeDeg":33.6405,"toLongitudeDeg":-117.8443,
             "simplifyToleranceM":5.0}'
```

| change | what it shows |
|---|---|
| `"hasFromHeading":true,"fromHeadingDeg":0` vs `180` | the departure heading ranks the start candidates: 7329 m northbound against 7498 m southbound, a genuinely different route |
| `"simplifyToleranceM":0` vs `5` | geometry precision: 414 points against 277, same distance |
| a destination in Santa Barbara (`34.4208,-119.6982`) | a 217 km, 2 h 05 search rather than a local one |
| a coordinate in Denver | `noMatch`, *"no road near the start"*: the coverage edge |
| `"graph":"nope"` | `noSuchGraph`, distinct from a graph that is configured and will not open |

A route is geometry and segment ids, and nothing above that: there are no turn
instructions, and no service resolves a segment id to the name the graph holds
for it. Names, refs and road class reach a client only through `map/nearest`.
`geometry` is a flat list of interleaved lon,lat in units of 1e-7 degrees, so
`[-1178556935, 336866382]` is -117.8556935, 33.6866382. `segmentStarts` indexes
into it, one entry per segment plus a final end, so segment *i* owns points
`[segmentStarts[i], segmentStarts[i+1])`.

## Configuration

`configs/map_server.yaml` has three lists of named files and a block of
service keys. `${REDLINE_DATA_DIR}`, `${VAR}` and `~` expand, and relative
paths resolve against the YAML file's own directory.

| Key | What it is |
|---|---|
| `tilesets[]` | `name` and `path` of each `.mbtiles`. The name is what clients ask for; the path is nobody's business but the node's |
| `graphs[]` | `name` and `path` of each `.graph` from `map_build graph`. Absent is fine: `map/nearest` and `map/route` answer `noSuchGraph` and the tiles are unaffected |
| `tracksets[]` | `name` and `path` of each race-track catalogue, which lives in extra tables inside a tracks `.mbtiles` |
| `services.*_key` | The nine service and topic keys; defaults are the `map/...` names in the next section |
| `services.status_interval_ms` | How often `map/status` is published, default `5000` |
| `assets.root`, `assets.max_bytes` | Files served beside the tiles. Off by default and nothing needs it: the renderer draws with QPainter, so there are no glyph ranges or sprite sheets to fetch |

The tracks archive appears twice on purpose, once under `tilesets` and once
under `tracksets`: `mbtiles::Archive` reads the tiles and `track_store::Store`
reads the catalogue, which carries what tiling cannot (the full-resolution
outline, a centreline with distance along it, and the start/finish gate).
Either half can be present without the other. It is a separate archive from
the basemap because new track maps arrive on their own schedule and rebuilding
them must not mean rebuilding a 383 MB basemap, nor the reverse; it is global
where the basemap is regional, which 994 circuits at 4 MB of tiles make cheap.

A graph's contraction-hierarchy overlay is optional and checked: without it, or
with one built for a different graph, routing answers the same route more
slowly, and `--check` says which of the two it is. An empty graph name in a
request resolves to the only configured graph when there is exactly one.

### Adding a tileset

One entry:

```yaml
tilesets:
  - name: socal
    path: ${REDLINE_DATA_DIR}/maps/socal.mbtiles
```

Two tilesets with one name, or a name containing `/`, are refused at startup;
both would otherwise produce a server that starts cleanly and answers wrongly.
An archive that fails to open is kept, with its error, so "not configured" and
"configured and unreadable" stay different answers.

## The map widget

The `map` widget is registered through `DASHBOARD_WIDGET_TABLE`, so it is in
the editor's palette and settable through `widget_*_config`. Every key is
optional; omitting one keeps the default in
`libs/dashboard_widgets/widgets/map/include/map/config.h`. For a BD992:

```yaml
- type: map
  id: nav
  config:
    tileset: socal
    overlay_tilesets: ["tracks"]
    position_zenoh_key: nodes/bd992/gsof/lat_long_height
    position_schema_type: GsofLatLongHeight
    latitude_expression: latitudeDeg
    longitude_expression: longitudeDeg
    highlight_zenoh_key: nodes/map_match/horizon
    follow_vehicle: true
```

| Key | What it does |
|---|---|
| `tileset`, `overlay_tilesets` | The base archive by name, and extra archives drawn over it in order. Each overlay keeps its own zoom range, cache and backoff, so a missing overlay archive is distinguishable from a hole in coverage |
| `tile_zenoh_key`, `request_timeout_ms` | Where tiles are asked for (`map/tile`) and how long to wait (`4000`) |
| `center_latitude`, `center_longitude`, `zoom`, `bearing` | Where the map opens. Used until a position arrives and whenever Follow Vehicle is off |
| `min_zoom`, `max_zoom` | The camera's limits, `0` and `17`, not the archive's. Which tile level to fetch comes from the server on every reply; past the archive's depth the widget magnifies its deepest tiles, which stays sharp. Default 17 because by z18 there is too little left in frame to be a map |
| `interactive` | Off by default. Drag to pan and wheel to zoom; a dashboard is a surface people brace a hand against on a bad road, so a layout has to ask for a map that moves |
| `follow_vehicle` | Keep the camera on the vehicle. A drag suspends it and the recentre button restores it; the wheel does not suspend it, zooming about the centre instead |
| `orientation`, `heading_expression` | `north_up` holds the configured bearing; `heading_up` turns the map to the vehicle's heading, which needs the expression |
| `view_mode`, `pitch` | `top_down` or `perspective`; pitch is degrees off straight-down, `45` by default, at most `60` |
| `position_*`, `latitude_expression`, `longitude_expression` | The position topic, its schema and the two expressions. Empty key for a static map |
| `highlight_zenoh_key`, `highlight_color`, `highlight_extra_width` | The matched road from `map_match`, recoloured. Way ids only survive in tiles at z13 and deeper, so the highlight quietly disappears further out |
| `marker_*`, `track_*`, `show_track`, `track_points` | The vehicle marker, its outline, and the trail (`600` points) |
| `style.*` | 21 colours, `label_font`, `label_size`, `label_halo_width`, `label_spacing`, `label_repeat_distance` (`250` px between repeats of a road name; `0` is once per viewport), `road_width_scale`, and the `show_*` toggles including `show_road_labels` and `show_water_labels` |
| `style.widths.*` | Per-layer half-width in px at z14. `0` hides the layer and costs nothing |
| `style.detail.*` | Per-layer lowest zoom, plus `road_label` and `water_label` (z14). This can only be stricter than the archive: lowering `building` below z13 draws nothing extra, because nothing wrote those features at those zooms |
| `tile_fade_ms` | Fade-in for new tiles, `150`; the old level stays underneath meanwhile |
| `show_status` | Draw the diagnostic line when no tiles arrive. Without it a stopped `map_server` looks like an empty map |

Out-of-range values are clamped rather than refused, an inverted zoom range is
put in order, and bearing wraps. A camera shallower than the archive's own
minimum zoom asks for 256× the tiles at four levels out and is truncated into a
partly drawn map; raise `min_zoom` to the archive's floor if that matters.

The corner buttons come from `libs/map_controls` and stack bottom-right:
recentre appears once the camera has been moved and drops the pan without
touching the zoom; the compass shows true north, a click cycles
`north_up`/`heading_up`, and dragging its needle spins the map; the view button
toggles `top_down`/`perspective`; the zoom pair steps a level per press and
auto-repeats. These are session state: `orientation`, `view_mode` and `bearing`
are what the layout opens with, and a button press never writes back to them.
`ui_snapshot` lists them as `mapRecentreButton`, `mapCompassButton`,
`mapViewModeButton` and `mapZoomInButton`.

## What it serves

All keys are configurable; these are the defaults.

| Key | Request / response | What it answers |
|---|---|---|
| `map/tile` | `MapTileRequest` / `MapTileResponse` | A batch of tiles from one tileset, each with its own status and encoding; the reply carries the archive's `minzoom`/`maxzoom` |
| `map/catalog` | `MapCatalogRequest` / `MapCatalogResponse` | Every configured tileset with its metadata, or its open error |
| `map/asset` | `MapAssetRequest` / `MapAssetResponse` | A file under `assets.root`; `failed` with the reason unless one is configured |
| `map/graph` | `MapGraphInfoRequest` / `MapGraphInfoResponse` | A graph's bounds, size and routable profiles |
| `map/nearest` | `MapNearestRequest` / `MapNearestResponse` | The segments a coordinate could be on, with name, ref and road class |
| `map/route` | `MapRouteRequest` / `MapRouteResponse` | A route as geometry and segment ids |
| `map/track_catalog` | `MapTrackCatalogRequest` / `MapTrackCatalogResponse` | What tracks exist and where |
| `map/track_detail` | `MapTrackDetailRequest` / `MapTrackDetailResponse` | One track's full-resolution outline, centreline and gate, simplified server-side to a tolerance the caller names |
| `map/status` | `MapServerStatus` (topic) | Per-tileset served, missing and byte counters, and the asset counters, every `status_interval_ms` |

A tile comes back with one of `ok`, `notFound`, `noSuchTileset`, `outOfRange`,
`badRequest` or `failed`. `notFound` means the archive covers that level and
has nothing at that coordinate, which is normal and final; most of the pyramid
is empty. `outOfRange` means the coordinate is real but the archive does not go
to that zoom, so the answer exists shallower. `badRequest` is not a tile
coordinate at all (z past 22, or x/y outside 2^z), a client bug. Keeping the
middle two apart is what the widget's whole zoom behaviour hangs off, and
`map_server_test_tile_range` pins it. Tiles are shipped exactly as stored,
gzipped, with the encoding sniffed from the blob and sent beside the bytes; the
client inflates.

Graph queries answer `noMatch` (nothing within the radius, normal in a car
park), `outOfCoverage`, `noRoute` (both ends matched, nothing connects them) or
`noSuchGraph`; a profile the graph does not offer is `badRequest`, and the list
`map/graph` reports is the list `map/route` accepts.

## Troubleshooting

`--check` first, always. The node is built to degrade rather than refuse: a
stale or absent graph, overlay or archive fails nothing at startup, the node
still answers, and the catalog names each tileset with its error. The graph is
the half more likely to be wrong, being a path plus a sidecar overlay built by
a different verb at a different time, and `--check` is where that shows.

### Diagnosing a blank map

`MapWidget::status()` separates the causes that look identical in a
screenshot, and the widget draws the answer on itself when `show_status` is on.

| Message | Means |
|---|---|
| `No GPU backend — map geometry cannot be drawn` | `QRhi::create()` failed for every backend; labels and marker still draw |
| `No tiles requested` | the widget has no size yet |
| `No reply from map_server on 'map/tile'` | the node is not running, or the key is wrong |
| `Waiting for tiles…` | requests are out, nothing back yet |
| `No coverage here in tileset 'socal'` | tiles arrived and the archive is empty here |

`status()` also reports `camera`, where the map is actually looking, which is
not always the configured centre once Follow Vehicle or a drag has moved it,
and `cameraMoved`, which says a pan is in effect. A screenshot of the wrong
place and a screenshot of an archive with no coverage there are the same
picture. `status().tilesStandIn` staying non-zero means tiles are not arriving
and the widget is drawing from cached neighbours; `status().gpu.devicePixelRatio`
reports the ratio the last frame was really rendered at, which drops below the
screen's when a viewport would exceed the 8192 px texture limit.

### Other silent failures

A map that renders beautifully but mirrored about the equator is a second
TMS/XYZ flip somewhere; the only legitimate one is in `mbtiles::Archive::tile()`.
A layout whose map is empty at every zoom past the archive has the
`outOfRange`/`notFound` distinction backwards somewhere. A routing service that
answers `noSuchGraph` with a graph configured means the file did not open;
`--check` prints why. A trackset pointed at an ordinary basemap reports that
the file has no track tables, which is what it says rather than breakage.

{: .warning }
The vendored `sqlite3` behind `libs/mbtiles` must stay off the dashboard's
and editor's link lines: Qt reaches SQLite through `Qt6::Sql`, which links its
own copy, and the two must never meet in one process. The widget has no use
for it, since it asks this node. Check with
`grep -o sqlite3 build/apps/dashboard/CMakeFiles/dashboard.dir/link.txt`,
which must stay empty.
