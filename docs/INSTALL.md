# Installation

## Automatic installer

```sh
curl -fsSL https://raw.githubusercontent.com/vladbiber/gluewc/main/install.sh | sh
```

The script uses the native package manager, then checks for the exact pkg-config
modules required by gluewc. When `wlroots-0.19` or `scenefx-0.4` is unavailable,
it builds wlroots 0.19.3 and SceneFX 0.4.1 from their official repositories.
Versioned libraries can coexist with newer wlroots releases.

Useful modes:

```sh
./install.sh --dry-run       # show the package command
./install.sh --no-deps       # use dependencies already installed
./install.sh --deps-only     # prepare dependencies without installing gluewc
./install.sh --prefix /opt/gluewc
./install.sh --uninstall
```

The default prefix is `/usr/local`; the display-manager entry is installed in
`/usr/share/wayland-sessions`. Override `SESSIONDIR` when a distribution uses a
different location.

## Required versions

| Dependency | Version |
| --- | --- |
| wlroots | 0.19.x |
| SceneFX | 0.4.x |
| Wayland | 1.23.1+ when building SceneFX |
| libdrm | 2.4.122+ when building SceneFX |
| Pixman | 0.43.0+ when building SceneFX |
| libinput | distribution version compatible with wlroots 0.19 |
| xkbcommon | distribution version compatible with wlroots 0.19 |

XCB, xcb-icccm and the Xwayland binary are needed by the default XWayland
build. Meson, Ninja, pkg-config, a C compiler and wayland-protocols are build
requirements.

## Manual dependency examples

Package names change over time; `install.sh --dry-run` shows the command used
for the current system. The central packages are:

- Arch: `wlroots0.19`, `wayland-protocols`, `libinput`, `libxkbcommon`,
  `libxcb`, `xcb-util-wm`, `xorg-xwayland`
- Gentoo: `gui-libs/wlroots:0.19`, `gui-libs/scenefx:0.4`,
  `dev-libs/wayland-protocols`, `x11-libs/xcb-util-wm`, `x11-base/xwayland`
- Alpine Edge: `wlroots0.19-dev`, `wayland-protocols`, `libinput-dev`,
  `libxkbcommon-dev`, `libxcb-dev`, `xcb-util-wm-dev`, `xwayland`
- Fedora/openSUSE/Void: their `*-devel` packages for wlroots, Wayland,
  libinput, xkbcommon, Pixman, libdrm, Mesa, libseat and XCB
- Debian/Ubuntu: the matching `lib*-dev` packages; the installer builds the
  versioned wlroots and SceneFX libraries

After dependencies are available:

```sh
git clone https://github.com/vladbiber/gluewc.git
cd gluewc
make
sudo make install
```

## Starting a session

From a display manager, select **gluewc**. From a configured TTY:

```sh
gluewc-session
```

The wrapper supplies the Wayland desktop environment, creates a D-Bus session
when needed, starts available PipeWire components, seeds the default config and
writes `~/.local/state/gluewc.log`.

## Troubleshooting

Check dependency discovery:

```sh
pkg-config --modversion wlroots-0.19 scenefx-0.4
```

Check the installed binary and runtime libraries:

```sh
/usr/local/bin/gluewc -v
ldd /usr/local/bin/gluewc | grep 'not found'
```

`gluewc -v` prints the version and returns a non-zero status by design. For a
verbose compositor log, start `gluewc -d` from a TTY. A session launched through
`gluewc-session` records its normal log at:

```text
~/.local/state/gluewc.log
```

If the session entry is missing, verify that
`/usr/share/wayland-sessions/gluewc.desktop` exists and restart the display
manager. If the compositor starts but default shortcuts launch nothing, install
the optional programs listed in the README or replace their `spawn:` actions.
