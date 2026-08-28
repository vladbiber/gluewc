# Installation

## Automatic installer

```sh
curl -fsSL https://raw.githubusercontent.com/vladbiber/gluewc/main/install.sh | sh
```

The script uses the native package manager, then checks for the exact pkg-config
modules required by gluewc. Arch, Alpine and Void package wlroots 0.20; on every
other distribution the installer builds wlroots 0.20.2 from source. No
distribution packages SceneFX 0.5 yet, so that one is always built from source
unless `scenefx-0.5` is already installed. Both libraries carry their version in
their name, so they sit next to any other wlroots a system already has.

Useful modes:

```sh
./install.sh --update        # pull, rebuild, install, then report config drift
./install.sh --check-config  # only the drift report, nothing is built
./install.sh --with-bar      # also set up the glueqs quickshell bar
./install.sh --dry-run       # show the package command
./install.sh --no-deps       # use dependencies already installed
./install.sh --no-audio      # skip PipeWire and friends
./install.sh --deps-only     # prepare dependencies without installing gluewc
./install.sh --prefix /opt/gluewc
./install.sh --uninstall
```

### Updating

`--update` pulls the checkout it is run from, hands over to the freshly pulled
script, rebuilds and installs. `--update --no-deps` skips the package manager
and goes straight to the rebuild, which is what you want when nothing but
gluewc itself has changed.

Nothing in `~/.config` is rewritten. Instead, every install and update ends
with a list of the settings and bindings the shipped defaults have that your
own config does not — new keybindings land there rather than silently in a
file you have edited. Settings are matched by name and bindings by their key
combination, so a binding you deliberately pointed elsewhere is not reported as
missing, and `autostart` is left out of the comparison entirely. `--check-config`
prints that same list on its own.

### The bar

