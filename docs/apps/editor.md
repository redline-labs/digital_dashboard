---
title: editor
parent: Apps
nav_order: 2
---

# editor

## Overview

The editor builds and edits dashboard layouts: the YAML files that
[dashboard](dashboard/) runs. It links the same widget library the dashboard
does and builds each widget on the canvas through the same factory, so a gauge
in the editor is the gauge the dashboard will draw, subscribed to the same bus
topics and clamped by the same rules. The canvas is a live preview, not a
picture of one.

It opens empty, as a single unnamed window of `800x600`, and a layout is loaded
either from the command line or from the File menu. Nothing is written until you
save.

## Running it

```bash
# macOS
./build/apps/editor/editor.app/Contents/MacOS/editor -c configs/dashboard/mercedes_190e_dash.yaml
# elsewhere
./build/apps/editor/editor -c configs/dashboard/mercedes_190e_dash.yaml
```

| Option | Meaning |
|---|---|
| `-c`, `--config <file>` | A layout to open at startup. Optional; without it the editor opens empty. A file that fails to load stops the editor with a non-zero exit code. |
| `--mcp[=<socket>]` | Listen on the agent control socket and run headless on the `offscreen` platform. Bare `--mcp` uses `/tmp/redline_agent_<pid>.sock`. See [Agent control](../developing/agent-control.html). |
| `--debug` | Debug-level logging. |
| `-h`, `--help` | Print usage. |

{: .warning }
`--mcp` takes its value with `=`. `--mcp /tmp/a.sock` leaves the path unparsed
and the editor refuses to start rather than listen somewhere you did not ask
for.

## The window

The window is a splitter. The left column holds the palette above the
properties panel; the rest is a scroll area around the canvas, which is drawn at
the window's design size rather than stretched to fit. A toolbar above carries
the window picker, and the status bar has an **Editor Mode** toggle at its right
end.

**The palette** lists every widget type by its friendly name, in the order of
the widget table (Static Text first, Offline Map last). Drag an entry onto the
canvas to add one; it lands with its top-left corner under the cursor, at the
widget's own size hint, with every configuration field at its default. A drag
that does not name a widget type, such as text from a browser, is refused before
it is dropped.

**The canvas** is the window being edited, at its configured size and background
colour, with a `20 px` grid over it in editor mode. Click a widget to select it;
the most recently added widget wins where two overlap. Drag a selected widget to
move it, or drag one of its four corner handles to resize it; a widget cannot be
made smaller than `20x20`. Press Delete or Backspace to remove the selection, and
click empty canvas to clear it. Widgets are free to sit partly off the canvas,
which some of the shipped layouts rely on to show only part of a large gauge.

**The selection frame** is the outline around each widget on the canvas: grey
for unselected widgets, blue with filled corner handles for the selected one. It
owns the widget's stored configuration and rebuilds the preview whenever that
changes, and it is what the agent interface addresses when it names a widget.

**The properties panel** shows the selected widget's friendly name, its selector
name underneath, and one editor per configuration field. Strings get a line
edit, colours a line edit with a swatch that opens a colour picker, enums a
combo box, booleans a check box, integers and floats spin boxes, and lists a
column of rows with Add and per-row remove buttons. A nested configuration
struct, such as the four sub-gauges of the cluster gauge or the map's style, is
drawn as an inset group with its own rows. Each row's label carries the field's
description as a tooltip. Nothing changes on the canvas until you press
**Apply**, which writes every field back to the widget in one step.

Fields whose name is `zenoh_key` or ends in `_zenoh_key` are validated as you
type: a malformed key turns the field red, the reason appears as a tooltip and
under the form, and Apply stays disabled until it is fixed. An empty key is
allowed, since that is how an unbound widget is written. No other field is
refused by the panel. Instead, the value you typed is stored as you typed it,
and the preview is built from a copy clamped by the widget's own `validate()`
(a `max_rpm` of `0` becomes `1`, a `decimals` of `40` becomes `6`), so the
canvas shows what the dashboard would draw while the saved file keeps what you
wrote.

With nothing selected the panel shows the window's own properties: name, width
and height (`100` to `10000`), background colour, display and scale mode. These
apply as you type, though a background colour is only pushed to the canvas once
it parses as a colour.

**Editor Mode** is on by default. Turning it off hides the grid and the frames
and passes clicks through to the live widgets, so an interactive widget such as
the map or CarPlay can be driven inside the editor. Selection is cleared when it
goes off.

