#!/bin/sh
set -eu

REPO_URL=${GLUEWC_REPO_URL:-https://github.com/vladbiber/gluewc.git}
REPO_REF=${GLUEWC_REF:-main}
PREFIX=${PREFIX:-/usr/local}
SESSIONDIR=${SESSIONDIR:-/usr/share/wayland-sessions}
DESTDIR=${DESTDIR:-}
WITH_DEPS=1
WITH_AUDIO=1
DEPS_ONLY=0
UPDATE=0
CHECK_CONFIG=0
WITH_BAR=0
BAR_REPO=${GLUEWC_BAR_REPO:-https://github.com/vladbiber/glueqs.git}
WLROOTS_VERSION=0.20.2
SCENEFX_VERSION=0.5
DRY_RUN=0
UNINSTALL=0
WORKDIR=

usage() {
	cat <<EOF
Usage: install.sh [options]

  --prefix PATH    installation prefix (default: /usr/local)
  --no-deps        do not install or build dependencies
  --no-audio       do not install PipeWire, WirePlumber and their ALSA and
                   PulseAudio bridges (they are installed by default so that
                   sound works in the session out of the box)
  --deps-only      install dependencies, then stop
  --update         pull the newest source into this checkout, then build and
                   install it. Your ~/.config/gluewc/config.conf is never
                   touched; what is new in the defaults is listed at the end.
                   Add --no-deps to skip the package manager and go straight
                   to the rebuild
  --check-config   only list what the defaults gained that your config does
                   not have, and stop
  --with-bar       also set up glueqs, the quickshell bar built for gluewc,
                   and start it from the session. Works with --update too
  --dry-run        print the package-manager command without running it
  --uninstall      remove gluewc from the selected prefix
  -h, --help       show this help

Environment: GLUEWC_REPO_URL, GLUEWC_REF, GLUEWC_DISTRO, PREFIX, SESSIONDIR
EOF
}

log() {
	printf '\n\033[1;34m==>\033[0m %s\n' "$*"
}

warn() {
	printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2
}

die() {
	printf '\033[1;31merror:\033[0m %s\n' "$*" >&2
	exit 1
}

cleanup() {
	if [ -n "$WORKDIR" ] && [ -d "$WORKDIR" ]; then
		rm -rf "$WORKDIR"
	fi
}

trap cleanup EXIT HUP INT TERM

run_root() {
	if [ "$(id -u)" -eq 0 ]; then
		"$@"
	elif command -v sudo >/dev/null 2>&1; then
		sudo "$@"
	else
		die "sudo is required for system installation"
	fi
}

run_install() {
	if [ -n "$DESTDIR" ]; then
		"$@"
	else
		run_root "$@"
	fi
}

show_command() {
	printf '  '
	printf '%s ' "$@"
	printf '\n'
}

while [ "$#" -gt 0 ]; do
	case "$1" in
	--prefix)
		[ "$#" -ge 2 ] || die "--prefix needs a path"
		PREFIX=$2
		shift 2
		;;
	--no-deps)
		WITH_DEPS=0
		shift
		;;
	--no-audio)
		WITH_AUDIO=0
		shift
		;;
	--deps-only)
		DEPS_ONLY=1
		shift
		;;
	--update)
		UPDATE=1
		shift
		;;
	--check-config)
		CHECK_CONFIG=1
		shift
		;;
	--with-bar)
		WITH_BAR=1
		shift
		;;
	--dry-run)
		DRY_RUN=1
		shift
		;;
	--uninstall)
		UNINSTALL=1
		WITH_DEPS=0
		shift
		;;
	-h|--help)
		usage
		exit 0
		;;
	*)
		die "unknown option: $1"
		;;
	esac
done

