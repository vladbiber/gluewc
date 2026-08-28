# Changelog

## Unreleased

### Added

- `Super+Ctrl+Arrow` takes the focused window along to the workspace in that
  direction, on the same axis `Super+Arrow` steps along when there is nothing
  left to focus: left and right in the BSP and drift layouts, up and down in
  the scroll layout, which stacks workspaces vertically the way niri does. A
  press across that axis does nothing. New actions
  `wm:move_to_workspace_left/right/up/down`
- `Super+Ctrl+wheel` takes the window along to the workspace the wheel lands
  on, the mouse counterpart of `Super+Ctrl+Arrow`. Plain `Super+wheel` still
  only changes the view
- Screenshots without reaching for a script: `Print` saves the screen and
  `Shift+Print` saves an area drawn with the mouse, both into
  `$XDG_PICTURES_DIR/Screenshots`; `Ctrl+Print` and `Super+Shift+S` put the
  same two onto the clipboard. Cancelling the selection with Escape takes no
  picture
- The installer brings in what the default bindings call: `grim` and `slurp`
  for screenshots, `wl-clipboard`, `playerctl` for the media keys and
  `brightnessctl` (`dev-libs/light` on Gentoo, which is the fallback the
  bindings already had) for the backlight. Those keys used to be bound to
  helpers nothing installed


- finix is recognised as its own family. It reports `ID=nixos` so that the
  NixOS tooling it reuses keeps working, so it is matched on the name in
  `/etc/os-release` ahead of the nixos branch, and it gets its own flake
  instructions pointing at `/etc/finix` and `finix-rebuild`
- The NixOS module runs on finix. finix has no systemd and spells several
  options differently — `programs.pipewire`, `services.rtkit` and
  `services.polkit` against NixOS's `services.pipewire`, `security.rtkit` and
  `security.polkit` — and has no display-manager session registry at all. The
  module now sets only the options the running system declares, so one import
  works on both
- The installer knows many more derivatives by name: Parabola, BlackArch,
  SteamOS, RebornOS and Obarun; MX, antiX, Deepin, Trisquel, KDE neon, PureOS,
  Parrot, SparkyLinux, Peppermint and TUXEDO OS; Ultramarine, Bazzite, Bluefin
  and Oracle Linux; SLED, SLES and GeckoLinux; Funtoo, Calculate, Redcore and
  Pentoo


- Media, volume and backlight on the function row: Mod-F1 plays and pauses,
  Mod-F2 and Mod-F3 step through tracks, Mod-Shift-F2 and Mod-Shift-F3 seek,
  Mod-F4 to Mod-F6 mute and change the volume, Mod-Shift-F4 mutes the
  microphone, and Mod-F7 and Mod-F8 drive the backlight — so a keyboard
  without media keys, or with them behind Fn, can still reach all of it.
  Normal mode has the same keys without Mod
- The volume and backlight commands pick the helper that is installed instead
  of insisting on one: `wpctl`, which ships with WirePlumber, before `pactl`,
  and `brightnessctl` before `light`
- Saving the config applies it: `~/.config/gluewc/config.conf` is watched and
  reloaded on the spot, including when an editor writes a temporary file and
  renames it over the original
- A line the config parser rejects is reported rather than only logged: a red
  strip across the top of the screen, a `notify-send` notification naming the
  line, and a log entry with its number. Everything that did parse still
  applies, so a typo cannot leave the session without keybinds
- NixOS support through a flake: a package, an overlay, a `nix develop` shell
  and a `programs.gluewc` NixOS module that registers the session with the
  display manager and sets up PipeWire, the portals and Xwayland
- The installer sets up sound as well: PipeWire, WirePlumber and the ALSA and
  PulseAudio bridges are installed with the build dependencies and their user
  units are enabled on systemd distributions, with `--no-audio` to opt out
- The installer recognises more distributions by name — Artix, CachyOS, Garuda,
  ArcoLinux, Devuan, Mint, Pop!_OS, elementary, Zorin, Kali, Raspberry Pi OS,
  Nobara, Rocky, AlmaLinux, postmarketOS — and an unlisted distribution now
  falls back to whichever package manager is on PATH instead of being rejected
- A detailed git-and-make build walkthrough in the installation guide, covering
  `PREFIX`, `DESTDIR`, `config.h`, private wlroots builds and staying up to date

### Changed

- The drift camera pan moved from `Super+Ctrl+Arrow` to `Super+Alt+Arrow` to
  make room for the workspace move. In the BSP and scroll layouts that
  binding still swaps windows, which `Super+Shift+Arrow` already did
