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
| `animation_duration` | milliseconds | base animation duration |
| `warp_pointer` | boolean | move the pointer to keyboard-focused windows |

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
| `wm:toggle_split` | change the focused BSP split direction |
| `wm:ratio:VALUE` | adjust the focused split ratio |
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
- A three-finger horizontal desktop swipe changes workspace.
- A three-finger upward swipe opens the overview; downward closes it.
- Holding `Super` and scrolling changes workspace from the desktop.
- Click a window card to focus it; drag it to a neighboring card to move it.

Touchpad gesture availability depends on libinput and the hardware.

## Bars and shells

Layer-shell panels reserve their exclusive area automatically, including during
overview transitions. gluewc advertises `dwl-ipc-unstable-v2` for workspace
modules and the foreign-toplevel protocol for window lists. A Waybar config can
therefore use the `dwl/tags` module even though gluewc presents them as fixed
workspaces.
