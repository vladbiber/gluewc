# Configuration

gluewc reads `~/.config/gluewc/config.conf`. The installed session creates it
from `config.def.conf` on first login. Unknown settings are logged and ignored.
Press `Super+Shift+R` after editing; gluewc reloads the file in place.

## Appearance

| Setting | Value | Meaning |
| --- | --- | --- |
| `gap` | integer | gap around tiled windows |
| `border` | integer | focused border width |
| `border_focus` | RRGGBB or RRGGBBAA | insert-mode focus color |
| `border_normal` | RRGGBB or RRGGBBAA | unfocused border color |
| `normal_mode_color` | RRGGBB or RRGGBBAA | normal-mode focus color |
| `unfocused_borders` | boolean | draw borders on unfocused windows |
| `root_color` | RRGGBB or RRGGBBAA | fallback background color |
| `corner_radius` | integer | window corner radius; zero disables it |
| `blur` | boolean | blur behind transparent windows |
| `blur_passes` | integer | SceneFX blur passes |
| `blur_radius` | integer | SceneFX blur radius |
| `opacity` | 0.1–1.0 | opacity used while transparency is enabled |
| `animations` | boolean | enable window, workspace and overview animation |
| `animation_duration` | milliseconds | retile and workspace animation duration |
| `warp_pointer` | boolean | move the pointer to keyboard-focused windows |

### Opening and closing windows

| Setting | Value | Meaning |
| --- | --- | --- |
| `animation_type_open` | `zoom`, `slide`, `fade`, `none` | how a window appears |
| `animation_type_close` | `zoom`, `slide`, `fade`, `none` | how a window disappears |
| `animation_duration_open` | milliseconds | length of the open animation |
| `animation_duration_close` | milliseconds | length of the close animation |
| `zoom_initial_ratio` | 0.05–1.0 | size an opening window starts at |
| `zoom_end_ratio` | 0.05–1.0 | size a closing window ends at |
| `animation_curve_open` | `x1,y1,x2,y2` | easing of the open animation |
| `animation_curve_close` | `x1,y1,x2,y2` | easing of the close animation |

`zoom` grows the window out of its own centre and collapses it back the same
way; `slide` moves it in from below and drops it out; `fade` only touches
opacity. The curves are CSS cubic-beziers — the control points of a curve from
(0,0) to (1,1) — so anything written for a browser works here:

```ini
animation_curve_open  = 0.16,1.0,0.3,1.0    # snappy, the default
animation_curve_open  = 0.25,0.1,0.25,1.0   # plain ease
animation_curve_close = 0.42,0.0,0.6,1.0    # ease in out, the default
```

`y` values outside 0–1 overshoot on purpose; `x` values are clamped to 0–1 so
the curve stays a function of time.

A closing window is animated from a copy of its last frame, so the window is
gone from the layout the moment it is closed and nothing waits on it.

## Layout

Mod+N cycles the focused monitor through the three layouts: BSP, the
niri-style scroll layout and the driftwm-style drift canvas.

| Setting | Value | Meaning |
| --- | --- | --- |
| `layout` | `bsp`, `scroll`, `drift` | layout every monitor starts in |
| `remember_layout` | boolean | reopen in the layout the last session ended in (saved in `$XDG_STATE_HOME/gluewc/layout`), overriding `layout` |
| `drift_snap` | integer | drift: distance at which dragged edges snap, in canvas pixels |
| `drift_nudge` | integer | drift: pixels a window moves per keyboard nudge |
| `drift_zoom_min` | float | drift: how far the camera can zoom out |
| `drift_zoom_max` | float | drift: how far the camera can zoom in |
| `drift_zoom_step` | float | drift: zoom factor per key press or wheel notch |
| `drift_pan_speed` | float | drift: touchpad and scroll panning multiplier |

Fullscreen windows stay opaque and lose gaps, borders and rounded corners.
`wm:toggle_opacity` switches the configured opacity without changing the file.

## Keyboard

```ini
xkb_layout = us
xkb_variant =
xkb_options = ctrl:nocaps
repeat_rate = 25
repeat_delay = 600
```

An empty XKB value uses the system default.

## Autostart

Every `autostart` line is started through `/bin/sh -c`:

```ini
autostart = waybar
autostart = swww-daemon
autostart = swww img ~/Pictures/wallpaper.jpg
```

Wallpaper programs should use a background or bottom layer-shell surface.
gluewc copies those layers into the overview. If none is running, `root_color`
is shown.

## Keybind syntax

```ini
bind_insert = mod+Return = spawn:foot
bind_insert = mod+space = spawn:rofi -show drun
bind_normal = h = wm:ratio:-0.05
```

Modifiers are `mod`/`super`/`logo`, `shift`, `ctrl` and `alt`. Key names use
xkbcommon names. A runtime binding with the same mode, modifiers and key
replaces the compiled default.

Actions:

