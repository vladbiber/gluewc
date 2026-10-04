# Configuration

gluewc reads `~/.config/gluewc/config.conf`. The installed session creates it
from `config.def.conf` on first login.

## Saving applies

The file is watched, so saving it is all it takes — the new settings and
keybinds are live about a tenth of a second later, with no restart and nothing
to press. Editors that write a temporary file and rename it over the original
are handled too, which is what vim and most others do. `Super+Shift+R` still
forces a reload, and remains the way to reload after editing the file from
somewhere the watch cannot see, such as a different machine over a network
mount.

A line the parser rejects is skipped, and everything that did parse still
applies — a typo can never leave the session without keybinds. To make sure a
mistake is not missed, gluewc reports it three ways:

- a red strip across the top of the screen for five seconds, which needs
  nothing installed,
- a `notify-send` notification naming the line and what was wrong with it, when
  a notification daemon is running,
- a line in `~/.local/state/gluewc.log`, always:

```text
config:24: bad bind 'mod+NoSuchKey = spawn:true'
config:31: unknown key 'nonsens'
```

The same report appears at login when the config was already broken.

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
| `start_in_overview` | boolean | open the session on the overview (default false) |

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
autostart = swaybg -i ~/Pictures/wallpaper.jpg -m fill
```

Wallpaper programs should use a background or bottom layer-shell surface.
gluewc copies those layers into the overview, and a wallpaper that changes
while the overview is up changes in the cards too. If none is running,
`root_color` is shown. glueqs paints the wallpaper itself, so with it there
is no wallpaper line at all.

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
| `macro:stop_all` | stop every macro and release synthetic keys/buttons |
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
| `wm:move_left/right/up/down` | move the window that way: trade places with the neighbour in that direction, and when there is none left go on to the workspace that lies that way (up/down in the scroll layout, left/right elsewhere) |
| `wm:move_to_workspace_left/right/up/down` | only the second half of that: take the window along to the workspace that way, on the axis the layout puts workspaces on |
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

## Macros

Macros are scheduled inside the compositor. They create no helper process and
have no active timer while stopped. A definition contains its own global
trigger:

```ini
# Left click 20 times per second while Shift+R is held.
macro = rapid_click trigger=shift+r type=click mode=hold button=left cps=20 press=10

# Type Q, P, O and Space repeatedly; the second press of Super+F9 stops it.
macro = qpo trigger=mod+F9 type=sequence mode=toggle interval=80 press=10 sequence=q,p,o,space

# Explicit waits and pointer clicks can be mixed into a sequence.
macro = mixed trigger=mod+F10 type=sequence mode=once interval=0 press=10 sequence=q,wait:120,click:left,wait:80,Return

