# gluewc

[![build](https://github.com/vladbiber/gluewc/actions/workflows/build.yml/badge.svg)](https://github.com/vladbiber/gluewc/actions/workflows/build.yml)
[![license: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![wlroots 0.19](https://img.shields.io/badge/wlroots-0.19-7aa2f7.svg)](https://gitlab.freedesktop.org/wlroots/wlroots/-/releases/0.19.3)

gluewc is a compact, animated Wayland compositor built around automatic BSP
tiling. It combines a dwl-sized C codebase with workspaces, a GNOME-style
overview, touchpad gestures, runtime configuration and SceneFX visuals.

It is a fork of [dwl 0.8](https://codeberg.org/dwl/dwl), using
[wlroots 0.19](https://gitlab.freedesktop.org/wlroots/wlroots) and
[SceneFX 0.4](https://github.com/wlrfx/scenefx/releases/tag/0.4.1).

<video src="https://raw.githubusercontent.com/vladbiber/gluewc/main/docs/media/glue-demo.mp4" controls muted loop playsinline width="720">
  <a href="https://raw.githubusercontent.com/vladbiber/gluewc/main/docs/media/glue-demo.mp4">Watch the demo</a>
</video>

The clip runs through the layouts, the overview, the gestures and the shell in
[the video above](docs/media/glue-demo.mp4).

## Highlights

- Automatic BSP tiling with per-split direction and ratio control
- Nine independent workspaces on every monitor
- Animated window open, close, retile and workspace transitions, with
  configurable type, duration and cubic-bezier easing
- GNOME-style overview with neighboring workspaces and live wallpaper layers
- Niri-style scrolling layout per monitor (Mod-N): columns on an endless strip,
  vertical workspace slides and a vertical overview over the wallpaper
- driftwm-style drift layout (Mod-N again): windows keep their native size on
  an infinite canvas with edge snapping, per-workspace camera pan and zoom,
  and trackpad gestures for both
- Drag windows between workspaces directly from the overview
- Three-finger workspace and overview gestures
- Directional keyboard focus; crossing the left or right edge changes workspace
- Insert and normal keyboard modes inspired by modal window managers
- Runtime config and keybind reload without recompiling, applied as soon as the
  file is saved and reported on screen when a line does not parse
- Rounded corners, blur and optional transparency through SceneFX
- XWayland, layer-shell, session lock and output power-management support
- dwl IPC and foreign-toplevel support for bars and desktop shells
- Black fallback background when no wallpaper program is running

## Install

The installer detects the distribution, installs build dependencies and audio,
builds a compatible SceneFX/wlroots pair when needed, and installs the session
entry:

```sh
curl -fsSL https://raw.githubusercontent.com/vladbiber/gluewc/main/install.sh | sh
```

It requests administrator privileges for packages and the system install. To
inspect it before running:

```sh
git clone https://github.com/vladbiber/gluewc.git
cd gluewc
less install.sh
./install.sh
```

The dependency resolver covers current releases of:

| Family | Target |
| --- | --- |
| Arch | Arch Linux, EndeavourOS, Manjaro, CachyOS, Garuda, Artix, ArcoLinux |
| Debian | Debian 13+, Devuan |
| Ubuntu | Ubuntu 25.10+, Mint, Pop!\_OS, elementary, Zorin, Kali, Raspberry Pi OS |
| Fedora | Fedora 43+, Nobara, RHEL, CentOS Stream, Rocky, AlmaLinux |
| SUSE | openSUSE Tumbleweed and Leap |
| Gentoo | Gentoo Linux |
| Alpine | Alpine Edge, postmarketOS |
| Void | Void Linux rolling |
| NixOS | through the flake in this repository |

A distribution outside the list still works: the installer falls back to
whichever package manager it finds on `PATH` and uses that family's package
set. Gentoo is hardware-tested and the Arch path is exercised by CI. The
remaining resolvers target their current package sets. Older distributions may
not have the Wayland, libdrm and Pixman versions required by SceneFX 0.4; see
[the installation guide](docs/INSTALL.md) for manual and troubleshooting
instructions.

Sound is set up along the way: PipeWire, WirePlumber and the ALSA and
PulseAudio bridges are installed and, where there are systemd user units,
enabled. Pass `--no-audio` to keep your own audio stack.

### NixOS

```sh
nix run github:vladbiber/gluewc          # try it without installing
```

Permanently, add the flake as an input and enable the module, which registers
the session and sets up PipeWire, the portals and Xwayland:

```nix
imports = [ inputs.gluewc.nixosModules.default ];
programs.gluewc.enable = true;
```

### From source

```sh
git clone https://github.com/vladbiber/gluewc.git
cd gluewc
./install.sh --deps-only     # or install the dependencies yourself
make
sudo make install
```

[The installation guide](docs/INSTALL.md) covers `PREFIX`, `DESTDIR`,
`config.h`, building against a private wlroots and staying up to date.

After installation, log out and select **gluewc** in the display manager. From
a TTY, run:

```sh
gluewc-session
```

The default bindings expect `alacritty`, `rofi`, `grim`, `playerctl`, and for
volume and backlight either `wpctl`/`pactl` and `brightnessctl`/`light`. They
are optional and every command can be replaced in the runtime config.

## First steps

The session wrapper creates `~/.config/gluewc/config.conf` on first login.
Saving it applies it: the file is watched and reloaded on the spot, and
anything the parser rejects raises a red strip across the top of the screen
and a notification naming the line. `Super+Shift+R` still reloads by hand.

| Binding | Action |
| --- | --- |
| `Super` tap | Toggle overview |
| `Super+Return` or `Super+Q` | Open terminal |
| `Super+Space` | Open Rofi |
| `Super+1` … `Super+9` | Change workspace |
| `Super+Ctrl+1` … `Super+Ctrl+9` | Move window and follow |
| `Super+Arrow` or `Super+H/J/K/L` | Directional focus |
| `Super+Shift+Arrow` | Swap windows (nudge them in the drift layout) |
| `Super+N` | Cycle BSP, scroll and drift layouts |
| `Super+Ctrl+Arrow` | Pan the drift canvas |
| `Super+drag` | Swap the window with the tile it is dropped on |
| `Super+right-drag` | Resize the window against its neighbours |
| `Super+Shift+drag` | Drag the drift canvas around with the mouse |
| `Super+W`, `Super+±`, `Super+0` | Drift: fit, zoom, 1:1 |
| `Super+wheel` / `Super+Shift+wheel` | Change workspace / zoom the drift camera |
| `Super+F` | Workspace-area fullscreen |
| `Super+Shift+F` | Real fullscreen |
| `Super+V` | Toggle centered floating |
| `Super+C` | Close window |
| `Super+O` | Toggle configured transparency |
| `Super+Escape` | Enter normal mode; `I` returns to insert mode |
| `Super+Shift+R` | Reload config in place (it also reloads itself when saved) |
| `Super+M` or `Super+Shift+Q` | Quit gluewc |

Media, volume and backlight sit on the function row, so they work on keyboards
without media keys or with them behind `Fn`. The media keys themselves keep
doing the same thing, and in normal mode the same keys need no `Super`.

| Binding | Action |
| --- | --- |
| `Super+F1` | Play / pause |
| `Super+F2` / `Super+F3` | Previous / next track |
| `Super+Shift+F2` / `Super+Shift+F3` | Seek 10s back / forward |
| `Super+F4` / `Super+Shift+F4` | Mute output / microphone |
| `Super+F5` / `Super+F6` | Volume down / up |
| `Super+F7` / `Super+F8` | Backlight down / up |

Playback needs `playerctl`. Volume goes through `wpctl` (part of WirePlumber,
so it is there on any PipeWire system) and falls back to `pactl`; the backlight
uses `brightnessctl` and falls back to `light`. Every one of these is a plain
`spawn:` line in the config, so they can be pointed at anything else.

In the overview, use arrows, workspace numbers, the mouse wheel or a two-finger
horizontal swipe to navigate. Click a window to focus it, or drag it onto the
left, center or right workspace. A three-finger horizontal swipe changes the
workspace from the desktop; a three-finger vertical swipe opens or closes the
overview. `Super+wheel` also changes workspace. In the drift layout three
fingers pan the canvas and a pinch zooms it, so four fingers take over
workspaces and the overview there.

## Desktop components

gluewc deliberately does not bundle a bar, launcher or wallpaper daemon.
Layer-shell clients reserve their own space, and wallpaper layers are included
in the overview.

Examples for `~/.config/gluewc/config.conf`:

```ini
autostart = swww-daemon
autostart = waypaper --restore
autostart = waybar
```

Use either wallpaper command, not both. With neither, `root_color = 000000`
keeps the background black. Waybar can use its `dwl` workspaces module; desktop
shells can also consume the foreign-toplevel protocol.

### glueqs

[glueqs](https://github.com/vladbiber/glueqs) is a Quickshell desktop shell
written alongside gluewc: a dot-matrix bar with workspaces off the dwl IPC,
tray, media, network, weather and notifications, plus OSDs, a launcher and a
dash that appears over the overview. It is the shell in the video above.

It is entirely optional — gluewc runs with waybar, any other layer-shell bar,
or none at all — but it is the combination the compositor is developed against:

```ini
autostart = qs -c glueqs
```

## Configuration and building

- [Installation and distro notes](docs/INSTALL.md)
- [Configuration, actions and gestures](docs/CONFIGURATION.md)
- [Default runtime config](config.def.conf)
- [Contributing](CONTRIBUTING.md)

Manual build, once dependencies are installed:

```sh
make
sudo make install
```

Compiled defaults remain in `config.def.h`; normal users only need the runtime
config. Run `man gluewc` for command-line and environment details.

## License and credits

gluewc is GPL-3.0-or-later. It is derived from dwl, which grew from the wlroots
TinyWL example and carries code influenced by dwm and sway. See `LICENSE*` and
the Git history for the complete notices.
