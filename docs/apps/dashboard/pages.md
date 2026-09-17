---
title: Pages
parent: dashboard
grand_parent: Apps
nav_order: 4
---

# Pages

## Overview

A `page_stack` is a rectangle of a window that shows one of several pages of
widgets. Switching pages hides the others; nothing is destroyed, so CarPlay
keeps playing audio on another page and every hidden gauge stays current. The
page changes on a command from the bus, a `page_button`, a trigger watching any
topic, or the agent interface. A window can hold several stacks, each with its
own pages and state.

The shipped example is the `carplay` window of
`configs/dashboard/mercedes_190e_carplay.yaml`: CarPlay and a vehicle page
share the left 800×600, CarPlay's manufacturer tile leaves it, a button on the
vehicle page comes back, and Grayhill button 1 cycles.

## Configuration

A stack is an ordinary widget entry with a required `id` and a `pages:` list
beside `config:`. Each page has a `name`, an optional `in_cycle`, and a
`widgets:` list placed relative to the stack.

```yaml
- id: main_pages
  type: page_stack
  x: 0
  y: 0
  width: 800
  height: 600
  config:
    default_page: carplay
    triggers:
      - zenoh_key: nodes/grayhill_keypad/buttons
        schema_type: GrayhillButtons
        expression: bit(buttons1To8, 0)
        edge: rising
        action: next
  pages:
    - name: carplay
      widgets:
        - id: carplay_view
          type: carplay
          x: 0
          y: 0
          width: 800
          height: 600
    - name: diagnostics
      in_cycle: false
      widgets: []
    - name: vehicle
      widgets:
        - id: back_to_carplay
          type: page_button
          x: 580
          y: 510
          width: 200
          height: 60
          config:
            label: CarPlay
            command: {target: main_pages, action: go_to, page: carplay}
```

| Key | Where | Meaning |
|---|---|---|
| `id` | the stack | Required. Names the stack's topics, so it is one segment of a zenoh key: no `/`. |
| `default_page` | `config` | The page shown at startup. Empty means the first. |
| `triggers` | `config` | Bus inputs that change the page; see below. |
| `pages[].name` | the stack | Required and unique within the stack. Commands find a page by it. |
| `pages[].in_cycle` | the stack | `false` keeps `next` and `prev` from stopping on the page. Default `true`. |
| `pages[].widgets` | the stack | Ordinary widget entries, positioned relative to the stack. |

A stack cannot be placed on another stack's page. The loader refuses it, along
with a missing or duplicate `id`, a missing or repeated page name, and a
`default_page`, trigger or button naming a page that does not exist. A widget
that overhangs its stack is a warning: it is clipped.

## Commands

| Action | Effect |
|---|---|
| `next` | The next page with `in_cycle` true, wrapping. From a page outside the cycle, the next in-cycle page after its position. |
| `prev` | The same, backwards. |
| `go_to` | The page named in `page`, in the cycle or not. The page already showing is not a change. |
| `back` | The page shown before this one. Twice in a row toggles between two pages. |

A command reaches a stack in one of four ways: a `PageStackCommand` published
on `dashboard/pages/<id>/command`, a `page_button`, a trigger, or
`pages.command` over the [agent interface](../../developing/agent-control.html).

## Triggers

A trigger watches one topic, evaluates an expression over each message, and
applies its action when the expression fires.

| Key | Default | Meaning |
|---|---|---|
| `zenoh_key` | | Topic to watch. |
| `schema_type` | `GrayhillButtons` | Schema of that topic. |
| `expression` | | Non-zero is true. |
| `edge` | `rising` | `rising` fires when the expression becomes true; `on_sample` fires on every true message. |
| `stale_after_ms` | `0` | `rising` only: a gap this long makes the next message a first message again. |
| `action`, `page` | `next` | What to apply, as for a command. |

Use `rising` for a topic that reports a state, such as a keypad's button
bitmask or `CarPlaySessionState.deviceConnected`. Its first message only
primes it, so a button held at startup or a state that is already true changes
nothing. Use `on_sample` for a topic where each message is one event, such as
`CarPlayUiEvent`.

{: .warning }
Leave `stale_after_ms` at 0 for a source that only sends on change, such as a
keypad TPDO. After a quiet spell longer than the timeout the next message would
count as a first message, and the press it carries would only prime the trigger.

Expressions have `bit(x, n)` for bitmask fields, which returns bit `n` of `x`
as 0 or 1: `bit(buttons1To8, 0)` is Grayhill button 1. Enums compare by their
position in the schema, so `kind == 0` is `CarPlayUiEvent.oemButton` and
`kind == 1` is `screenRequested`.

## CarPlay

The carplay node publishes `CarPlayUiEvent` on `nodes/carplay/ui_event` when
the manufacturer tile is tapped (`kind == 0`). A trigger on it is how the tile
leaves CarPlay. With the node's `screen_handover` enabled it also publishes
`screenRequested` (`kind == 1`) when the phone takes the screen back for Siri or
a call, and a trigger on that brings CarPlay forward. See the
[carplay node](../../nodes/carplay.html).

The CarPlay widget pauses video decoding while its page is hidden and keeps
audio and the microphone running. It reports whether it is on screen on
`nodes/carplay/visibility`. With no phone session there is no manufacturer tile,
so set its `return_button` to give the page a way out; see
[Widgets](widgets.html#carplay).

## Topics

| Key | Schema | Direction | Notes |
|---|---|---|---|
| `dashboard/pages/<id>/command` | `PageStackCommand` | in | `action` and, for `goTo`, `page`. |
| `dashboard/pages/<id>/state` | `PageStackState` | out | `current`, `previous`, `index`, `pageCount`; on every change and once a second. |

## Names and selectors

A page is named `<stack>:<page>`. A widget on a page keeps its `id`; without
one it is named `<stack>:<page>:<type>#<index>`. Top-level widgets and their
selector paths are unchanged by a stack beside them. A page's widget has the
path `MainWindow/PageStackWidget/PageStackPage[n]/<Class>`, and a click on a
widget whose page is hidden is refused with `WIDGET_NOT_VISIBLE`.

`widget.set_config` works on a page's widgets and on the stack itself. A
rebuilt stack keeps its pages and stays on the page it was showing; it does
rebuild everything on its pages, so an embedded CarPlay widget reconnects.

## Editing

The editor edits stacks in place: double-click one to work on its pages, drag
widgets onto the page it is showing, and manage the pages from the properties
panel. See [Pages in the editor](../editor.html#pages).

## Troubleshooting

| Symptom | What it is |
|---|---|
| A button or trigger does nothing | `pages_list` over the agent interface shows each trigger's `valid`, `primed` and `fired`. Not primed means no message has arrived on its topic. |
| A keypad press does nothing the first time | expected with `edge: rising`: the first report primes. A second press fires. |
| The config does not load: `a page_stack needs an id` | every stack needs an `id`, and ids are unique across the file. |
| CarPlay on a hidden page still costs CPU | check the dashboard log for `hidden: video decode paused`; if it is absent, the widget is not on a page that was hidden. |