case "$PREFIX" in
/*) ;;
*) die "--prefix must be an absolute path" ;;
esac

USER_CONFIG=${XDG_CONFIG_HOME:-$HOME/.config}/gluewc/config.conf

# Everything the shipped defaults declare and the personal config does not, so
# an update can point at what it gained without editing anyone's file. Settings
# are matched by key and bindings by the key combination, so a binding pointed
# somewhere else on purpose does not show up as missing. autostart is skipped:
# it is personal by nature and the defaults ship it commented out.
config_report() {
	def=$1
	user=$2

	[ -r "$def" ] || return 0
	if [ ! -r "$user" ]; then
		printf '\nNo personal config yet — the session writes one from the\n'
		printf 'defaults the first time you log in.\n'
		return 0
	fi

	report=$(awk -v userfile="$user" '
		function ident(line,   a, count, key, combo) {
			if (line ~ /^[ \t]*#/ || line !~ /=/)
				return ""
			count = split(line, a, "=")
			key = a[1]
			gsub(/^[ \t]+|[ \t]+$/, "", key)
			if (key == "autostart" || key == "")
				return ""
			if (key == "bind_insert" || key == "bind_normal") {
				if (count < 3)
					return ""
				combo = a[2]
				gsub(/^[ \t]+|[ \t]+$/, "", combo)
				return key " " combo
			}
			return key
		}
		BEGIN {
			while ((getline line < userfile) > 0) {
				id = ident(line)
				if (id != "")
					have[id] = 1
			}
		}
		{
			id = ident($0)
			if (id != "" && !(id in have)) {
				sub(/^[ \t]+/, "")
				print "  " $0
			}
		}
	' "$def")

	if [ -z "$report" ]; then
		printf '\nYour config already has every setting and binding the defaults do.\n'
		return 0
	fi
	printf '\n\033[1;34m==>\033[0m New in the defaults, missing from %s:\n\n' "$user"
	printf '%s\n' "$report"
	printf '\nThat file was not touched. Copy across whatever you want — saving it\n'
	printf 'applies the change on the spot, no restart needed.\n'
}

# quickshell is packaged on Arch, Void, Fedora 44+, Debian 14/unstable and
# Ubuntu 26.10+, and lives in the GURU overlay on Gentoo. Older releases and
# Alpine have nothing, so this is deliberately allowed to fail: the bar's
# config is set up either way and the note below says where to get the binary.
install_bar_package() {
	[ "$DRY_RUN" -eq 0 ] || return 0
	case "$FAMILY" in
	arch)   set -- pacman -S --needed --noconfirm quickshell ;;
	debian) set -- apt-get install -y quickshell ;;
	fedora) set -- dnf install -y quickshell ;;
	void)   set -- xbps-install -Sy quickshell ;;
	*)      return 0 ;;
	esac
	run_root "$@" || warn "could not install quickshell from the package manager"
}

install_bar() {
	bardir=${XDG_CONFIG_HOME:-$HOME/.config}/quickshell/glueqs

	log "Setting up the glueqs bar"
	install_bar_package
	if [ -d "$bardir/.git" ]; then
		git -C "$bardir" pull --ff-only || warn "could not update $bardir"
	else
		mkdir -p "$(dirname "$bardir")"
		git clone --quiet "$BAR_REPO" "$bardir" \
			|| die "could not clone the bar from $BAR_REPO"
	fi

	# The session writes the config at first login; seed it now so the
	# autostart line has somewhere to live.
	if [ ! -r "$USER_CONFIG" ] && [ -r "$SOURCE_DIR/config.def.conf" ]; then
		mkdir -p "$(dirname "$USER_CONFIG")"
		cp "$SOURCE_DIR/config.def.conf" "$USER_CONFIG"
	fi
	# Upstream installs both names and every distribution that packages it
	# keeps them, but start whichever one is actually here rather than
	# assuming, and fall back to the short one when nothing is installed yet.
	barcmd=qs
	if ! command -v qs >/dev/null 2>&1 && command -v quickshell >/dev/null 2>&1; then
		barcmd=quickshell
	fi
	if [ -r "$USER_CONFIG" ] && ! grep -qE '^[[:space:]]*autostart[[:space:]]*=[[:space:]]*(qs|quickshell) -c glueqs' "$USER_CONFIG"; then
		printf '\n# the glueqs bar, added by install.sh --with-bar\nautostart = %s -c glueqs\n' "$barcmd" >> "$USER_CONFIG"
		log "Added 'autostart = $barcmd -c glueqs' to $USER_CONFIG"
	fi

	if ! command -v qs >/dev/null 2>&1 && ! command -v quickshell >/dev/null 2>&1; then
		warn "quickshell is not installed and your distribution does not package it"
		warn "get it from https://quickshell.outfoxxed.me — the bar is configured"
		warn "already and starts as soon as the binary is on PATH"
	fi
}

case "$0" in
*/*|install.sh)
	SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || true)
	;;