**Undo and redo** are on the Edit menu with the platform's standard shortcuts.
The history is snapshot-based, so every change is covered: a drag is one step
however many pixels it crossed, an Apply is one step, a window field edit is one
step per field, and a change made over the agent socket is a step like any
other. A drag that ends where it started, or an Apply that changed nothing,
records nothing. The history is capped at `100` entries and is cleared by a
load. Widgets keep their selector names through an undo. The title bar shows the
window name with an asterisk while the document differs from what was last
saved or loaded.

{: .note }
There is no id field in the properties panel. A widget's `id:` survives a load
and save, and the selector name under the panel heading shows it, but a widget
added in the editor gets only the derived `<type>#<index>` name until an `id:`
is added to the file by hand.

## Saving and loading

File > Load (Ctrl+O or Cmd+O) and File > Save (Ctrl+S or Cmd+S) both open a
file dialog; Save asks for a path every time. Loading replaces the whole
document, warning first if there are unsaved changes, and a load that fails
leaves the current document in place. Closing the window with unsaved changes
asks whether to save, discard or cancel; under `--mcp` it discards with a log
line instead, since nobody is there to answer.

The saved file is a dashboard layout. Run it directly with
`dashboard -c <file>`; nothing editor-specific is written into it. A one-window
layout on the primary display is written in the flat form the shipped configs
use, with the window's keys at the top level, and the `display:` and `scale:`
keys are omitted while they hold their defaults. A widget's `id:` is omitted
when empty.

The round trip is byte-stable for every shipped layout. The editor test suite
loads each file in `configs/dashboard/`, passes it through the canvas, exports
it, and asserts that the emitted YAML is identical to the YAML emitted from the
loaded file. This holds because the frame stores the configuration it was given
rather than reading it back off the clamped widget, and because the canvas keeps
the background colour as the string it was given rather than as a `QColor`.
Comments and key order in a hand-written file are not preserved: the emitter
writes its own.

## Windows

A layout can hold several windows, one per display, and the canvas shows one at
a time. The toolbar's **Window** picker lists them as `name (display)`; choosing
one swaps the canvas over, and is not itself an edit. **Add Window** creates a
window named `window_<n>` at `800x600` on the first display no window holds yet;
**Remove Window** removes the one being shown. Both are undoable, the last window
cannot be removed, and Add is disabled once every display role has a window.

Windows not on the canvas are held as configuration rather than as live widgets,
so a CarPlay widget in a window you are not looking at does not start its
decoder. Each window's display and scale mode are edited in the properties panel
with nothing selected; choosing a display another window already holds swaps
the two, so the document never has two windows on one display. The display
roles are `primary` and `secondary`; see [Windows and displays](dashboard/windows.html)
for what they mean on the target.

## Pages

A [page_stack](dashboard/pages.html) is edited in place. A single click selects
the stack like any widget. Double-click it to go inside: the stack gets an amber
outline with the page being previewed named in its corner, and clicks then reach
the widgets on that page. Clicking outside the stack, or Escape, comes back out;
Escape on a page's widget selects its stack first. Selecting a page's widget any
other way, from the agent interface for instance, also goes inside.

A widget dragged from the palette onto a stack lands on the page it is showing,
at a position relative to the stack. A `page_stack` dropped onto a stack lands on
the window instead, because one stack cannot sit on another's page.

With the stack selected, the properties panel has a **Pages** section under its
settings. Choosing a page there previews it on the canvas, which is not an edit
and is not saved; PageUp and PageDown do the same from the canvas. **Add**,
**Remove**, **Up** and **Down** change the pages, the name field renames one and
**In cycle** decides whether `next` and `prev` stop on it. Each is one undo step.
A page widget's properties start with a **Page** picker that moves it to another
page of the same stack.

{: .note }
Renaming a page follows it through the references to it: the stack's
`default_page` and triggers, and every `page_button` and CarPlay return button in
the document aimed at that stack. Removing the page a `default_page` names clears
it. Triggers and buttons that name a removed page are left as they are, and the
dashboard refuses the file until they are fixed.

A new `page_stack` gets a free id (`pages`, `pages_2`, ...) and one page named
`main`, since a stack without an id does not load. A widget added to a page
without an id is named `<stack>:<page>:<type>#<n>` for this editing session; set
an `id` on anything you will address again, because the dashboard derives the
name from the page's current name and the widget's position.

