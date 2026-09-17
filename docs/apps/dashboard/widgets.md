---
title: Widgets
parent: dashboard
grand_parent: Apps
nav_order: 3
---

# Widgets

## Overview

A widget is one entry in a window's `widgets:` list. Every entry has a `type`,
which is one of the section headings below, a position and size in `x`, `y`,
`width` and `height`, an optional `id`, and a `config:` block holding the keys
that type declares. A missing `config:` means every default; an unknown key
inside it is warned about and ignored.

```yaml
- type: value_readout
  id: oil_temp
  x: 10
  y: 40
  width: 240
  height: 96
  config:
    label_text: "ENGINE OIL TMP"
    alignment: left
    italic: true
    zenoh_key: "vehicle/engine/temperature_celsius"
    schema_type: "EngineTemperature"
    value_expression: "temperatureCelsius"
```

Widgets that read the bus take the same three keys: a `zenoh_key` naming the
topic, a `schema_type` naming the Cap'n Proto schema published on it, and an
expression evaluated against each message to produce the reading. The
expression can be a field name or arithmetic over several, so a gauge in mph
can read a topic in metres per second. `schema_type` is a schema name from the
registry, such as `VehicleSpeed` or `EngineRpm`; `zenoh_describe_schema` under
[agent control](../../developing/agent-control.html) lists a schema's fields.
An empty `zenoh_key` leaves the widget unbound, which is how a layout is
sketched before the signals exist.

Numeric fields are clamped, not refused: a value outside the range a widget can
draw is pulled into range with a warning in the log, and the file is left as
written. The ranges are given below where a config declares one. Colours are hex
strings, `#RRGGBB` or `#RRGGBBAA`.

The [editor](../editor.html) shows every field a widget accepts, with its
description, and previews the result; the layouts in `configs/dashboard/` show
each widget in use. Defaults below are the ones declared in each widget's
`config.h`; a dash means none is declared there.

## static_text

A line of text in a chosen font and colour. Reads nothing from the bus.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `text` | string | `Your Text Here` | The text to display. |
| `font` | string | `Arial` | Font family name. |
| `font_size` | int | `12` | Point size. |
| `color` | color | `#000000` | Text colour. |

## road_info

The road under the vehicle, from the map matcher's electronic horizon: name,
route number, posted speed limit and, optionally, the matcher's confidence. It
expects a `MapHorizon` topic published by `nodes/map_match`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `horizon_zenoh_key` | string | `nodes/map_match/horizon` | Topic carrying the horizon. |
| `horizon_schema_type` | enum | `MapHorizon` | Schema of the horizon topic. |
| `show_name` | bool | `true` | Display the road name. |
| `show_ref` | bool | `true` | Display the route number, e.g. I-405. |
| `show_speed` | bool | `true` | Display the posted speed limit. |
| `show_confidence` | bool | `false` | Display the matcher's confidence and position sigma. A diagnostic. |
| `font` | string | `Arial` | Font family. |
| `name_font_size` | int | `22` | Size of the road name, in points. |
| `detail_font_size` | int | `13` | Size of the ref and speed limit, in points. |
| `text_color` | color | `#FFFFFF` | Colour of the road name. |
| `detail_color` | color | `#9E9E9E` | Colour of the ref and speed limit. |
| `background_color` | color | `#00000000` | Panel background; transparent by default. |
| `no_limit_text` | string | `--` | Shown when OSM records no posted limit. |
| `no_road_text` | string | `No road` | Shown when there is a fix but no road under it. |
| `no_fix_text` | string | `Waiting for position` | Shown before the first horizon arrives. |
| `speed_in_mph` | bool | `true` | Convert the posted limit from km/h to mph. |

## value_readout