*)
	SELF_DIR=
	;;
esac
if [ -n "$SELF_DIR" ] && [ ! -f "$SELF_DIR/gluewc.c" ]; then
	SELF_DIR=
fi

if [ "$CHECK_CONFIG" -eq 1 ]; then
	def=$SELF_DIR/config.def.conf
	[ -r "$def" ] || def=$PREFIX/share/gluewc/config.def.conf
	[ -r "$def" ] || die "no config.def.conf to compare against; run from a checkout or install first"
	config_report "$def" "$USER_CONFIG"
	exit 0
fi

# Pull first, then hand over to the script that came with the new source: a
# running sh reads its own file as it goes, so rewriting it underneath is not
# safe. GLUEWC_REEXEC stops that from happening twice.
if [ "$UPDATE" -eq 1 ] && [ -z "${GLUEWC_REEXEC:-}" ] && [ -n "$SELF_DIR" ] \
		&& [ -d "$SELF_DIR/.git" ]; then
	log "Updating the checkout in $SELF_DIR"
	command -v git >/dev/null 2>&1 || die "git is needed to update a checkout"
	git -C "$SELF_DIR" pull --ff-only
	GLUEWC_REEXEC=1
	export GLUEWC_REEXEC
	exec sh "$SELF_DIR/install.sh" "$@"
fi

if [ "$UNINSTALL" -eq 1 ]; then
	log "Removing gluewc"
	run_install rm -f "$DESTDIR$PREFIX/bin/gluewc" "$DESTDIR$PREFIX/bin/gluewc-session" \
		"$DESTDIR$PREFIX/share/man/man1/gluewc.1" \
		"$DESTDIR$PREFIX/share/gluewc/config.def.conf" \
		"$DESTDIR$SESSIONDIR/gluewc.desktop"
	run_install rm -rf "$DESTDIR$PREFIX/share/doc/gluewc"
	printf 'gluewc was removed. Your ~/.config/gluewc directory was kept.\n'
	exit 0
fi

if [ -n "${GLUEWC_DISTRO:-}" ]; then
	ID=$GLUEWC_DISTRO
	ID_LIKE=
elif [ -r /etc/os-release ]; then
	. /etc/os-release
else
	ID=unknown
	ID_LIKE=
fi

DISTRO=" ${ID:-unknown} ${ID_LIKE:-} "

family_by_pm() {
	# Derivatives that carry neither a known ID nor a known ID_LIKE still
	# have a package manager, and that is enough to pick the right list.
	if command -v finix-rebuild >/dev/null 2>&1; then
		printf 'finix'
	elif command -v nixos-rebuild >/dev/null 2>&1; then
		printf 'nixos'
	elif command -v pacman >/dev/null 2>&1; then
		printf 'arch'
	elif command -v apt-get >/dev/null 2>&1; then
		printf 'debian'
	elif command -v dnf >/dev/null 2>&1 || command -v dnf5 >/dev/null 2>&1; then
		printf 'fedora'
	elif command -v zypper >/dev/null 2>&1; then
		printf 'suse'
	elif command -v emerge >/dev/null 2>&1; then
		printf 'gentoo'
	elif command -v xbps-install >/dev/null 2>&1; then
		printf 'void'
	elif command -v apk >/dev/null 2>&1; then
		printf 'alpine'
	else
		printf 'unknown'
	fi
}