Undo on a page works as it does on the window: moving one widget there and
undoing it rebuilds nothing else on the page, so a CarPlay preview stays
connected.

## Agent control

Under `--mcp` the editor registers the shared `app.*`, `ui.*`, `input.*`,
`widget.*` and `zenoh.*` methods plus these editor-specific ones. Every one that
takes a `target` uses the selector grammar in
[Agent control](../developing/agent-control.html), and every one that changes
the document goes through the same undo history as the GUI.

| Method | What it does |
|---|---|
| `editor.palette` | Lists every widget type with its friendly name. |
| `editor.items` | Lists the widgets in the window on the canvas, page widgets included, with their rectangles and selection state. A page widget carries `container`, `page` and `visible`, and its rectangle is relative to the stack; `scope` names the stack being edited. |
| `editor.add_widget` | Adds a widget of `type` at `x`, `y`, optionally with `width` and `height`; otherwise the size hint is used. With `container` (a stack selector) and optionally `page`, it goes on that page at a stack-relative position. |
| `editor.palette_drag` | Adds a widget by feeding a synthesized drop, with the palette's payload, to the canvas's real drop handler. |
| `editor.select` | Selects `target`. |
| `editor.move` | Moves `target` to `x`, `y`. |
| `editor.resize` | Resizes `target` to `width`, `height`; both must be positive. |
| `editor.delete` | Removes `target`. |
| `editor.set_mode` | Turns editor mode on or off with `editor_mode`. |
| `editor.undo`, `editor.redo` | Steps the history and reports `can_undo`, `can_redo` and `dirty`. |
| `editor.save` | Writes the document to `path`. |
| `editor.load` | Replaces the document with the file at `path`. |
| `editor.windows` | Lists the windows with name, display, scale, size and which is active. |
| `editor.select_window` | Shows the window named by index or name in `window`. |
| `editor.add_window` | Adds a window, with optional `name`, `display`, `width` and `height`; refused when the name or display is taken. |
| `editor.remove_window` | Removes the window named in `window`; the last one is refused. |
| `editor.pages` | Lists the pages of the stack `target`: name, `in_cycle`, whether shown, and the widgets on each. |
| `editor.add_page` | Adds a page to `target`, with an optional `name`; a repeated name is refused. |
| `editor.remove_page` | Removes `page` (name or index) from `target`; the last page is refused. |
| `editor.rename_page` | Renames `page` to `name`, following the references to it. |
| `editor.set_page` | Sets `in_cycle` on `page`. |
| `editor.move_page` | Moves `page` to position `index`. |
| `editor.show_page` | Previews `page` on the canvas. Not an edit. |
| `editor.move_to_page` | Moves the page widget `target` to `page` of its stack. |
| `editor.scope` | Goes inside the stack `target`, or out with no target. |

`widget.set_config` is the agent's equivalent of Apply, and like Apply it is
refused when the configuration is for a different widget type than the target.

## Troubleshooting

**A widget is missing after loading a file.** The loader skips a widget whose
`type:` is not a known type name and logs a warning with its position. The
widget is dropped from the document and will not be in a save, so fix the file
before saving over it. Type names are the keys in [Widgets](dashboard/widgets.html).

**Apply is greyed out.** A zenoh key field is malformed; it is outlined in red
and the reason is under the form. Clear the field or correct it.

**Clicks do nothing on the canvas.** Editor Mode is off, so clicks are reaching
the widgets themselves. Turn it back on from the status bar. Under `--mcp`, a
snapshot showing `mouse_transparent` on the frames means the same thing.

**A number I typed is drawn differently.** The preview is clamped by the
widget's own validation, and the file keeps the typed value. The clamp message
is in the log as a warning. The ranges are listed with each widget in
[Widgets](dashboard/widgets.html).

**Load then Save changed my file.** Comments and key order are not preserved; the
values are. If a value changed, that is a bug: the round-trip test over
`configs/dashboard/` is where to reproduce it.

**Saving reported failure.** The path was not writable, or the stream failed
mid-write. The status bar says which file; the document is unchanged and still
marked dirty.

**A map or CarPlay widget is blank in the editor.** The preview subscribes to
the same topics the dashboard does, so it shows the same thing the dashboard
would with nothing publishing. Start `map_server` or `carplay --simulate` and
the preview fills in.