- gluewc now builds against wlroots 0.20 and SceneFX 0.5. The two move
  together: SceneFX 0.5 requires wlroots 0.20, and neither is compatible with
  the 0.19/0.4 pair gluewc used before
- Rounded corners now go through the SceneFX 0.5 corner API: the corner
  location enum was replaced by a four-value `fx_corner_radii`, so each corner
  carries its own radius. The rendered result is unchanged
- The backdrop blur behind a window is now a scene node of its own instead of a
  flag on the surface buffer, masked by that buffer so it stays inside the
  parts the client actually paints — the same effect the old
  `ignore_transparent` flag gave. Window clones in the overview and in the
  close animation get a matching blur node, so previews keep their effects
- The installer follows: it installs `wlroots0.20` where a distribution has it
  (Arch, Alpine, Void) and builds wlroots 0.20.2 from source everywhere else.
  No distribution packages SceneFX 0.5 yet, so that one is always built from
  source


- Mod-F1 to Mod-F3 replace the bare F1 to F3 media binds in insert mode, which
  gives applications their function keys back
- `gluewc-session` looks for its default config next to its own prefix, and
  honours `GLUEWC_DATADIR`, so a custom `--prefix` and a Nix store path seed
  `~/.config/gluewc/config.conf` like a distribution install does
- BSP direct manipulation: Mod-drag no longer tears a tiled window out into a
  float, it hands the window the tile it is dropped on and the two trade
  places in the tree; Mod-right-drag moves the splits the window sits between
  instead of floating it, grabbing the edges nearest the click so a corner
  drag resizes in both directions at once. Windows that are already floating
  are still moved and resized as floats and stay floating.

### Fixed

- `make` finds the wlroots and SceneFX the installer builds. They land under
  `PREFIX`, which is not on pkg-config's default search path on most
  distributions, so a plain `make` — and `sudo make install`, which drops the
  environment — failed with "Package 'wlroots-0.20' was not found" until
  `PKG_CONFIG_PATH` was set by hand. `config.mk` now adds
  `$PREFIX/lib/pkgconfig` and `$PREFIX/lib64/pkgconfig` itself, behind
  whatever `PKG_CONFIG_PATH` already holds

- The Alpine package list asked for `ninja` and `seatd-dev`, neither of which
  exists there; the names are `ninja-build` and `libseat-dev`
- The Gentoo package list asked for `gui-libs/scenefx`, which is not in
  `::gentoo` but in an overlay, and left out `media-libs/libdisplay-info`,
  `dev-libs/libliftoff` and `sys-apps/hwdata`
- An unknown distribution with no recognised package manager no longer stops
  the installer: it warns, builds the libraries from source and lets the
  version checks name whatever is still missing


- Clicks landed in the wrong place in the drift layout at any zoom other than
  1:1, and the error grew with the zoom until parts of a window stopped
  responding at all. wlroots hit-tests a scaled buffer in destination pixels
  and hands the result to the client as surface coordinates, which only agree
  while nothing is scaled; the camera zoom is now divided back out before the
  point is compared against the input region and passed on
- A window that left the canvas kept its zoomed buffers, and with them the
  wrong hit test, until it next committed
- The drift clip was a border width too large, so a window's own content was
  drawn over its right and bottom border
- A drift window whose client never acknowledged a resize held the canvas at a
  size the client was not painting at for as long as it lived; the pending
  resize now expires like the frame it holds up
- A drift window that takes its real size after mapping, which is the first
  thing a browser does, is resized around its middle, so it keeps the place it
  was given instead of walking out of it

## 0.2.0 — 2026-08-15

### Added

- Window open and close animations that scale the frame: a window grows out
  of its own centre when it appears and collapses back into it when it goes,
  with the whole frame — surface, border and rounded corners — scaled per
  frame instead of only sliding into place
- `animation_type_open` and `animation_type_close` (`zoom`, `slide`, `fade`,
  `none`), separate `animation_duration_open` and `animation_duration_close`,
  `zoom_initial_ratio` and `zoom_end_ratio`, and CSS-style cubic-bezier easing
  through `animation_curve_open` and `animation_curve_close`
- Niri-style scrolling layout as a second per-monitor layout: windows form
  columns on an endless horizontal strip that scrolls to follow focus, and new
  windows open in their own column beside the focused one
- Column consume/expel with Mod-Comma / Mod-Period (`wm:consume`, `wm:expel`)
- Column width resize and maximize through the existing ratio/split actions
- Vertical workspace slide and a vertical, wallpaper-backed overview while the
  scrolling layout is active
- Scroll layout direct manipulation: Mod-drag reorders the strip in place,
  Mod-right-drag resizes the column; focus moves exit fullscreen like niri
