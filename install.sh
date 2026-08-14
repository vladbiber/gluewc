#!/bin/sh
set -eu

REPO_URL=${GLUEWC_REPO_URL:-https://github.com/vladbiber/gluewc.git}
REPO_REF=${GLUEWC_REF:-main}
PREFIX=${PREFIX:-/usr/local}
SESSIONDIR=${SESSIONDIR:-/usr/share/wayland-sessions}
DESTDIR=${DESTDIR:-}
WITH_DEPS=1
DEPS_ONLY=0
DRY_RUN=0
UNINSTALL=0
WORKDIR=

usage() {
	cat <<EOF
Usage: install.sh [options]

  --prefix PATH    installation prefix (default: /usr/local)
  --no-deps        do not install or build dependencies
  --deps-only      install dependencies, then stop
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
	--deps-only)
		DEPS_ONLY=1
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

install_packages() {
	case "$DISTRO" in
	*arch*|*manjaro*)
		set -- pacman -Syu --needed --noconfirm base-devel git meson ninja \
			pkgconf wayland wayland-protocols libinput libxkbcommon libxcb \
			xcb-util-wm libdrm mesa pixman seatd wlroots0.19 xorg-xwayland
		;;
	*debian*|*ubuntu*)
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
	*fedora*|*rhel*|*centos*)
		set -- dnf install -y gcc make git meson ninja-build \
			pkgconf-pkg-config wayland-devel wayland-protocols-devel \
			libinput-devel libxkbcommon-devel pixman-devel libdrm-devel \
			mesa-libgbm-devel mesa-libEGL-devel mesa-libGLES-devel \
			libseat-devel libdisplay-info-devel libliftoff-devel \
			systemd-devel libxcb-devel xcb-util-wm-devel \
			xcb-util-errors-devel xcb-util-renderutil-devel hwdata \
			wlroots-devel xorg-x11-server-Xwayland
		;;
	*suse*|*opensuse*)
		set -- zypper --non-interactive install -t pattern devel_basis \
			git meson ninja pkg-config wayland-devel wayland-protocols-devel \
			libinput-devel libxkbcommon-devel pixman-devel libdrm-devel \
			Mesa-libgbm-devel Mesa-libEGL-devel Mesa-libGLESv2-devel \
			libseat-devel libdisplay-info-devel libliftoff-devel \
			libudev-devel libxcb-devel xcb-util-wm-devel \
			xcb-util-errors-devel xcb-util-renderutil-devel wlroots-devel \
			xwayland
		;;
	*gentoo*)
		set -- emerge --noreplace --ask=n dev-vcs/git dev-build/meson \
			dev-build/ninja virtual/pkgconfig dev-libs/wayland \
			dev-libs/wayland-protocols dev-libs/libinput \
			x11-libs/libxkbcommon x11-libs/libxcb x11-libs/xcb-util-wm \
			gui-libs/wlroots:0.19 gui-libs/scenefx:0.4 x11-base/xwayland
		;;
	*alpine*)
		set -- apk add build-base git meson ninja pkgconf wayland-dev \
			wayland-protocols libinput-dev libxkbcommon-dev libxcb-dev \
			xcb-util-wm-dev libdrm-dev mesa-dev pixman-dev seatd-dev \
			wlroots0.19-dev scenefx-dev xwayland
		;;
	*void*)
		set -- xbps-install -Sy base-devel git meson ninja pkg-config \
			wayland-devel wayland-protocols libinput-devel \
			libxkbcommon-devel pixman-devel libdrm-devel MesaLib-devel \
			libseat-devel libdisplay-info-devel libliftoff-devel \
			eudev-libudev-devel libxcb-devel xcb-util-wm-devel \
			xcb-util-errors-devel xcb-util-renderutil-devel hwids \
			wlroots-devel xorg-server-xwayland
		;;
	*)
		die "unsupported distribution '${ID:-unknown}'; use --no-deps after installing the requirements manually"
		;;
	esac

	if [ "$DRY_RUN" -eq 1 ]; then
		show_command "$@"
	else
		run_root "$@"
	fi
}

if [ "$WITH_DEPS" -eq 1 ]; then
	log "Installing build dependencies for ${PRETTY_NAME:-${ID:-Linux}}"
	install_packages
	if [ "$DRY_RUN" -eq 1 ]; then
		printf '\nDry run complete; no changes were made.\n'
		exit 0
	fi
fi

for tool in cc make git meson ninja pkg-config; do
	command -v "$tool" >/dev/null 2>&1 || die "missing tool: $tool"
done

export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig:$PREFIX/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

pc_at_least() {
	pkg-config --atleast-version="$2" "$1" 2>/dev/null
}

check_source_requirements() {
	pc_at_least wayland-server 1.23.1 || die "Wayland >= 1.23.1 is required to build SceneFX 0.4"
	pc_at_least libdrm 2.4.122 || die "libdrm >= 2.4.122 is required to build SceneFX 0.4"
	pc_at_least pixman-1 0.43.0 || die "pixman >= 0.43.0 is required to build SceneFX 0.4"
}

WORKDIR=$(mktemp -d "${TMPDIR:-/tmp}/gluewc-install.XXXXXX")

if ! pkg-config --exists wlroots-0.19; then
	[ "$WITH_DEPS" -eq 1 ] || die "missing dependency: wlroots-0.19"
	check_source_requirements
	log "Building wlroots 0.19.3"
	git clone --quiet --depth 1 --branch 0.19.3 \
		https://gitlab.freedesktop.org/wlroots/wlroots.git "$WORKDIR/wlroots"
	meson setup "$WORKDIR/wlroots/build" "$WORKDIR/wlroots" \
		--prefix="$PREFIX" --libdir=lib --buildtype=release \
		-Dexamples=false -Dxwayland=enabled
	meson compile -C "$WORKDIR/wlroots/build"
	run_root meson install -C "$WORKDIR/wlroots/build"
fi

pkg-config --exists wlroots-0.19 || die "wlroots-0.19 was not found after installation"

if ! pkg-config --exists scenefx-0.4; then
	[ "$WITH_DEPS" -eq 1 ] || die "missing dependency: scenefx-0.4"
	check_source_requirements
	log "Building SceneFX 0.4.1"
	git clone --quiet --depth 1 --branch 0.4.1 \
		https://github.com/wlrfx/scenefx.git "$WORKDIR/scenefx"
	meson setup "$WORKDIR/scenefx/build" "$WORKDIR/scenefx" \
		--prefix="$PREFIX" --libdir=lib --buildtype=release \
		-Dexamples=false -Dwerror=false
	meson compile -C "$WORKDIR/scenefx/build"
	run_root meson install -C "$WORKDIR/scenefx/build"
fi

pkg-config --exists scenefx-0.4 || die "scenefx-0.4 was not found after installation"

if [ -z "$DESTDIR" ] && command -v ldconfig >/dev/null 2>&1; then
	run_root ldconfig
fi

if [ "$DEPS_ONLY" -eq 1 ]; then
	log "Dependencies are ready"
	exit 0
fi

case "$0" in
*/*|install.sh)
	SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || true)
	;;
*)
	SCRIPT_DIR=
	;;
esac
if [ -n "$SCRIPT_DIR" ] && [ -f "$SCRIPT_DIR/gluewc.c" ] \
		&& [ -f "$SCRIPT_DIR/Makefile" ]; then
	SOURCE_DIR=$SCRIPT_DIR
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

printf '\n\033[1;32mgluewc is installed.\033[0m Log out, select gluewc in your display manager, and log in.\n'
printf 'TTY users can start it with: gluewc-session\n'