detect_family() {
	# finix keeps ID=nixos so that the NixOS tooling it reuses keeps working,
	# so it is recognised by its own name, directory and rebuild command
	# before the nixos branch can claim it.
	case "$DISTRO" in
	*finix*)
		printf 'finix'
		return ;;
	esac
	case " ${NAME:-} ${PRETTY_NAME:-} " in
	*[Ff]inix*)
		printf 'finix'
		return ;;
	esac
	case "$DISTRO" in
	*nixos*)
		printf 'nixos' ;;
	*arch*|*manjaro*|*artix*|*endeavouros*|*cachyos*|*garuda*|*arcolinux*|\
	*parabola*|*blackarch*|*steamos*|*rebornos*|*obarun*)
		printf 'arch' ;;
	*debian*|*ubuntu*|*devuan*|*linuxmint*|*pop*|*elementary*|*zorin*|*kali*|\
	*raspbian*|*mx*|*antix*|*deepin*|*trisquel*|*neon*|*pureos*|*parrot*|\
	*sparky*|*peppermint*|*tuxedo*)
		printf 'debian' ;;
	*fedora*|*rhel*|*centos*|*nobara*|*rocky*|*almalinux*|*ultramarine*|\
	*bazzite*|*bluefin*|*oracle*|*scientific*)
		printf 'fedora' ;;
	*suse*|*opensuse*|*sle[sd]*|*gecko*)
		printf 'suse' ;;
	*gentoo*|*funtoo*|*calculate*|*redcore*|*pentoo*)
		printf 'gentoo' ;;
	*alpine*|*postmarketos*)
		printf 'alpine' ;;
	*void*)
		printf 'void' ;;
	*)
		family_by_pm ;;
	esac
}

FAMILY=$(detect_family)

# Sound is part of a working desktop, so PipeWire, WirePlumber and the ALSA
# and PulseAudio bridges are installed with everything else. gluewc-session
# starts whichever of them it finds. --no-audio skips this.
audio_packages() {
	[ "$WITH_AUDIO" -eq 1 ] || return 0
	case "$1" in
	arch)   printf 'pipewire wireplumber pipewire-pulse pipewire-alsa' ;;
	debian) printf 'pipewire wireplumber pipewire-pulse pipewire-alsa' ;;
	fedora) printf 'pipewire wireplumber pipewire-pulseaudio pipewire-alsa' ;;
	suse)   printf 'pipewire wireplumber pipewire-pulseaudio pipewire-alsa' ;;
	gentoo) printf 'media-video/pipewire media-video/wireplumber' ;;
	alpine) printf 'pipewire wireplumber pipewire-pulse pipewire-alsa' ;;
	void)   printf 'pipewire wireplumber alsa-pipewire' ;;
	esac
}

finix_instructions() {
	cat <<'EOF'

finix builds gluewc from the flake in this repository instead of installing
loose packages, so this script does not touch the system.

To TRY it, without installing anything. These build the compositor and then
start it: the screen is taken over until you quit with Super+Shift+Q, and
nothing is left behind afterwards.

  nix run     github:vladbiber/gluewc                    # the compositor alone
  nix shell   github:vladbiber/gluewc -c gluewc-session  # with D-Bus and audio
  nix develop github:vladbiber/gluewc                    # a shell for make

To INSTALL it, add the flake to /etc/finix and enable the module:

  inputs.gluewc.url = "github:vladbiber/gluewc";
  imports = [ inputs.gluewc.nixosModules.default ];
  programs.gluewc.enable = true;

then rebuild with 'finix-rebuild switch' and pick gluewc in your greeter.
The module notices that finix has
no systemd and wires up the options finix does have (programs.pipewire,
services.rtkit, services.polkit) instead of the NixOS ones; the session
starts the audio daemons itself either way. docs/INSTALL.md has the full
example. Inside a 'nix develop' shell this script still works with --no-deps.
EOF
}

# The default keybindings call out to these: grim and slurp take screenshots
# (slurp draws the crop rectangle), wl-clipboard puts one on the clipboard,
# playerctl drives the media keys and the backlight keys need brightnessctl,
# or light, which is what ::gentoo carries. Without them those keys do
# nothing, so they are installed alongside the compositor.
session_packages() {
	case "$1" in
	arch)   printf 'grim slurp wl-clipboard playerctl brightnessctl' ;;
	debian) printf 'grim slurp wl-clipboard playerctl brightnessctl' ;;
	fedora) printf 'grim slurp wl-clipboard playerctl brightnessctl' ;;
	suse)   printf 'grim slurp wl-clipboard playerctl brightnessctl' ;;
	gentoo) printf 'gui-apps/grim gui-apps/slurp gui-apps/wl-clipboard media-sound/playerctl dev-libs/light' ;;
	alpine) printf 'grim slurp wl-clipboard playerctl brightnessctl' ;;
	void)   printf 'grim slurp wl-clipboard playerctl brightnessctl' ;;
	esac
}