- niri-style gestures in the scroll layout: three-finger vertical swipes change
  workspace, horizontal swipes walk the windows; overview arrows pan the strip
  on the x axis while Up/Down change workspace
- Focus ring on the focused window inside the overview (hidden when window
  decorations are toggled off)
- Scroll layout Mod-F is maximize-column instead of fullscreen: the column
  grows to the full screen width and a second press restores the previous
  width; real fullscreen remains on Mod-Shift-F
- driftwm-style drift layout as a third per-monitor layout; Mod-N now cycles
  BSP, the scroll layout and drift. Windows keep their native size on an
  infinite canvas, each workspace has its own camera and new windows are
  placed in free space and panned into view
- Drift camera zoom that scales the live surfaces instead of resizing the
  clients: Mod-plus/minus/0, Mod+scroll at the pointer, a two- or three-finger
  pinch, and Mod-W to fit every window of the workspace on screen
- Drift camera panning with Mod-Ctrl-arrow, three-finger swipes and scrolling
  over bare canvas
- Drift direct manipulation: Mod-drag or Alt-drag moves a window with edge
  snapping and auto-pan at the viewport edges, Alt-Shift-drag moves the whole
  snapped cluster, Mod-right-drag resizes, Mod-Shift-arrow nudges
- Mod-Shift-drag pans the drift camera by dragging the canvas itself, so a
  plain mouse can move around a canvas that windows cover; the wheel only
  pans over bare canvas and the three-finger swipe needs a touchpad
- The active layout is remembered between sessions in
  `$XDG_STATE_HOME/gluewc/layout` (`remember_layout` turns it off)
- Four-finger gestures for workspaces and the overview in every layout, plus a
  four-finger pinch for the overview
- `layout`, `drift_snap`, `drift_nudge`, `drift_zoom_min`, `drift_zoom_max`,
  `drift_zoom_step` and `drift_pan_speed` runtime settings, and the
  `wm:layout:*`, `wm:pan_*`, `wm:zoom_*` actions
- The dwl IPC layout name now reports the active layout, and bars can select
  one through `set_layout`

### Changed

- The layout cycle moved from Mod-M to Mod-N, and Mod-M now quits the session
  alongside Mod-Shift-Q
- Mod-wheel changes workspace in every layout, including drift; the drift
  camera zoom moved to Mod-Shift-wheel
- Two-finger scrolling no longer changes workspace in the overview; the
  overview is walked with three fingers, the wheel or the keyboard
- The overview focus ring is drawn only in the scroll layout

### Fixed

- A closing overview that never reached its last frame left the screen without
  a single window on it: the tiling layers stay switched off until the
  transition ends, and only then does anything turn them back on, so the
  session looked empty, nothing under the pointer could be focused and every
  key was swallowed. The close now has a deadline and completes by hand if the
  frames stop coming
- A client that never acknowledges a resize no longer freezes its output
  indefinitely; after 300 ms it is drawn at whatever size it has, instead of
  holding back every frame and leaving an animation stuck halfway
- A workspace hide animation that finished after the workspace came back hid a
  window that belonged to the current workspace, leaving it invisible and
  unfocusable; animations left over from an earlier switch are also settled
  before a new one starts
- Moving the pointer now hands the keyboard to the window under it whenever
  nothing holds keyboard focus at all: focus dropped while the cursor stayed
  on a window used to be unrecoverable without moving the pointer off it and
  back, because the window was already the pointer focus
- Three-finger gestures now also work when the touchpad reports them as a
  pinch, instead of doing nothing or zooming the drift camera by accident
- The overview no longer draws clones at the wrong scale while the drift
  camera is zoomed: the zoom is kept applied on the live buffers the overview
  clones from, so the focus ring stopped showing as a band beside or inside
  the selected window
- Moving a drift window to another workspace or monitor while the camera was
  zoomed shrank it by the zoom factor, because the canvas size was taken from
  the on-screen box

## 0.1.0 — 2026-08-15

Initial gluewc release.

### Added

- Automatic BSP tiling with nine workspaces per output
- Directional focus, swapping and modal keyboard control
- Runtime appearance, XKB, autostart and keybind configuration
- In-place config reload
- GNOME-style animated overview with neighboring workspaces
- Mouse overview navigation and window drag between workspaces
- Two- and three-finger touchpad gestures
- Animated window open, close, retile and workspace transitions
- SceneFX rounded corners, blur and optional transparency
- Layer-shell launcher animation, including Rofi
- XWayland, session-lock and output power-management support
- dwl IPC and foreign-toplevel integration for bars and shells
- Display-manager session wrapper and multi-distribution installer

### Based on

gluewc started from dwl 0.8. The repository retains the upstream Git history
and the complete license notices.