A label and a number, drawn in the MoTeC display style. The number can be
formatted as a plain value with a units suffix or as a lap time.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `label_text` | string | `Untitled` | Label text. |
| `alignment` | enum | `left` | `left`, `right` or `center`. |
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `VehicleSpeed` | Schema of the topic. |
| `value_expression` | string | `""` | Expression producing the value. |
| `format` | enum | `number` | `number`, or `lap_time` to render seconds as `m:ss.SS`. |
| `decimals` | int | `0` | Digits after the decimal point in `number` format. Clamped to `0`–`6`. |
| `units` | string | `""` | Suffix printed after the value. |
| `show_sign` | bool | `false` | Always print a leading `+` on positive values. |
| `label_color` | color | `#FFA500` | Label colour. |
| `value_color` | color | `#FFFFFF` | Value colour. |
| `italic` | bool | `false` | Render label and value in italics. |

## segment_readout

A segmented-LCD readout in a DSEG face, with the unlit segments of every cell
drawn faintly behind the lit ones as a real panel shows them. With no expression
it shows fixed text, which is how the CDL3 layout's alphanumeric fields are set.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `VehicleSpeed` | Schema of the topic. |
| `value_expression` | string | `""` | Expression producing the displayed value. |
| `static_text` | string | `""` | Fixed text shown when there is no expression. |
| `prefix` | string | `""` | Fixed text in the leading cells; the value is right-aligned after it. |
| `face` | enum | `seven` | `seven` for digits only, `fourteen` for alphanumerics. |
| `digits` | int | `4` | Number of cells, which also fixes the drawn width. Clamped to `1`–`16`. |
| `decimals` | int | `0` | Digits after the decimal point. Clamped to `0`–`6`. |
| `lit_color` | color | `#101820` | Colour of the driven segments. |
| `ghost_color` | color | `#5AB4BE` | Colour of the undriven segments. |
| `show_ghosts` | bool | `true` | Draw the undriven segments. |
| `caption` | string | `""` | Small label drawn beside the value. |
| `caption_position` | enum | `right` | `right` of the value, or `top`. |
| `caption_color` | color | `#101820` | Caption colour. |

## center_bar

A horizontal bar whose origin is the centre: the marker sits in the middle at
zero and travels either way. This is the MoTeC gain/loss strip, and suits
anything signed around a target.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `VehicleSpeed` | Schema of the topic. |
| `value_expression` | string | `""` | Expression producing the signed value. |
| `range` | float | `1.0` | Full-scale deflection either side of centre, in the value's units. Must be positive; otherwise reset to `1`. |
| `left_label` | string | `LOSS` | Label at the left end. |
| `right_label` | string | `GAIN` | Label at the right end. |
| `negative_is_good` | bool | `true` | Colour negative values with `good_color`. |
| `track_color` | color | `#333333` | Colour of the unfilled bar. |
| `good_color` | color | `#39B54A` | Marker colour on the good side. |
| `bad_color` | color | `#C4281E` | Marker colour on the bad side. |
| `label_color` | color | `#AAAAAA` | Colour of the end labels. |
| `tick_color` | color | `#777777` | Colour of the centre tick. |

## mercedes_190e_speedometer

The 190E speedometer dial with a needle and a six-digit odometer. It reads two
topics: one for road speed and one for the odometer.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `odometer_value` | int | `0` | Starting odometer reading. Clamped to `0`–`999999`. |
| `max_speed` | int | `125` | Full-scale reading at the end of the dial. Clamped to `1`–`1000`. |
| `zenoh_key` | string | `""` | Topic the road speed is read from. |
| `schema_type` | enum | `VehicleSpeed` | Schema of the speed topic. |
| `speed_expression` | string | `""` | Expression producing speed in the dial's own units. |
| `odometer_expression` | string | `""` | Expression producing the odometer reading. |
| `odometer_zenoh_key` | string | `""` | Topic the odometer is read from. |
| `odometer_schema_type` | enum | `VehicleOdometer` | Schema of the odometer topic. |
| `shift_box_markers` | list of int | `[]` | Speeds, in dial units, at which to draw a shift box on the face. At most `64` entries. |