nixos_instructions() {
	cat <<'EOF'

NixOS builds gluewc from the flake in this repository instead of installing
loose packages, so this script does not touch the system.

To TRY it, without installing anything. These build the compositor and then
start it: the screen is taken over until you quit with Super+Shift+Q, and
nothing is left behind afterwards.

  nix run     github:vladbiber/gluewc                    # the compositor alone
  nix shell   github:vladbiber/gluewc -c gluewc-session  # with D-Bus and audio
  nix develop github:vladbiber/gluewc                    # a shell for make

To INSTALL it, add the flake to your configuration and enable the module:

  inputs.gluewc.url = "github:vladbiber/gluewc";
  imports = [ inputs.gluewc.nixosModules.default ];
  programs.gluewc.enable = true;              # session entry + PipeWire audio

docs/INSTALL.md has the full configuration.nix example. Inside a
'nix develop' shell this script still works with --no-deps.
EOF
}

install_packages() {
	case "$FAMILY" in
	finix)
		finix_instructions
		exit 0
		;;
	nixos)
		nixos_instructions
		exit 0
		;;
	arch)
		set -- pacman -Syu --needed --noconfirm base-devel git meson ninja \
			pkgconf wayland wayland-protocols libinput libxkbcommon libxcb \
			xcb-util-wm xcb-util-errors xcb-util-renderutil libdrm mesa \
			pixman seatd libdisplay-info libliftoff hwdata wlroots0.20 \
			xorg-xwayland
		;;
	debian)
		if [ "$DRY_RUN" -eq 0 ]; then
			run_root apt-get update
		fi
		set -- apt-get install -y build-essential git meson ninja-build \
			pkg-config wayland-protocols libwayland-dev libinput-dev \
			libxkbcommon-dev libpixman-1-dev libdrm-dev libgbm-dev \
			libegl1-mesa-dev libgles2-mesa-dev libseat-dev \
			libdisplay-info-dev libliftoff-dev libudev-dev libxcb1-dev \
			libxcb-composite0-dev libxcb-dri3-dev libxcb-ewmh-dev \
			libxcb-icccm4-dev libxcb-present-dev libxcb-render0-dev \
			libxcb-render-util0-dev libxcb-res0-dev libxcb-shm0-dev \
			libxcb-xfixes0-dev libxcb-xinput-dev libxcb-errors-dev \
			hwdata xwayland
		;;
	fedora)
		set -- dnf install -y gcc make git meson ninja-build \
			pkgconf-pkg-config wayland-devel wayland-protocols-devel \
			libinput-devel libxkbcommon-devel pixman-devel libdrm-devel \
			mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel \
			libseat-devel libdisplay-info-devel libliftoff-devel \
			systemd-devel libxcb-devel xcb-util-wm-devel \
			xcb-util-errors-devel xcb-util-renderutil-devel hwdata \
			xorg-x11-server-Xwayland
		;;
	suse)
		set -- zypper --non-interactive install -t pattern devel_basis \
			git meson ninja pkg-config wayland-devel wayland-protocols-devel \
			libinput-devel libxkbcommon-devel pixman-devel libdrm-devel \
			Mesa-libgbm-devel Mesa-libEGL-devel Mesa-libGLESv2-devel \
			libseat-devel libdisplay-info-devel libliftoff-devel \
			libudev-devel libxcb-devel xcb-util-wm-devel \
			xcb-util-errors-devel xcb-util-renderutil-devel xwayland
		;;
	gentoo)
		# wlroots 0.20 and SceneFX 0.5 are not in ::gentoo, so only the
		# build dependencies come from portage and the two libraries are
		# built from source below.
		set -- emerge --noreplace --ask=n dev-vcs/git dev-build/meson \
			dev-build/ninja virtual/pkgconfig dev-libs/wayland \
			dev-libs/wayland-protocols dev-libs/libinput \
			x11-libs/libxkbcommon x11-libs/pixman x11-libs/libdrm \
			media-libs/mesa sys-auth/seatd media-libs/libdisplay-info \
			dev-libs/libliftoff sys-apps/hwdata x11-libs/libxcb \
			x11-libs/xcb-util-wm x11-libs/xcb-util-errors \
			x11-libs/xcb-util-renderutil x11-base/xwayland
		;;
	alpine)
		# alpine calls Ninja ninja-build, and the seat headers live in
		# libseat-dev rather than in the daemon package.
		set -- apk add build-base git meson ninja-build pkgconf wayland-dev \
			wayland-protocols libinput-dev libxkbcommon-dev libxcb-dev \
			xcb-util-wm-dev xcb-util-errors-dev xcb-util-renderutil-dev \
			libdrm-dev mesa-dev pixman-dev seatd libseat-dev \
			libdisplay-info-dev libliftoff-dev hwdata wlroots0.20-dev \
			xwayland
		;;
	void)
		set -- xbps-install -Sy base-devel git meson ninja pkg-config \
			wayland-devel wayland-protocols libinput-devel \
			libxkbcommon-devel pixman-devel libdrm-devel MesaLib-devel \
			libseat-devel libdisplay-info-devel libliftoff-devel \
			eudev-libudev-devel libxcb-devel xcb-util-wm-devel \
			xcb-util-errors-devel xcb-util-renderutil-devel hwids \
			wlroots0.20-devel xorg-server-xwayland
		;;
	*)
		# Anything else still builds: the source fallback below produces
		# wlroots and SceneFX, and the checks after it name whatever is
		# still missing instead of guessing package names for a
		# distribution this script has never seen.
		warn "no package list for '${ID:-unknown}' and no known package manager found"
		warn "install the requirements from docs/INSTALL.md; the build continues"
		warn "and will say which of them are still missing"
		return 0
		;;
	esac

	# shellcheck disable=SC2046  # the package lists are deliberately split
	set -- "$@" $(audio_packages "$FAMILY") $(session_packages "$FAMILY")

	if [ "$DRY_RUN" -eq 1 ]; then
		show_command "$@"
	else
		run_root "$@"
	fi
}