# Optional emergency shortcut.
bind_insert = mod+shift+Escape = macro:stop_all
```

`type` is `click` or `sequence`. `mode` is `hold`, `toggle` or `once`.
Click macros accept `button=left|right|middle` and `cps=1..200`. Sequence
items are XKB key combinations, `wait:MILLISECONDS`, or
`click:left|right|middle`; `interval` is the extra gap after each key or click.
`press` controls how long a generated key or button stays down. Macro triggers
take priority over ordinary compositor binds using the same combination.

All macros stop when the session locks or the configuration reloads. gluewc
also releases any generated key or mouse button that was down at that moment.

## Media, volume and backlight

The function row carries the media keys, so it works on keyboards that have
none or that hide them behind `Fn`. `XF86Audio*` and `XF86MonBrightness*` keep
working as they always did, and normal mode has the same keys without `Super`.

| Binding | Action |
| --- | --- |
| `Super+F1` | play / pause |
| `Super+F2`, `Super+F3` | previous, next |
| `Super+Shift+F2`, `Super+Shift+F3` | seek 10 seconds back, forward |
| `Super+F4`, `Super+Shift+F4` | mute output, mute microphone |
| `Super+F5`, `Super+F6` | volume down, up |
| `Super+F7`, `Super+F8` | backlight down, up |

The commands pick the helper that is installed rather than insisting on one:
`wpctl` (WirePlumber, present on any PipeWire system) before `pactl`. The
backlight keys run `gluewc-backlight up|down`, a small script installed next
to the compositor that wraps `brightnessctl` (or `light`) and walks the ladder
0, 1, 2 ... 10, 15, 20 ... 100: 5% steps where the difference is barely
visible, 1% steps in the dark, and 0 turns the panel off. Playback uses
`playerctl`. They are ordinary `spawn:` binds, so replacing one is a single
line:

```ini
bind_insert = mod+F6 = spawn:pamixer -i 5
```

Volume up is capped at 100% with `wpctl -l 1.0`; drop the flag if you want the
software boost above it.

## Overview and gestures

- Tap and release `Super` without another key to toggle the overview.
  `start_in_overview` opens the session on it.
- Arrow keys and workspace numbers navigate while it is open. Every binding
  with a modifier keeps working: a terminal opens, a window closes, the layout
  cycles, and the cards follow. Bindings that would switch workspace
  underneath it (`wm:workspace:N`, `wm:workspace_prev/next`, the arrows) move
  the overview instead.
- The cards are live: the windows keep painting and every commit redraws the
  card it belongs to, so a video plays inside its card.
- Nothing crosses a monitor edge: cards, windows sliding between workspaces,
  the strip's off-screen columns and the drift canvas are all cut at the
  monitor they belong to.
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

## Monitors

Every output can be set up from the config file, and a saved change is applied
on the spot like everything else. One line per monitor:

```ini
output = eDP-1 scale=1.5 pos=0,0
output = DP-1 mode=2560x1440@144 pos=1280,0
output = HDMI-A-1 mirror=eDP-1
output = DP-2 enabled=false
```

The name is the one the monitor has on the wire (`eDP-1`, `DP-1`, `HDMI-A-1`);
`gluewc-msg outputs` lists them. `*` stands for every output that has no line
of its own, and a later line for the same name overrides the earlier one key
by key.

| Key | Values | Default |
| --- | --- | --- |
| `mode` | `WxH`, `WxH@Hz`, `preferred` | the monitor's preferred mode |
| `pos` | `X,Y` or `auto` | `auto`: to the right of the others |
| `scale` | `0.1` to `10`, fractions allowed | `1` |
| `transform` | `normal`, `90`, `180`, `270`, `flipped`, `flipped-90`, `flipped-180`, `flipped-270` | `normal` |
| `enabled` | `true`, `false` | `true` |
| `mirror` | another output's name, or `none` | `none` |
| `adaptive_sync` | `true`, `false` | `false` |

A `mode` no fixed mode matches is tried as a custom mode; a monitor that
cannot do it keeps its current one and the log says so. A key that is missing
from the line means the default, so removing `scale=2` puts the scale back to 1
on the next save. Changes made from outside with `wlr-randr` or `kanshi` work
as before and are not written back, which means the next save of the config
resets them to what the file says.

### Mirroring

`output = HDMI-A-1 mirror=eDP-1` makes the second monitor show everything on
the first: the whole logical area, scaled to fit and centred, with black bars
where the shapes differ. `output = * mirror=eDP-1` does it for every other
monitor at once, which is the "same picture everywhere" setting; the source
itself is exempt. A mirror is not a place of its own: it has no workspaces,
never holds a window or the focus, its bar and wallpaper are closed while it
mirrors (the source's are what it shows), and the cursor is visible on both.
When the source is unplugged or turned off the mirror becomes a normal monitor
again, and comes back to mirroring when the source returns. A mirror cannot be
a source, and a monitor cannot mirror itself; both are reported as config
errors. With `pos=auto` the monitors may shift while a mirror engages; give the
source a fixed `pos` if the numbers matter.

### The outputs state file

Whenever the layout changes gluewc rewrites `$XDG_STATE_HOME/gluewc/outputs`
(`~/.local/state/gluewc/outputs`), one tab-separated line per output, on or
off:

```text
name=DP-1	enabled=1	x=0	y=0	w=1920	h=1080	pw=1920	ph=1080	hz=60.00	scale=1.00	transform=normal	mirror=none	focused=1	make=Dell Inc.	model=U2720Q	serial=none	preferred=3840x2160@60.00	modes=3840x2160@60.00,1920x1080@60.00
```

`w`/`h` are the logical size after scale and transform, `pw`/`ph` the pixels of
the current mode, `modes` every fixed mode the monitor offers, `mirror` the
source it is copying right now. `gluewc-msg outputs` prints the file. The
glueqs settings panel has a Monitors page built on it: it draws the layout,
lets you drag screens around, pick modes and scales, mirror one or all, and
writes the `output` lines above.

## Bars and shells

Layer-shell panels reserve their exclusive area automatically, including during
overview transitions. gluewc advertises `dwl-ipc-unstable-v2` for workspace
modules and the foreign-toplevel protocol for window lists. A Waybar config can
therefore use the `dwl/tags` module even though gluewc presents them as fixed
workspaces.