## mercedes_190e_tachometer

The 190E tachometer with a red zone and, optionally, the analogue clock inset
in its face.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `max_rpm` | int | `7000` | Full-scale reading. Clamped to `1`–`30000`. |
| `redline_rpm` | int | `6000` | Where the red zone begins. Clamped to at most `max_rpm`. |
| `show_clock` | bool | `true` | Draw the clock inset. |
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `EngineRpm` | Schema of the topic. |
| `rpm_expression` | string | `""` | Expression producing engine RPM. |

## mercedes_190e_cluster_gauge

The 190E centre cluster: four small sub-gauges around a face, with the tapered
ECONOMY band drawn over the bottom one. `fuel_gauge`, `right_gauge`,
`bottom_gauge` and `left_gauge` are each a struct with these keys; an inverted
`min_value`/`max_value` pair is swapped.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `min_value` | float | `0.0` | Reading at the empty end of the sweep. |
| `max_value` | float | `100.0` | Reading at the full end of the sweep. |
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `VehicleSpeed` | Schema of the topic. |
| `value_expression` | string | `""` | Expression producing the reading. |

`economy_sweep` is a struct describing the band over the bottom gauge; the value
it indicates comes from `bottom_gauge`.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `label` | string | `ECONOMY` | Text printed above the sweep. |
| `red_start_fraction` | float | `0.60` | Fraction along the sweep where the red section begins. Clamped to `0`–`1`. |
| `outline_color` | color | `#FFFFFF` | Colour of the band outline and label. |
| `red_color` | color | `#C4281E` | Fill colour of the uneconomical section. |

## sparkline

A scrolling line graph of one signal with the current value and a units label
over it.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `units` | string | `Untitled` | Units text, e.g. mph. |
| `min_value` | float | `0.0` | Bottom of the Y axis. Swapped with `max_value` if inverted. |
| `max_value` | float | `100.0` | Top of the Y axis. |
| `line_color` | color | `#0000FF` | Graph colour. |
| `text_color` | color | `#FFFFFF` | Colour of the value and units text. |
| `font_family` | string | `Arial` | Font family. |
| `font_size_value` | int | `24` | Font size of the value. Clamped to `1`–`200`. |
| `font_size_units` | int | `10` | Font size of the units label. Clamped to `1`–`200`. |
| `update_rate` | int | `30` | Graph update rate in Hz. Clamped to `1`–`240`. |
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `VehicleSpeed` | Schema of the topic. |
| `value_expression` | string | `""` | Expression producing the value. |

## background_rect

A filled rectangle, solid or with a linear gradient, for layering behind other
widgets. Reads nothing from the bus.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `colors` | list of color | `[]` | Gradient stops in order. One colour gives a solid fill. |
| `direction` | enum | `vertical` | `vertical` or `horizontal`. |

## mercedes_190e_telltale

One warning lamp from the 190E cluster. It lights while the condition expression
evaluates non-zero. Each lamp is its own widget, so a row of five is five
entries.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `telltale_type` | enum | `battery` | Which symbol: `battery`, `brake_system`, `high_beam` or `windshield_washer`. |
| `warning_color` | color | `#FF0000` | Lamp colour while the condition holds. |
| `normal_color` | color | `#333333` | Lamp colour otherwise. |
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `VehicleSpeed` | Schema of the topic. |
| `condition_expression` | string | `""` | Expression; the lamp lights when it is non-zero. |

## motec_c125_tachometer