enable_audio() {
	# Only systemd distributions have user units to enable; everywhere else
	# gluewc-session starts the daemons itself when it finds them.
	[ "$WITH_AUDIO" -eq 1 ] || return 0
	command -v systemctl >/dev/null 2>&1 || return 0
	if [ "$(id -u)" -eq 0 ]; then
		warn "running as root: enable the audio units from your own account with"
		warn "  systemctl --user enable --now pipewire.socket pipewire-pulse.socket wireplumber.service"
		return 0
	fi
	log "Enabling PipeWire for $(id -un)"
	systemctl --user daemon-reload >/dev/null 2>&1 || true
	systemctl --user enable --now pipewire.socket pipewire-pulse.socket \
		wireplumber.service >/dev/null 2>&1 \
		|| warn "could not enable the PipeWire user units; gluewc-session starts them at login instead"
}

if [ "$WITH_DEPS" -eq 1 ]; then
	if [ "$FAMILY" = nixos ] || [ "$FAMILY" = finix ]; then
		: # install_packages prints the flake instructions and stops
	elif [ "$WITH_AUDIO" -eq 1 ]; then
		log "Installing build dependencies and audio for ${PRETTY_NAME:-${ID:-Linux}} (family: $FAMILY)"
	else
		log "Installing build dependencies for ${PRETTY_NAME:-${ID:-Linux}} (family: $FAMILY)"
	fi
	install_packages
	if [ "$DRY_RUN" -eq 1 ]; then
		printf '\nDry run complete; no changes were made.\n'
		exit 0
	fi
	enable_audio
fi

for tool in cc make git meson ninja pkg-config; do
	command -v "$tool" >/dev/null 2>&1 || die "missing tool: $tool"
done

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

pc_at_least() {
	pkg-config --atleast-version="$2" "$1" 2>/dev/null
}

