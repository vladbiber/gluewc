# Changelog

## Unreleased

### Changed

- BSP direct manipulation: Mod-drag no longer tears a tiled window out into a
  float, it hands the window the tile it is dropped on and the two trade
  places in the tree; Mod-right-drag moves the splits the window sits between
  instead of floating it, grabbing the edges nearest the click so a corner
  drag resizes in both directions at once. Windows that are already floating
  are still moved and resized as floats and stay floating.

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