The MoTeC C125 sweep tachometer with a large centre digit, a page banner and a
scale caption.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `max_rpm` | int | `6000` | Full-scale reading. Clamped to `1`–`30000`. |
| `redline_rpm` | int | `5000` | Where the red zone begins. Clamped to at most `max_rpm`. |
| `center_page_digit` | int | `5` | The large centre digit; the gear on a real display. |
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `EngineRpm` | Schema of the topic. |
| `rpm_expression` | string | `""` | Expression producing engine RPM. |
| `page_label` | string | `RACE` | Banner above the centre digit. |
| `scale_label` | string | `RPMx1000` | Caption under the centre digit. |
| `italic` | bool | `true` | Set the text in an italic face. |
| `fill_color` | color | `#FFB400` | Sweep colour below the redline. |
| `redline_color` | color | `#DC0000` | Sweep colour at and above the redline. |
| `ring_color` | color | `#C8C8C8` | Outer ring and tick marks. |
| `digit_color` | color | `#FFFFFF` | Centre digit and dial labels. |

## motec_cdl3_tachometer

The MoTeC CDL3 segmented RPM bargraph and its scale. The widget assumes a
square; the CDL3 layout oversizes it and offsets it upward so only the arc shows.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `max_rpm` | int | `6000` | Full-scale reading; sets how many segments the bar spans. Clamped to `1`–`30000`. |
| `zenoh_key` | string | `""` | Topic to subscribe to. |
| `schema_type` | enum | `EngineRpm` | Schema of the topic. |
| `rpm_expression` | string | `""` | Expression producing engine RPM. |

## carplay