# The minimums are the ones wlroots 0.20 and SceneFX 0.5 ask for themselves.
check_source_requirements() {
	pc_at_least wayland-server 1.24.0 || die "Wayland >= 1.24.0 is required to build wlroots 0.20"
	pc_at_least libdrm 2.4.129 || die "libdrm >= 2.4.129 is required to build wlroots 0.20"
	pc_at_least xkbcommon 1.8.0 || die "xkbcommon >= 1.8.0 is required to build wlroots 0.20"
	pc_at_least pixman-1 0.43.0 || die "pixman >= 0.43.0 is required to build wlroots 0.20"
	pc_at_least wayland-protocols 1.47 || die "wayland-protocols >= 1.47 is required to build wlroots 0.20"
}

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/gluewc-install.XXXXXX")

# Arch, Alpine and Void ship wlroots 0.20; everywhere else it is built here.
# No distribution packages SceneFX 0.5 yet, so that one is always built unless
# it is already installed.
if ! pkg-config --exists wlroots-0.20; then
	[ "$WITH_DEPS" -eq 1 ] || die "missing dependency: wlroots-0.20"
	check_source_requirements
	log "Building wlroots $WLROOTS_VERSION"
	git clone --quiet --depth 1 --branch "$WLROOTS_VERSION" \
		https://gitlab.freedesktop.org/wlroots/wlroots.git "$WORKDIR/wlroots"
	meson setup "$WORKDIR/wlroots/build" "$WORKDIR/wlroots" \
		--prefix="$PREFIX" --libdir=lib --buildtype=release \
		-Dexamples=false -Dxwayland=enabled
	meson compile -C "$WORKDIR/wlroots/build"
	run_root meson install -C "$WORKDIR/wlroots/build"
fi

pkg-config --exists wlroots-0.20 || die "wlroots-0.20 was not found after installation"

if ! pkg-config --exists scenefx-0.5; then
	[ "$WITH_DEPS" -eq 1 ] || die "missing dependency: scenefx-0.5"
	check_source_requirements
	log "Building SceneFX $SCENEFX_VERSION"
	git clone --quiet --depth 1 --branch "$SCENEFX_VERSION" \
		https://github.com/wlrfx/scenefx.git "$WORKDIR/scenefx"
	meson setup "$WORKDIR/scenefx/build" "$WORKDIR/scenefx" \
		--prefix="$PREFIX" --libdir=lib --buildtype=release \
		-Dexamples=false -Dwerror=false
	meson compile -C "$WORKDIR/scenefx/build"
	run_root meson install -C "$WORKDIR/scenefx/build"
fi

pkg-config --exists scenefx-0.5 || die "scenefx-0.5 was not found after installation"

if [ -z "$DESTDIR" ] && command -v ldconfig >/dev/null 2>&1; then
	run_root ldconfig
fi

if [ "$DEPS_ONLY" -eq 1 ]; then
	log "Dependencies are ready"
	exit 0
fi

if [ -n "$SELF_DIR" ] && [ -f "$SELF_DIR/Makefile" ]; then
	SOURCE_DIR=$SELF_DIR
else
	log "Downloading gluewc ${REPO_REF}"
	git clone --quiet --depth 1 --branch "$REPO_REF" "$REPO_URL" "$WORKDIR/gluewc"
	SOURCE_DIR=$WORKDIR/gluewc
fi

JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')
RPATH_FLAGS="-Wl,-rpath,$PREFIX/lib -Wl,-rpath,$PREFIX/lib64"

log "Building gluewc"
make -C "$SOURCE_DIR" clean
make -C "$SOURCE_DIR" -j"$JOBS" PREFIX="$PREFIX" SESSIONDIR="$SESSIONDIR" \
	LDFLAGS="$RPATH_FLAGS"

log "Installing gluewc to $PREFIX"
run_install make -C "$SOURCE_DIR" install PREFIX="$PREFIX" \
	SESSIONDIR="$SESSIONDIR" DESTDIR="$DESTDIR" LDFLAGS="$RPATH_FLAGS"

if [ "$UPDATE" -eq 1 ]; then
	printf '\n\033[1;32mgluewc is up to date.\033[0m Log out and back in to run the new build.\n'
else
	printf '\n\033[1;32mgluewc is installed.\033[0m Log out, select gluewc in your display manager, and log in.\n'
	printf 'TTY users can start it with: gluewc-session\n'
fi

if [ "$WITH_BAR" -eq 1 ]; then
	install_bar
fi

config_report "$SOURCE_DIR/config.def.conf" "$USER_CONFIG"
