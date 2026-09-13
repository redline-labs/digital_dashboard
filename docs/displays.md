# Windows and displays

One dashboard config can open several windows. Each window has a design size and
names the display it belongs on. On the target, each window is bound to that
display, goes full screen and is scaled to fit it. Everywhere else (a desktop, a
Mac, `--mcp`), the windows open as ordinary windows at their design size, and
nothing about the display is looked at.

## The config

The original flat form is still one window, and every existing config is in it:

```yaml
name: instrument_cluster
width: 1200
height: 450
background_color: "#000000"
widgets: [...]
```

For more than one window, use a `windows:` list:

```yaml
name: mercedes_190e_carplay        # the document's name (optional)
windows:
  - name: instrument_cluster
    display: primary               # primary | secondary; default primary
    scale: fit                     # fit | none; default fit
    width: 1200
    height: 450
    widgets: [...]
  - name: carplay
    display: secondary
    width: 1200
    height: 600
    widgets: [...]
```

`configs/dashboard/mercedes_190e_carplay.yaml` is a complete example.

The loader refuses:

- a `windows:` list that is empty;
- `widgets:` at the top level next to `windows:`;
- a `display` or `scale` it does not know;
- two windows with the same name. The name roots agent selectors, so a
  duplicate makes both windows ambiguous;
- two windows on the same display. A window with no `display:` counts as
  primary.

Saving writes the smallest form that still says everything. A document with one
window, on the primary display, with `scale: fit` and no document name, is
written flat. Anything else is written as a list. This is why an existing config
comes back from an editor save byte-identical.

## What the target publishes

`redline-display-setup.service` runs before the dashboard. It detects the
FPD-Link display modules and records what it found. When no module is detected
it writes nothing, and nothing in this document applies. On the LattePanda with
the Rivian IC panel, a boot produces:

```
# /run/redline/display.env, imported by redline-dashboard.service
REDLINE_DISPLAY_PRIMARY_CONNECTOR=HDMI-A-2
REDLINE_DISPLAY_PRIMARY_PROFILE=rivian-ic-la123wf9
REDLINE_DISPLAY_PRIMARY_MODE=1920x720
```

- A second module adds the same keys with `SECONDARY`.
- The roles are fixed names for port pairs on the housing, set in
  `/etc/redline/display.conf` (`primary = i2c-11:0x0c HDMI-A-2`,
  `secondary = i2c-12:0x0c HDMI-A-1`). They are never inferred.
- The per-display records in `/run/redline/displays/<role>` are read by
  [the backlight node](backlight.md). The dashboard reads only the environment.

## How a window is placed

`dashboard/include/dashboard/display_binding.h` makes the decisions. It is pure
C++ with the environment passed in, and `dashboard_test_display_binding` tests it.

**No `REDLINE_DISPLAY_*_CONNECTOR` set** (desktop, Mac, `--mcp`): every window is
shown at its fixed design size. The environment is not touched.

**On the target:**

1. Scaling is set up before `QApplication` is constructed. For every `scale: fit`
   window whose display has both a connector and a mode, the factor is
   `min(mode.w / width, mode.h / height)`. These factors go into
   `QT_SCREEN_SCALE_FACTORS` as `connector=factor` pairs. For the 190E cluster
   on the Rivian panel this is `HDMI-A-2=1.6`. Nothing else on the target should
   set `QT_SCALE_FACTOR`: Qt multiplies it on top of the per-screen factors.
   - Factors are per screen, not one global value, because two windows with
     different design sizes on two panels need two factors.
   - The chosen value is logged (`Displays: QT_SCREEN_SCALE_FACTORS=...`).
     Check the log, not `/proc/<pid>/environ`: that file only shows the
     environment the process started with.
2. Each window is matched to the `QScreen` whose `name()` is its role's
   connector. Under eglfs_kms, `name()` is the DRM connector name. The window is
   moved to that screen and shown full screen.
3. The layout keeps its design size and is centred. If the panel's aspect ratio
   differs, the bars show the window's `background_color`. Widgets remain direct
   children of `MainWindow`, so agent selector paths do not change.
4. **The display is not there:**
   - A primary window logs one line listing the screens that do exist, then uses
     Qt's primary screen. This is what happened before binding existed.
   - A secondary window logs one line and is not built. On eglfs it would
     otherwise land full screen on top of the primary, and it would start
     subscriptions (CarPlay's decoder among them) for nothing.

To exercise the binding on a desktop, export the variables yourself. The lookup
has no platform-specific code:

```sh
REDLINE_DISPLAY_PRIMARY_CONNECTOR="<a screen name from the fallback log line>" \
REDLINE_DISPLAY_PRIMARY_MODE=2400x900 \
./build/dashboard/dashboard -c configs/dashboard/mercedes_190e_dash.yaml
```

## Editing

The editor holds the whole document and shows one window at a time.

- The toolbar's window picker switches between windows. Switching is not an
  edit, so it does not dirty the document.
- **Add Window** creates a window on the first display no window has yet.
- **Remove Window** removes the window being shown.
- The window page's **Display** and **Scale** fields set the placement. Choosing
  a display another window already has swaps the two windows' displays, so the
  editor cannot produce a file the loader rejects.
- Adding, removing and changing a display are all undoable. Undo returns to the
  window the edit was made in.

The agent interface has the same operations: `editor.windows`,
`editor.select_window`, `editor.add_window` and `editor.remove_window`. Every
other editor verb acts on the window being shown. See
[agent_control.md](agent_control.md).

## Still to do on the target

- **Two windows on two eglfs screens is unproven.** The image has no Wayland
  plugin, so both windows go through eglfs_kms's per-screen compositing.
  `/run/redline/kms.json` lists every display `redline-display` detected (only
  the primary exists today), so nothing more is needed on that side. Check this
  as soon as a second panel is attached.
- **Named `QT_SCREEN_SCALE_FACTORS` is not yet verified under eglfs.**
  `QT_LOGGING_RULES=qt.highdpi.*=true` prints the factor Qt applied to each
  screen.
- ~~**The Yocto side drops its `QT_SCALE_FACTOR` settings**~~ Done 2026-09-12:
  both the profile's 1.6 and the unit's 3.2 are gone; nothing on the rootfs sets
  `QT_SCALE_FACTOR`.
- ~~**The deployed dashboard's `main.cpp` differs from this tree.**~~ The
  `startup: … at N ms` marks now live in this tree and the Yocto layer carries
  no patches against it.