The projected CarPlay screen. The widget is a thin client of the
[carplay node](../../nodes/carplay.html), which owns the phone session: it
decodes the video the node publishes, plays its audio, and publishes touch and
microphone back. The keys must match the node's configuration; there is no
schema or expression to choose.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `video_key` | string | `nodes/carplay/video` | Topic the node publishes the phone's H.264/H.265 screen on. |
| `audio_key` | string | `nodes/carplay/audio` | Topic the node publishes phone audio on. |
| `mic_key` | string | `nodes/carplay/mic` | Topic this widget publishes captured microphone audio on, for Siri and calls. |
| `input_key` | string | `nodes/carplay/input` | Topic this widget publishes touch events to. |
| `session_key` | string | `nodes/carplay/session` | Topic carrying session state: whether a phone is connected and what it is doing. |
| `visibility_key` | string | `nodes/carplay/visibility` | Topic this widget reports whether it is on screen on, so the node can hand the screen to the car. |
| `session_stale_after_ms` | int | `3000` | No session state for this long means no driver, and the return button shows. Clamped to `50`–`600000`. |
| `return_button.enabled` | bool | `false` | Draw a button to leave the page while no phone session is live. |
| `return_button.label` | string | `Vehicle` | Text on the button. |
| `return_button.command` | command | | `target`, `action` and `page`, as for a [page command](pages.html#commands). |
| `return_button.width`, `.height` | int | `200`, `56` | Button size; it sits bottom centre. |

The widget decodes video only while it is visible. On a hidden
[page](pages.html) it drops the video subscription, keeps the last frame for
when it returns, and carries on playing audio and capturing the microphone. It
publishes `CarPlayVisibility` on every change and once a second.

The return button shows unless session state is arriving, a device is connected
and the session is recording. A driver that stops leaves "recording" as its last
message, which is why staleness counts too. The button is addressable as
`#<widget id>:return`.

## now_playing

Track metadata and album art from the CarPlay node, with a progress bar. An
active call takes the widget over for its duration and hands it back to the
music afterwards. It subscribes to the node's `CarPlayNowPlaying` and
`CarPlayCall` topics and needs no video surface.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `zenoh_key` | string | `nodes/carplay/nowplaying` | Topic publishing now-playing metadata. |
| `show_album_art` | bool | `true` | Draw album artwork when the phone provides it. |
| `show_progress` | bool | `true` | Draw the progress bar and elapsed/duration times. |
| `title_color` | color | `#FFFFFF` | Track title colour. |
| `detail_color` | color | `#AAAAAA` | Artist, album and app text colour. |
| `accent_color` | color | `#FFA500` | Progress bar colour. |
| `show_calls` | bool | `true` | Let an active call take the widget over. |
| `call_zenoh_key` | string | `nodes/carplay/call` | Topic publishing call state. |
| `call_accent_color` | color | `#39B54A` | Colour of the call badge and status text. |
| `transition_ms` | int | `260` | Cross-fade between music and call. Clamped to `0`–`2000`. |
| `call_linger_ms` | int | `1600` | How long "Call ended" stays up before the music returns. Clamped to `0`–`10000`. |

## carplay_nav

Turn-by-turn guidance from CarPlay: the manoeuvre arrow, distance to it, the
road being turned onto and a trip summary strip. It subscribes to the node's
`CarPlayNav` topic and shows `idle_text` when the phone reports no route.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `zenoh_key` | string | `nodes/carplay/nav` | Topic publishing guidance. An empty key is reset to this. |
| `imperial_units` | bool | `false` | Show feet and miles instead of metres and kilometres. |
| `show_trip_summary` | bool | `true` | Draw remaining distance, remaining time and ETA. |
| `arrow_color` | color | `#39B54A` | Manoeuvre arrow colour. |
| `distance_color` | color | `#FFFFFF` | Distance-to-manoeuvre text colour. |
| `road_color` | color | `#FFFFFF` | Colour of the road name being turned onto. |
| `detail_color` | color | `#AAAAAA` | Secondary text and trip summary colour. |
| `background_color` | color | `#00000000` | Fill behind the card; transparent by default. |
| `idle_text` | string | `No route` | Shown when there is no active route. |

## page_stack

A region showing one of several pages of widgets. It is the one widget with a
`pages:` list beside `config:`, and it needs an `id`. Pages, commands, triggers
and topics are described on [Pages](pages.html).

| Key | Type | Default | Meaning |
|---|---|---|---|
| `default_page` | string | `""` | Page shown at startup; empty means the first. |
| `triggers` | list | `[]` | Bus inputs that change the page: `zenoh_key`, `schema_type`, `expression`, `edge`, `stale_after_ms`, `action`, `page`. |

## page_button

A touch target that sends a page command. It sends over the bus, so the button
and the stack need not share a window.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `label` | string | `Back` | Text on the button. |
| `command.target` | string | `""` | The `id` of the page_stack to change. |
| `command.action` | enum | `next` | `next`, `prev`, `go_to` or `back`. |
| `command.page` | string | `""` | Page name, for `go_to`. |
| `background_color` | color | `#222222` | Fill while not pressed. |
| `pressed_color` | color | `#444444` | Fill while pressed. |
| `text_color` | color | `#FFFFFF` | Label colour. |
| `font_size` | int | `18` | Label size in points. Clamped to `4`–`200`. |
| `corner_radius` | int | `8` | Corner rounding in pixels. Clamped to `0`–`500`. |

A press that slides off the button before release sends nothing.

## map

The offline map. Every tile comes from [map_server](../../nodes/map_server.html)
over the `tile_zenoh_key` service, asked for by tileset name and z/x/y; there is
no URL and no style document. The vehicle position comes from any topic with
latitude and longitude fields, and an optional horizon topic from the map
matcher lights up the matched road ahead. The camera's zoom range says nothing
about which tiles exist: map_server reports its archive's range on every reply
and the widget stays within it.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tileset` | string | `socal` | Tileset name as configured in map_server. |
| `overlay_tilesets` | list of string | `[]` | Extra tilesets drawn over the base one, e.g. `tracks`, each from its own archive. |
| `tile_zenoh_key` | string | `map/tile` | Service key map_server answers tile requests on. |
| `request_timeout_ms` | int | `4000` | How long to wait for a tile. Clamped to `100`–`30000`. |
| `min_zoom` | int | `0` | Shallowest the camera may go. Clamped to `0`–`22`. |
| `max_zoom` | int | `17` | Deepest the camera may go; past the archive's depth it magnifies the deepest tiles. Clamped to `0`–`22`, and swapped with `min_zoom` if inverted. |
| `center_latitude` | float | `33.6865966` | Degrees north, used until a position arrives and whenever follow is off. Clamped to Web Mercator's range. |
| `center_longitude` | float | `-117.8557874` | Degrees east. Clamped to `-180`–`180`. |
| `zoom` | float | `13.0` | Camera zoom; `14` is street level. Clamped to `0`–`22`. |
| `bearing` | float | `0.0` | Map rotation in degrees clockwise from north. Wrapped into `0`–`360`. |
| `interactive` | bool | `false` | Allow dragging to pan and the wheel to zoom. A recentre button appears once the camera has moved. |
| `follow_vehicle` | bool | `true` | Keep the camera centred on the vehicle. |
| `orientation` | enum | `north_up` | `north_up` keeps the configured bearing; `heading_up` turns the map to the vehicle's heading and needs `heading_expression`. |
| `view_mode` | enum | `top_down` | `top_down` is the flat map; `perspective` tilts it back to `pitch`. |
| `pitch` | float | `45.0` | Tilt of the perspective view, in degrees. Clamped to `10` and the projection's maximum. |
| `show_track` | bool | `true` | Draw a trail behind the vehicle. |
| `track_points` | int | `600` | How many positions the trail keeps; `0` disables it. |
| `position_zenoh_key` | string | `""` | Topic carrying the vehicle position, e.g. `nodes/bd992/position`. |
| `position_schema_type` | enum | `GsofLatLongHeight` | Schema of the position topic. |
| `latitude_expression` | string | `""` | Expression yielding degrees north. |
| `longitude_expression` | string | `""` | Expression yielding degrees east. |
| `heading_expression` | string | `""` | Expression yielding degrees clockwise from north. Optional. |
| `highlight_zenoh_key` | string | `""` | `MapHorizon` topic, e.g. `nodes/map_match/horizon`. The matched road ahead is recoloured; way ids only exist at zoom 13 and deeper, so it vanishes when shallower. Empty disables it. |
| `highlight_color` | color | `#00E5FFB0` | Colour of the matched road. |
| `highlight_extra_width` | float | `2.0` | Extra half-width in pixels beyond the road's own. Clamped to `0`–`20`. |
| `marker_color` | color | `#FF3B30` | Vehicle marker and trail colour. |
| `marker_size` | int | `9` | Marker radius in pixels. Clamped to `2`–`64`. |
| `marker_outline_color` | color | `#FFFFFF` | Ring around the marker. |
| `track_width` | float | `3.0` | Trail line width in pixels. Clamped to `0.5`–`20`. |
| `track_opacity` | float | `0.7` | Trail opacity, `0` to `1`. |
| `style` | struct | — | Colours, widths and zoom thresholds for the map itself. See below. |
| `tile_fade_ms` | int | `150` | Fade-in for a newly arrived tile; `0` disables it. Clamped to `0`–`1000`. |
| `show_status` | bool | `true` | Draw a line of text when no tiles are arriving. |

`style` holds the map's look. Field names are OpenMapTiles layer and class
names. Thresholds here can only be stricter than the archive: lowering one
below what the archive carries draws nothing extra.

| Key | Type | Default | Meaning |
|---|---|---|---|
| `background` | color | `#16181d` | Everything the map does not cover. |
| `water` | color | `#0f2231` | Lakes and sea. |
| `waterway` | color | `#0f2231` | Rivers, streams and canals. |
| `landcover` | color | `#1b2a20` | Wood, forest, grass and farmland. |
| `landuse` | color | `#1b1d23` | Residential and built-up areas. |
| `park` | color | `#1b2a20` | Parks and nature reserves. |
| `building` | color | `#242830` | Building footprints. |
| `aeroway_surface` | color | `#20242c` | Aprons, aerodrome grounds and helipads. |
| `aeroway_line` | color | `#39404b` | Runways and taxiways. |
| `bridge_casing` | color | `#0d0f13` | Outline around a bridge deck. |
| `road_minor` | color | `#2b3038` | Residential streets, service roads and tracks. |
| `road_major` | color | `#3a414c` | Secondary and tertiary roads. |
| `road_primary` | color | `#59616f` | Primary roads and trunk routes. |
| `motorway` | color | `#d9a441` | Motorways. |
| `motorway_casing` | color | `#8a5a1e` | Outline under a motorway. |
| `rail` | color | `#3c3f47` | Railway lines. |
| `boundary` | color | `#4a4f5c` | State and country borders. |
| `racetrack_surface` | color | `#2a2f3a` | The tarmac ribbon of a race track. |
| `racetrack_centre` | color | `#7fd8ff` | A race track's derived centre line. |
| `label_text` | color | `#c3cad6` | Place name colour. |
| `label_halo` | color | `#12141a` | Outline behind label text. |
| `label_font` | string | `Arial` | Family for place names. |
| `label_size` | int | `12` | Point size for place names. Clamped to `6`–`48`. |
| `label_halo_width` | float | `3.0` | Halo stroke width in pixels; `0` draws bare text. Clamped to `0`–`12`. |
| `label_spacing` | int | `4` | Clear space around each label in pixels. Clamped to `0`–`64`. |
| `label_repeat_distance` | int | `250` | How far apart a road or river repeats its name; `0` names each once per screen. Clamped to `0`–`4096`. |
| `road_width_scale` | float | `1.0` | Multiplier on every width in `widths`. Clamped to `0.1`–`8`. |
| `show_buildings` | bool | `true` | Draw building footprints. |
| `show_labels` | bool | `true` | Draw place names. |
| `show_boundaries` | bool | `true` | Draw borders. |
| `show_aeroways` | bool | `true` | Draw runways, taxiways and aprons. |
| `show_bridges` | bool | `true` | Draw bridges above the roads they cross. |
| `show_road_labels` | bool | `true` | Draw street names. |
| `show_water_labels` | bool | `true` | Draw river and lake names. |
| `show_racetracks` | bool | `true` | Draw race tracks, when one is configured as an overlay tileset. |
| `widths` | struct | — | Per-layer half-widths in pixels at zoom 14. See below. |
| `detail` | struct | — | Per-layer lowest zoom. See below. |

`style.widths` holds a float per line layer: the half-width in pixels at zoom
14, before `road_width_scale` and the zoom taper. Zero hides the layer. Each is
clamped to `0`–`40`.

| Key | Default | Key | Default |
|---|---|---|---|
| `motorway` | `3.75` | `aeroway_runway` | `7.0` |
| `motorway_casing` | `5.5` | `aeroway_taxiway` | `2.0` |
| `road_primary` | `3.5` | `bridge_casing` | `1.5` |
| `road_major` | `2.5` | `waterway` | `1.25` |
| `road_minor` | `1.5` | `boundary` | `0.9` |
| `rail` | `0.9` | `racetrack_centre` | `1.0` |

`style.detail` holds an integer per layer: the lowest camera zoom at which it is
drawn. Raising one thins a cluttered map at no cost. Each is clamped to `0`–`22`.

| Key | Default | Key | Default |
|---|---|---|---|
| `landcover` | `0` | `rail` | `11` |
| `landuse` | `9` | `aeroway_surface` | `11` |
| `park` | `11` | `aeroway_runway` | `10` |
| `water` | `0` | `aeroway_taxiway` | `13` |
| `waterway` | `8` | `road_label` | `14` |
| `building` | `13` | `water_label` | `14` |
| `road_minor` | `12` | `bridge` | `13` |
| `road_major` | `9` | `boundary` | `0` |
| `road_primary` | `7` | `racetrack_surface` | `11` |
| `motorway` | `5` | `racetrack_centre` | `12` |