| Action | Result |
| --- | --- |
| `spawn:COMMAND` | run a shell command |
| `wm:quit` | quit gluewc |
| `wm:reload` | reload the runtime config |
| `wm:overview` | toggle overview |
| `wm:toggle_opacity` | toggle configured window opacity |
| `wm:kill` | close the focused client |
| `wm:mode:insert`, `wm:mode:normal` | switch keyboard mode |
| `wm:focus_left/right/up/down` | directional focus |
| `wm:focus_next` | next BSP leaf |
| `wm:swap_left/right/up/down` | swap directionally |
| `wm:swap_prev`, `wm:swap_next` | swap in tree order |
| `wm:toggle_split` | change the focused BSP split direction; in the scroll layout, maximize the column |
| `wm:toggle_layout` | cycle the monitor through BSP, scroll and drift |
| `wm:layout:bsp`, `wm:layout:scroll`, `wm:layout:drift` | select a layout directly |
| `wm:pan_left/right/up/down` | drift: pan the camera; swaps windows in the other layouts |
| `wm:zoom_in`, `wm:zoom_out`, `wm:zoom_reset` | drift: camera zoom around the viewport centre |
| `wm:zoom_fit` | drift: zoom to fit every window on the workspace |
| `wm:consume` | scroll layout: pull the next column's window into the focused column |
| `wm:expel` | scroll layout: push the focused window into its own column |
| `wm:ratio:VALUE` | adjust the focused split ratio, or the column width in the scroll layout |
| `wm:toggle_fullscreen` | fill the usable area |
| `wm:toggle_real_fullscreen` | cover the complete output |
| `wm:toggle_float_centered` | toggle centered floating |
| `wm:toggle_decorations` | toggle borders and gaps |
| `wm:workspace:N` | show workspace 1–9 |
| `wm:workspace_prev`, `wm:workspace_next` | adjacent workspace |
| `wm:move_to_workspace:N` | move without following |
| `wm:move_to_workspace_follow:N` | move and follow |
| `wm:move_to_workspace_prev/next` | move to an adjacent workspace |

## Overview and gestures

- Tap and release `Super` without another key to toggle the overview.
- Arrow keys and workspace numbers navigate while it is open.
- The mouse wheel changes workspace in the overview.
- A two-finger horizontal overview swipe changes workspace.
- Scroll layout: three-finger vertical swipes change workspace and horizontal
  swipes walk the focus through the windows on the strip, inside and outside
  the overview; a long swipe repeats. Overview arrows do the same: Left/Right
  walk the strip, Up/Down change workspace.
- BSP layout: Super+drag hands the window the tile it is dropped on — the two
  windows trade places in the tree and neither starts floating — and
  Super+right-drag moves the splits the window sits between, so its neighbours
  give up the room instead. The right-drag grabs the edges nearest the click,
  so a corner drag resizes in both directions at once; a window against the
  screen edge has no split there and moves the one on its other side. A window
  that is already floating keeps floating and is moved or resized as before.
- Scroll layout: Super+drag reorders the strip in place (windows never start
  floating) and Super+right-drag resizes the column. Moving focus away from a
  fullscreen window resizes it back into its column, like niri.
- Scroll layout: Mod+F does not fullscreen — it makes the column as wide as
  the screen (like a right-drag resize to full width) and a second press
  restores the previous width. Real fullscreen stays on Mod+Shift+F.
- The overview draws a focus ring around the focused window, following the
  configured focus color; it disappears while decorations are toggled off.
- A three-finger horizontal desktop swipe changes workspace.
- A three-finger upward swipe opens the overview; downward closes it.
- Four fingers change workspace horizontally and open or close the overview
  vertically in every layout; a four-finger pinch in opens the overview and a
  pinch out closes it.
- Only a two-finger pinch zooms the drift camera. Touchpads report a
  three-finger swipe as a pinch as soon as the fingers drift apart, so
  three-finger pinches are treated as swipes instead of zooming by accident.
- Two-finger scrolling never changes workspace: inside the overview it is
  ignored, and elsewhere it belongs to the window under the pointer (or to the
  canvas in the drift layout).
- `Super+wheel` changes workspace in every layout, including drift;
  `Super+Shift+wheel` zooms the drift camera at the pointer.
- Drift layout: three fingers pan the canvas, a two- or three-finger pinch
  zooms the camera, Mod+scroll zooms at the pointer, and scrolling over bare
  canvas pans it. Mod+Shift+drag grabs the canvas itself and pulls it under
  the cursor, which is how a plain mouse gets around a canvas that windows
  cover — the wheel only pans over bare canvas and the three-finger swipe
  needs a touchpad. Mod+drag moves a window with snapping and auto-pan at the
  viewport edges (Alt+drag does the same, and Alt bindings only exist in this
  layout so applications keep Alt+click elsewhere), Mod+right-drag resizes it for real,
  and Alt+Shift+drag moves the whole snapped cluster. Mod+arrow jumps to the
  nearest window in that direction and pans the camera onto it,
  Mod+Shift+arrow nudges the window, Mod+W fits the whole canvas on screen and
  the overview card shows the entire canvas instead of the viewport. The
  overview marks the focused window with a ring only in the scroll layout,
  where the strip has a current window; bsp and drift cards are unmarked.
- Holding `Super` and scrolling changes workspace from the desktop.
- Click a window card to focus it; drag it to a neighboring card to move it.

Touchpad gesture availability depends on libinput and the hardware.

## Bars and shells

Layer-shell panels reserve their exclusive area automatically, including during
overview transitions. gluewc advertises `dwl-ipc-unstable-v2` for workspace
modules and the foreign-toplevel protocol for window lists. A Waybar config can
therefore use the `dwl/tags` module even though gluewc presents them as fixed
workspaces.
