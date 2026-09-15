---
title: dashboard
parent: Apps
nav_order: 1
---

# dashboard

The instrument cluster. It loads one YAML layout, opens a window per display
named in it, builds the widgets each window lists, and subscribes each widget to
the bus expressions its configuration names. Nothing is drawn from a bitmap;
every gauge is painted.

## Running it

```bash
./build/apps/dashboard/dashboard -c configs/dashboard/mercedes_190e_dash.yaml
```

| Option | Meaning |
|---|---|
| `-c`, `--config <file>` | The layout. Required. |
| `--config-override <file>` | A second layout that replaces the first when present and loads; used on the target so an operator's file wins over the shipped one. See [Config on the target](on-target.html). |
| `--check` | Load the layout and build its windows headless, then exit. The exit code says whether the file is usable. |
| `--mcp[=<socket>]` | Listen on the agent control socket. See [Agent control](../../developing/agent-control.html). |
| `--debug` | Debug-level logging. |

On a desktop each window opens at its design size. On the target each window is
bound to the display it names, goes full screen and is scaled to fit; see
[Windows and displays](windows.html).

## The layout file

A layout is a list of windows, and each window a list of widgets. Every widget
entry has a `type`, a position and size, an optional `id`, and the configuration
that widget type declares. Data sources are expressions over bus signals, so a
gauge can show `rpm`, or `(coolant_temp_c * 9 / 5) + 32`, or anything else the
expression parser accepts.

The example layouts in `configs/dashboard/` are the best reference while the
widget catalogue is being written: each one exercises a different set of
widgets, and the [editor](../) will show every field a widget accepts.

{: .note }
Set an `id:` on any widget you will want to address again, whether from the
editor, from an agent or from a later edit. Without one the widget is named
`<type>#<index>`, which shifts when widgets are reordered.

## Changing the layout on a running board

The shipped layout lives in a read-only slot. An operator's replacement goes on
the data partition and is picked up through `--config-override`; a file that
fails to load is not retried, so a bad edit cannot take the cluster down twice.
The details are on [Config on the target](on-target.html).