`--with-bar` installs [glueqs](https://github.com/vladbiber/glueqs), a
quickshell bar written for gluewc: it adds quickshell where the distribution
packages it, clones the bar into `~/.config/quickshell/glueqs` (or pulls it if
it is already there) and appends `autostart = qs -c glueqs` to your config,
once. quickshell is packaged on Arch, Void, Fedora 44+, Debian 14 and unstable,
Ubuntu 26.10+ and in Gentoo's GURU overlay; where it is missing the bar is
still set up and the script tells you where to get the binary.

The default prefix is `/usr/local`; the display-manager entry is installed in
`/usr/share/wayland-sessions`. Override `SESSIONDIR` when a distribution uses a
different location.

### Which distribution the installer recognises

The family is taken from `ID` and `ID_LIKE` in `/etc/os-release`, so derivatives
follow their parent automatically. Recognised outright:

| Family | Package manager | Recognised names |
| --- | --- | --- |
| arch | `pacman` | Arch, Manjaro, EndeavourOS, CachyOS, Garuda, Artix, ArcoLinux, Parabola, BlackArch, SteamOS, RebornOS, Obarun |
| debian | `apt-get` | Debian 13+, Ubuntu 25.10+, Mint, Pop!\_OS, elementary, Zorin, Devuan, Kali, Raspberry Pi OS, MX, antiX, Deepin, Trisquel, KDE neon, PureOS, Parrot, SparkyLinux, Peppermint, TUXEDO OS |
| fedora | `dnf` | Fedora 43+, Nobara, RHEL, CentOS Stream, Rocky, AlmaLinux, Ultramarine, Bazzite, Bluefin, Oracle Linux |
| suse | `zypper` | openSUSE Tumbleweed and Leap, SLED, SLES, GeckoLinux |
| gentoo | `emerge` | Gentoo, Funtoo, Calculate, Redcore, Pentoo |
| alpine | `apk` | Alpine Edge, postmarketOS |
| void | `xbps-install` | Void |
| nixos | — | NixOS, through the flake (see below) |
| finix | — | finix, through the flake (see below) |

finix reports `ID=nixos` on purpose, so that the NixOS tooling it reuses keeps
working. It is recognised by the name in `/etc/os-release` instead, ahead of the
nixos branch.

A distribution that matches none of these is not rejected: the installer looks
for a package manager on `PATH` and uses the list of the family that owns it,
which is what makes unlisted derivatives work. When there is no known package
manager either, it says so and carries on — the source build below produces
wlroots and SceneFX on any system with a compiler, and the version checks name
whatever is still missing.

Check what would happen without changing anything:

```sh
./install.sh --dry-run
GLUEWC_DISTRO=fedora ./install.sh --dry-run   # pretend to be another family
```

### Audio

Sound is part of a working desktop, so the installer adds PipeWire, WirePlumber
and the ALSA and PulseAudio bridges alongside the build dependencies, and on
systemd distributions enables the user units:

```sh
systemctl --user enable --now pipewire.socket pipewire-pulse.socket wireplumber.service
```

On the distributions without systemd user units — Alpine, Void, Artix, Gentoo
with OpenRC — nothing needs enabling: `gluewc-session` starts `pipewire`,
`wireplumber` and `pipewire-pulse` itself when it finds them and they are not
already running.

Pass `--no-audio` to keep your own audio stack. If you run the installer as
root, enable the user units afterwards from your own account, since the units
belong to the user session and not to root.

## NixOS

NixOS is supported through the flake in this repository rather than through the
installer, which deliberately refuses to touch a NixOS system.

Try it without installing anything. These build the compositor and then run
it, so the screen is taken over until you quit with `Super+Shift+Q`; nothing is
installed and nothing is left behind. Run them from a TTY, not from inside
another Wayland session, unless you want a nested one:

```sh
nix run github:vladbiber/gluewc                        # start the compositor
nix shell github:vladbiber/gluewc -c gluewc-session    # start a full session
nix develop github:vladbiber/gluewc                    # a shell where make works
```

For a permanent installation, add the flake as an input and enable the module.
It installs the package, registers the session with the display manager and sets
up PipeWire, the portals and Xwayland:

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    gluewc.url = "github:vladbiber/gluewc";
  };

  outputs = { nixpkgs, gluewc, ... }: {
    nixosConfigurations.mymachine = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ./configuration.nix
        gluewc.nixosModules.default
        { programs.gluewc.enable = true; }
      ];
    };
  };
}
```

Module options:

| Option | Default | Meaning |
| --- | --- | --- |
| `programs.gluewc.enable` | `false` | install gluewc and register its session |
| `programs.gluewc.package` | this flake's package | swap in your own build |
| `programs.gluewc.audio` | `true` | PipeWire with the ALSA and PulseAudio bridges |

Everything the module turns on uses `mkDefault`, so your own settings win. If
you only want the package and none of the session wiring, skip the module and
use the overlay instead:

```nix
nixpkgs.overlays = [ gluewc.overlays.default ];
environment.systemPackages = [ pkgs.gluewc ];
```

The flake tracks `nixos-unstable` because it needs `wlroots_0_20` and
`scenefx_0_5`; on a stable channel without those attributes, keep the flake's
own nixpkgs input rather than overriding it with `follows`.

### finix

finix has no systemd, and it spells several options differently: it carries
`programs.pipewire`, `services.rtkit` and `services.polkit` where NixOS carries
`services.pipewire`, `security.rtkit` and `security.polkit`, and it has no
display-manager session registry. The module sets only the options the running
system actually declares, so the same import works on both:

```nix
inputs.gluewc.url = "github:vladbiber/gluewc";
imports = [ inputs.gluewc.nixosModules.default ];
programs.gluewc.enable = true;
```

Rebuild with `finix-rebuild switch`. The session entry travels with the package
in `share/wayland-sessions`, which is all a greeter reading `XDG_DATA_DIRS`
needs, and `gluewc-session` starts the audio daemons itself where there are no
user services to enable.

## Required versions

| Dependency | Version |
| --- | --- |
| wlroots | 0.20.x |
| SceneFX | 0.5.x |
| Wayland | 1.24.0+ when building wlroots |
| libdrm | 2.4.129+ when building wlroots |
| Pixman | 0.43.0+ when building wlroots |
| wayland-protocols | 1.47+ (wlroots 0.20 takes several protocol enums from it) |
| xkbcommon | 1.8.0+ |
| libinput | distribution version compatible with wlroots 0.20 |

XCB, xcb-icccm and the Xwayland binary are needed by the default XWayland
build. Meson, Ninja, pkg-config, a C compiler and wayland-protocols are build
requirements.

## Manual dependency examples

Package names change over time; `install.sh --dry-run` shows the command used
for the current system. The central packages are:

- Arch: `wlroots0.20`, `wayland-protocols`, `libinput`, `libxkbcommon`,
  `libxcb`, `xcb-util-wm`, `xcb-util-errors`, `xcb-util-renderutil`,
  `xorg-xwayland`
- Gentoo: neither wlroots 0.20 nor SceneFX 0.5 is in `::gentoo`, so both are
  built from source on top of `dev-libs/wayland-protocols`,
  `media-libs/libdisplay-info`, `dev-libs/libliftoff`, `x11-libs/xcb-util-wm`
  and `x11-base/xwayland`
- Alpine Edge: `wlroots0.20-dev`, `wayland-protocols`, `libinput-dev`,
  `libxkbcommon-dev`, `libxcb-dev`, `xcb-util-wm-dev`, `libseat-dev`,
  `ninja-build`, `xwayland`
- Void: `wlroots0.20-devel` plus the `*-devel` packages for Wayland, libinput,
  xkbcommon, Pixman, libdrm, Mesa, libseat and XCB
- Fedora/openSUSE: their `*-devel` packages for Wayland, libinput, xkbcommon,
  Pixman, libdrm, Mesa, libseat and XCB; wlroots and SceneFX are built here
- Debian/Ubuntu: the matching `lib*-dev` packages; the installer builds the
  versioned wlroots and SceneFX libraries
- NixOS and finix: `wlroots_0_20` and `scenefx_0_5`, wired up by the flake

For audio, add `pipewire` and `wireplumber` plus the bridges your distribution
splits out: `pipewire-pulse` and `pipewire-alsa` on Arch, Debian, Ubuntu and
Alpine; `pipewire-pulseaudio` and `pipewire-alsa` on Fedora and openSUSE;
`alsa-pipewire` on Void; nothing extra on Gentoo.

## Building from source with git and make

The installer is only a wrapper around this. Doing it by hand is four commands
and gives you the source tree to hack on.

**1. Install the dependencies.** Either let the installer do only that step:

```sh
git clone https://github.com/vladbiber/gluewc.git
cd gluewc
./install.sh --deps-only
```

or install them yourself from the lists above and skip to the next step. Verify
that the two libraries gluewc links against are visible. `make` searches
`$PREFIX/lib/pkgconfig` and `$PREFIX/lib64/pkgconfig` on its own, which is
where the installer puts the wlroots and SceneFX it builds; bare `pkg-config`
does not, so name the path when checking by hand:

```sh
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig pkg-config --modversion wlroots-0.20 scenefx-0.5
```

**2. Build.**

```sh
make
```

The first build copies `config.def.h` to `config.h`. That file holds the
compile-time defaults — keybindings, colours, autostart — and is yours to edit;
`make` never overwrites it afterwards. Most settings can also be changed at
runtime in `~/.config/gluewc/config.conf`, so you rarely need to touch it. After
editing it, rebuild with `make` again.

To keep the version string that `gluewc -v` prints, build from a git checkout:
the Makefile asks `git describe` for it and falls back to `_VERSION` in
`config.mk` when there is no repository.

**3. Install.**

```sh
sudo make install
```

The default prefix is `/usr/local`; the session entry goes to
`/usr/share/wayland-sessions/gluewc.desktop`. Both are variables:

```sh
sudo make install PREFIX=/opt/gluewc
sudo make install SESSIONDIR=/usr/local/share/wayland-sessions
make install DESTDIR=/tmp/stage PREFIX=/usr      # staged, for packaging
```

`gluewc-session` finds its default config relative to its own prefix, so a
custom `PREFIX` needs no further setup; `GLUEWC_DATADIR` overrides the lookup if
you move the data directory somewhere else.

**4. Check and clean up.**

```sh
make check      # shell syntax of the scripts, plus the test helpers
make clean      # remove the binary, the objects and the generated protocols
sudo make uninstall
```

`make uninstall` leaves `~/.config/gluewc` alone.

### Building against a wlroots that is not installed

`config.mk` carries commented examples for pointing `WLR_INCS` and `WLR_LIBS` at
a wlroots build tree or a private prefix, including the `-rpath` needed so the
binary does not pick up the system library at runtime. The same trick works for
SceneFX through `SCENEFX_INCS` and `SCENEFX_LIBS`. Alternatively, install both
into a prefix of your own and point pkg-config at it:

```sh
export PKG_CONFIG_PATH=/opt/wlroots/lib/pkgconfig
make LDFLAGS='-Wl,-rpath,/opt/wlroots/lib'
```

### Keeping a source install up to date

```sh
cd gluewc
git pull
make clean && make
sudo make install
```

`config.h` survives `git pull` because it is not tracked. When `config.def.h`
gains new options upstream, diff the two files and copy over what you want.

## Starting a session

From a display manager, select **gluewc**. From a configured TTY:

```sh
gluewc-session
```

The wrapper supplies the Wayland desktop environment, creates a D-Bus session
when needed, starts available PipeWire components, seeds the default config and
writes `~/.local/state/gluewc.log`.

## Troubleshooting

Check dependency discovery. Most distributions do not search under
`/usr/local` by default, so a library the installer built there is invisible to
a bare `pkg-config` even though `make` finds it:

```sh
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig pkg-config --modversion wlroots-0.20 scenefx-0.5
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

If there is no sound, check that the daemons are actually running inside the
session — `pgrep -u "$USER" -x pipewire wireplumber` — and look at the tail of
`~/.local/state/gluewc.log`. On a systemd distribution the units are the usual
suspect:

```sh
systemctl --user status pipewire.socket wireplumber.service
```
