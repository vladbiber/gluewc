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
  --with-bar       also set up glueqs, the quickshell shell built for gluewc
                   (bar, wallpaper, launcher, OSDs, notifications), and start
                   it from the session. Installs Quickshell from the package
                   manager, or builds it where there is no package (Chimera,
                   Alpine, openSUSE). Works with --update too
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

# sudo where it exists, else doas (Chimera, some Artix and Alpine systems),
# else systemd's run0.
run_root() {
	if [ "$(id -u)" -eq 0 ]; then
		"$@"
	elif command -v sudo >/dev/null 2>&1; then
		sudo "$@"
	elif command -v doas >/dev/null 2>&1; then
		doas "$@"
	elif command -v run0 >/dev/null 2>&1; then
		run0 "$@"
	else
		die "sudo, doas or run0 is required for system installation"
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

# Quickshell is packaged on Arch and its derivatives (Artix carries it in
# galaxy), Void, Fedora 44+, Debian 14/unstable and Ubuntu 26.10+, and lives
# in the GURU overlay on Gentoo. Chimera, Alpine and openSUSE have no package,
# so there it is built from source by build_quickshell below. The bar's
# runtime helpers come along: curl fetches the weather, bluetoothctl and nmcli
# drive the network panel (NetworkManager itself is not forced on anyone; the
# Wi-Fi list just stays empty without it) and upower feeds the battery widget.
# The bar draws the wallpaper itself, so no wallpaper daemon is needed.
bar_packages() {
	case "$1" in
	arch)    printf 'quickshell curl bluez bluez-utils upower' ;;
	debian)  printf 'quickshell curl bluez upower' ;;
	fedora)  printf 'quickshell curl bluez upower' ;;
	void)    printf 'quickshell curl bluez upower' ;;
	alpine)  printf 'curl bluez upower' ;;
	chimera) printf 'curl bluez upower' ;;
	gentoo)  printf 'net-misc/curl net-wireless/bluez sys-power/upower' ;;
	suse)    printf 'curl bluez upower' ;;
	esac
}

# What building Quickshell 0.3 needs, where no package exists. CLI11 is not
# packaged on Chimera or Alpine and is header-only, so it is installed from
# source next to the build; the crash handler is left out so cpptrace is not
# needed either. openSUSE has both but the same build works there too.
quickshell_build_packages() {
	case "$1" in
	chimera) printf 'clang gmake cmake ninja pkgconf git qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtdeclarative-devel qt6-qtsvg-devel qt6-qtwayland-devel qt6-qtshadertools-devel spirv-tools vulkan-headers jemalloc-devel pipewire-devel polkit-devel linux-pam-devel libxcb-devel wayland-devel wayland-protocols libdrm-devel mesa-devel' ;;
	alpine)  printf 'cmake ninja pkgconf git qt6-qtbase-private-dev qt6-qtdeclarative-private-dev qt6-qtsvg-dev qt6-qtwayland-dev qt6-qtshadertools-dev spirv-tools vulkan-headers jemalloc-dev pipewire-dev polkit-dev linux-pam-dev libxcb-dev wayland-dev wayland-protocols libdrm-dev mesa-dev' ;;
	suse)    printf 'cmake ninja pkg-config git cli11-devel qt6-base-private-devel qt6-declarative-private-devel qt6-svg-devel qt6-wayland-private-devel qt6-shadertools-devel spirv-tools vulkan-headers jemalloc-devel pipewire-devel polkit-devel pam-devel libxcb-devel wayland-devel wayland-protocols-devel libdrm-devel' ;;
	esac
}

pm_install_cmd() {
	case "$FAMILY" in
	arch)    printf 'pacman -S --needed --noconfirm' ;;
	debian)  printf 'apt-get install -y' ;;
	fedora)  printf 'dnf install -y' ;;
	suse)    printf 'zypper --non-interactive install' ;;
	gentoo)  printf 'emerge --noreplace --ask=n' ;;
	alpine)  printf 'apk add' ;;
	chimera) printf 'apk add' ;;
	void)    printf 'xbps-install -Sy' ;;
	esac
}

# One command for the lot; if that fails, each package on its own, so a
# single name the repositories do not have (or a mirror hiccup on one of
# them) does not take the rest down with it. Whatever still fails is named.
install_list() {
	cmd=$(pm_install_cmd)
	[ -n "$cmd" ] || return 0
	[ -n "$*" ] || return 0
	if [ "$DRY_RUN" -eq 1 ]; then
		show_command $cmd "$@"
		return 0
	fi
	# shellcheck disable=SC2086
	run_root $cmd "$@" && return 0
	failed=
	for p in "$@"; do
		# shellcheck disable=SC2086
		run_root $cmd "$p" >/dev/null 2>&1 || failed="$failed $p"
	done
	[ -z "$failed" ] || warn "could not install:$failed"
	return 0
}

have_qs() {
	command -v qs >/dev/null 2>&1 || command -v quickshell >/dev/null 2>&1 \
		|| [ -x "$PREFIX/bin/qs" ]
}

# Quickshell from source, into $PREFIX. Only reached when the packages above
# left no qs behind. Quickshell needs private Qt headers and must be rebuilt
# whenever Qt is updated; the Qt version it was built against is recorded so
# --update can tell when that is due.
QUICKSHELL_VERSION=0.3.1
CLI11_VERSION=2.5.0
build_quickshell() {
	deps=$(quickshell_build_packages "$FAMILY")
	if [ -z "$deps" ]; then
		warn "no Quickshell package for '${ID:-unknown}' and no build recipe for family '$FAMILY'"
		warn "get it from https://quickshell.org; the bar is configured already"
		warn "and starts as soon as qs is on PATH"
		return 0
	fi
	log "Building Quickshell $QUICKSHELL_VERSION (no package on ${PRETTY_NAME:-$FAMILY})"
	install_list $deps
	[ "$DRY_RUN" -eq 0 ] || return 0
	for tool in cmake ninja git; do
		command -v "$tool" >/dev/null 2>&1 || { warn "missing $tool, Quickshell not built"; return 0; }
	done
	qsdir=$WORKDIR/quickshell
	if ! pkg-config --exists CLI11 2>/dev/null \
			&& [ ! -f "$PREFIX/lib/cmake/CLI11/CLI11Config.cmake" ] \
			&& [ ! -f "$PREFIX/share/cmake/CLI11/CLI11Config.cmake" ] \
			&& [ ! -f /usr/lib/cmake/CLI11/CLI11Config.cmake ] \
			&& [ ! -f /usr/lib64/cmake/CLI11/CLI11Config.cmake ] \
			&& [ ! -f /usr/share/cmake/CLI11/CLI11Config.cmake ]; then
		git clone --quiet --depth 1 --branch "v$CLI11_VERSION" \
			https://github.com/CLIUtils/CLI11.git "$WORKDIR/cli11" || return 0
		cmake -S "$WORKDIR/cli11" -B "$WORKDIR/cli11/build" -Wno-dev \
			-DCLI11_BUILD_TESTS=OFF -DCLI11_BUILD_EXAMPLES=OFF \
			-DCLI11_BUILD_DOCS=OFF -DCLI11_PRECOMPILED=OFF \
			-DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null || return 0
		run_root cmake --install "$WORKDIR/cli11/build" >/dev/null || return 0
	fi
	git clone --quiet --depth 1 --branch "v$QUICKSHELL_VERSION" \
		https://github.com/quickshell-mirror/quickshell.git "$qsdir" \
		|| { warn "could not fetch Quickshell"; return 0; }
	# clang rejects Quickshell's precompiled header once a target adds
	# -pthread ("POSIX thread support was disabled in precompiled file"),
	# so the header is skipped there
	pch=OFF
	case "$(cc --version 2>/dev/null | head -n1)" in *clang*) pch=ON ;; esac
	if ! cmake -G Ninja -S "$qsdir" -B "$qsdir/build" -Wno-dev \
			-DCMAKE_BUILD_TYPE=Release -DCRASH_HANDLER=OFF -DNO_PCH=$pch \
			-DDISTRIBUTOR="gluewc install.sh" \
			-DCMAKE_INSTALL_PREFIX="$PREFIX" \
			-DINSTALL_QML_PREFIX=lib/qt6/qml \
			-DCMAKE_PREFIX_PATH="$PREFIX"; then
		warn "Quickshell did not configure; the bar needs qs on PATH"
		return 0
	fi
	cmake --build "$qsdir/build" || { warn "Quickshell did not build"; return 0; }
	run_root cmake --install "$qsdir/build" >/dev/null || return 0
	# a fresh shell would find it; this one has not looked in $PREFIX/bin yet
	hash -r 2>/dev/null || true
	qtver=$(pkg-config --modversion Qt6Core 2>/dev/null || printf unknown)
	run_root sh -c "mkdir -p '$PREFIX/share/gluewc' && printf '%s\n' '$qtver' > '$PREFIX/share/gluewc/quickshell-qt-version'"
	log "Quickshell $QUICKSHELL_VERSION installed to $PREFIX/bin/qs (built against Qt $qtver)"
}

# A Quickshell built here against one Qt stops working after a Qt upgrade
# (it uses private Qt API), so --update rebuilds it when the version moved.
quickshell_stale() {
	f=$PREFIX/share/gluewc/quickshell-qt-version
	[ -r "$f" ] || return 1
	[ "$(cat "$f")" != "$(pkg-config --modversion Qt6Core 2>/dev/null)" ]
}

# The bar is written against Quickshell 0.2 or newer. The noctalia-qs fork
# reports its own 0.0.x numbers and is fine, so only upstream's version is
# checked; an old upstream fails with "module not installed", which is not
# an obvious message.
check_bar_runtime() {
	qs=$(command -v qs 2>/dev/null || command -v quickshell 2>/dev/null) || return 0
	ver=$("$qs" --version 2>/dev/null | head -n1)
	case "$ver" in
	[Qq]uickshell" 0.0."*|[Qq]uickshell" 0.1."*)
		warn "$ver is older than the bar needs (Quickshell 0.2+); update it"
		;;
	esac
}

# The bar's panels talk to daemons on the system bus. systemd systems have
# them socket- or D-Bus-activated; on OpenRC, runit, dinit and s6 the
# Bluetooth daemon needs enabling by hand, and it is the one thing the panel
# cannot do without. Everything else (upower, polkit) is D-Bus activated.
enable_bar_services() {
	[ "$DRY_RUN" -eq 0 ] || return 0
	if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
		run_root systemctl enable --now bluetooth.service >/dev/null 2>&1 || true
		return 0
	fi
	if command -v rc-update >/dev/null 2>&1; then
		[ "$FAMILY" = arch ] && install_list bluez-openrc
		[ -e /etc/init.d/bluetoothd ] && run_root rc-update add bluetoothd default >/dev/null 2>&1
		[ -e /etc/init.d/bluetooth ] && run_root rc-update add bluetooth default >/dev/null 2>&1
	elif [ -d /etc/runit/sv ] || [ -d /etc/sv ]; then
		[ "$FAMILY" = arch ] && install_list bluez-runit
		for svdir in /etc/runit/sv /etc/sv; do
			for name in bluetoothd bluetooth; do
				if [ -d "$svdir/$name" ]; then
					target=/run/runit/service
					[ -d "$target" ] || target=/var/service
					[ -d "$target" ] && [ ! -e "$target/$name" ] \
						&& run_root ln -s "$svdir/$name" "$target/$name" 2>/dev/null
				fi
			done
		done
	elif command -v dinitctl >/dev/null 2>&1; then
		[ "$FAMILY" = arch ] && install_list bluez-dinit
		[ "$FAMILY" = chimera ] && install_list bluez-dinit
		run_root dinitctl enable bluetoothd >/dev/null 2>&1 \
			|| run_root dinitctl enable bluetooth >/dev/null 2>&1 || true
	elif command -v s6-service >/dev/null 2>&1; then
		[ "$FAMILY" = arch ] && install_list bluez-s6
		run_root s6-service add default bluetoothd >/dev/null 2>&1 && run_root s6-db-reload >/dev/null 2>&1 || true
	fi
	return 0
}

install_bar() {
	bardir=${XDG_CONFIG_HOME:-$HOME/.config}/quickshell/glueqs

	log "Setting up the glueqs bar"
	# shellcheck disable=SC2046
	install_list $(bar_packages "$FAMILY")
	if ! have_qs || { [ "$UPDATE" -eq 1 ] && quickshell_stale; }; then
		build_quickshell
	fi
	enable_bar_services
	[ "$DRY_RUN" -eq 0 ] || return 0
	if [ -d "$bardir/.git" ]; then
		git -C "$bardir" pull --ff-only || warn "could not update $bardir"
	else
		mkdir -p "$(dirname "$bardir")"
		git clone --quiet "$BAR_REPO" "$bardir" \
			|| die "could not clone the bar from $BAR_REPO"
	fi

	# The session writes the config at first login; seed it now so the
	# autostart line has somewhere to live.
	seed_config
	# Upstream installs both names and every distribution that packages it
	# keeps them, but start whichever one is actually here rather than
	# assuming, and fall back to the short one when nothing is installed yet.
	barcmd=qs
	if ! command -v qs >/dev/null 2>&1 && command -v quickshell >/dev/null 2>&1; then
		barcmd=quickshell
	fi
	if [ -r "$USER_CONFIG" ] && ! grep -qE '^[[:space:]]*autostart[[:space:]]*=[[:space:]]*(qs|quickshell|glueqs)( |$)' "$USER_CONFIG"; then
		printf '\n# the glueqs bar, added by install.sh --with-bar\nautostart = %s -c glueqs\n' "$barcmd" >> "$USER_CONFIG"
		log "Added 'autostart = $barcmd -c glueqs' to $USER_CONFIG"
	fi

	if ! have_qs; then
		warn "quickshell is not installed: the bar is configured and starts as"
		warn "soon as qs is on PATH (https://quickshell.org)"
	else
		check_bar_runtime
	fi
	# the bar draws the wallpaper itself now; a wallpaper daemon left in the
	# autostart would paint over it
	if [ -r "$USER_CONFIG" ] && grep -qE '^[[:space:]]*autostart[[:space:]]*=[[:space:]]*(swww-daemon|waypaper|swaybg)' "$USER_CONFIG"; then
		warn "$USER_CONFIG still starts a wallpaper daemon (swww/waypaper/swaybg);"
		warn "glueqs sets the wallpaper itself, so remove that autostart line"
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
		"$DESTDIR$PREFIX/bin/gluewc-msg" "$DESTDIR$PREFIX/bin/gluewc-backlight" \
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
		# Chimera keeps its repositories under apk's own directory and has
		# no /etc/apk/repositories file; Alpine has the file.
		if [ -d /usr/lib/apk/db ] && [ ! -e /etc/apk/repositories ]; then
			printf 'chimera'
		else
			printf 'alpine'
		fi
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
	*chimera*)
		printf 'chimera' ;;
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
	chimera) printf 'pipewire wireplumber pipewire-alsa pipewire-dinit' ;;
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

  nix shell github:vladbiber/gluewc nixpkgs#alacritty nixpkgs#rofi \
    -c gluewc-session

The terminal and the launcher are named on purpose. The package deliberately
does not carry them, and the compiled defaults bind Super+Q and Super+Return
to alacritty and Super+Space to rofi, so without them you get a working but
completely empty screen and no way to open anything. Two leaner variants:

  nix run     github:vladbiber/gluewc                    # bare, no session
  nix develop github:vladbiber/gluewc                    # a shell for make

To INSTALL it, add the flake to /etc/finix and enable the module:

  inputs.gluewc.url = "github:vladbiber/gluewc";
  imports = [ inputs.gluewc.nixosModules.default ];
  programs.gluewc.enable = true;
  programs.gluewc.bar.enable = true;          # the glueqs bar, autostarted

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
# The desktop portal and its wlroots backend are what give sandboxed and
# Electron apps their file dialogs and screen sharing; without them Discord
# cannot share a screen and a Flatpak cannot open a file. They are installed
# with the rest of the session.
session_packages() {
	case "$1" in
	arch)   printf 'grim slurp wl-clipboard playerctl brightnessctl xdg-desktop-portal xdg-desktop-portal-wlr' ;;
	debian) printf 'grim slurp wl-clipboard playerctl brightnessctl xdg-desktop-portal xdg-desktop-portal-wlr' ;;
	fedora) printf 'grim slurp wl-clipboard playerctl brightnessctl xdg-desktop-portal xdg-desktop-portal-wlr' ;;
	suse)   printf 'grim slurp wl-clipboard playerctl brightnessctl xdg-desktop-portal xdg-desktop-portal-wlr' ;;
	gentoo) printf 'gui-apps/grim gui-apps/slurp gui-apps/wl-clipboard media-sound/playerctl dev-libs/light sys-apps/xdg-desktop-portal gui-libs/xdg-desktop-portal-wlr' ;;
	alpine) printf 'grim slurp wl-clipboard playerctl brightnessctl xdg-desktop-portal xdg-desktop-portal-wlr' ;;
	chimera) printf 'grim slurp wl-clipboard playerctl brightnessctl xdg-desktop-portal xdg-desktop-portal-wlr' ;;
	void)   printf 'grim slurp wl-clipboard playerctl brightnessctl xdg-desktop-portal xdg-desktop-portal-wlr' ;;
	esac
}

# The compiled defaults open alacritty on Super+Return and rofi on
# Super+Space; without a terminal and a launcher the session is a working
# but empty screen. Both are installed where the distribution has them.
# Chimera packages neither, so it gets foot and wmenu and the seeded config
# is pointed at those (see seed_config); an existing config is never edited.
TERMINAL_CMD=alacritty
LAUNCHER_CMD='rofi -show drun'
[ "$FAMILY" != chimera ] || { TERMINAL_CMD=foot; LAUNCHER_CMD=wmenu-run; }
app_packages() {
	case "$1" in
	arch|debian|fedora|suse|alpine|void) printf 'alacritty rofi' ;;
	gentoo)  printf 'x11-terms/alacritty x11-misc/rofi' ;;
	chimera) printf 'foot wmenu' ;;
	esac
}

# Copy the shipped defaults into place when there is no personal config yet,
# with the terminal and launcher swapped for the ones this system got. The
# session would do the same copy at first login, minus the swap.
seed_config() {
	[ ! -r "$USER_CONFIG" ] || return 0
	[ -r "$SOURCE_DIR/config.def.conf" ] || return 0
	mkdir -p "$(dirname "$USER_CONFIG")"
	if [ "$TERMINAL_CMD" = alacritty ] && [ "$LAUNCHER_CMD" = 'rofi -show drun' ]; then
		cp "$SOURCE_DIR/config.def.conf" "$USER_CONFIG"
	else
		sed -e "s/= spawn:alacritty\$/= spawn:$TERMINAL_CMD/" \
			-e "s/= spawn:rofi -show drun\$/= spawn:$LAUNCHER_CMD/" \
			"$SOURCE_DIR/config.def.conf" > "$USER_CONFIG"
		log "Seeded $USER_CONFIG with $TERMINAL_CMD and $LAUNCHER_CMD as terminal and launcher"
	fi
}

# An existing config that still names programs this system does not have.
report_apps() {
	[ -r "$USER_CONFIG" ] || return 0
	[ "$TERMINAL_CMD" = alacritty ] && [ "$LAUNCHER_CMD" = 'rofi -show drun' ] && return 0
	if grep -qE '^[[:space:]]*bind_insert.*= spawn:alacritty$' "$USER_CONFIG" && ! command -v alacritty >/dev/null 2>&1; then
		warn "$USER_CONFIG opens alacritty, which ${PRETTY_NAME:-this system} does not package;"
		warn "change the 'spawn:alacritty' binds to 'spawn:$TERMINAL_CMD'"
	fi
	if grep -qE '^[[:space:]]*bind_insert.*= spawn:rofi -show drun$' "$USER_CONFIG" && ! command -v rofi >/dev/null 2>&1; then
		warn "$USER_CONFIG launches with rofi, which ${PRETTY_NAME:-this system} does not package;"
		warn "change the 'spawn:rofi -show drun' bind to 'spawn:$LAUNCHER_CMD'"
	fi
}

nixos_instructions() {
	cat <<'EOF'

NixOS builds gluewc from the flake in this repository instead of installing
loose packages, so this script does not touch the system.

To TRY it, without installing anything. These build the compositor and then
start it: the screen is taken over until you quit with Super+Shift+Q, and
nothing is left behind afterwards.

  nix shell github:vladbiber/gluewc nixpkgs#alacritty nixpkgs#rofi \
    -c gluewc-session

The terminal and the launcher are named on purpose. The package deliberately
does not carry them, and the compiled defaults bind Super+Q and Super+Return
to alacritty and Super+Space to rofi, so without them you get a working but
completely empty screen and no way to open anything. Two leaner variants:

  nix run     github:vladbiber/gluewc                    # bare, no session
  nix develop github:vladbiber/gluewc                    # a shell for make

To INSTALL it, add the flake to your configuration and enable the module:

  inputs.gluewc.url = "github:vladbiber/gluewc";
  imports = [ inputs.gluewc.nixosModules.default ];
  programs.gluewc.enable = true;              # session entry + PipeWire audio
  programs.gluewc.bar.enable = true;          # the glueqs bar, autostarted

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
			libgbm-devel Mesa-libEGL-devel Mesa-libGLESv2-devel \
			seatd-devel libdisplay-info-devel libliftoff-devel \
			systemd-devel libxcb-devel xcb-util-wm-devel \
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
	chimera)
		# Chimera is clang and musl, ships wlroots 0.20 and calls GNU make
		# gmake; base-devel is only a marker package there.
		set -- apk add clang gmake git meson ninja pkgconf wayland-devel \
			wayland-protocols libinput-devel libxkbcommon-devel libxcb-devel \
			xcb-util-wm-devel xcb-util-errors-devel xcb-util-renderutil-devel \
			libdrm-devel mesa-devel pixman-devel libseat-devel \
			libdisplay-info-devel libliftoff-devel hwdata wlroots0.20-devel \
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
	set -- "$@" $(audio_packages "$FAMILY") $(session_packages "$FAMILY") $(app_packages "$FAMILY")

	if [ "$DRY_RUN" -eq 1 ]; then
		show_command "$@"
		if [ "$WITH_BAR" -eq 1 ]; then
			# shellcheck disable=SC2046
			install_list $(bar_packages "$FAMILY")
			have_qs || [ -z "$(quickshell_build_packages "$FAMILY")" ] \
				|| install_list $(quickshell_build_packages "$FAMILY")
		fi
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

# WirePlumber's default of switching Bluetooth headphones to the headset
# profile the moment any app opens the microphone is the classic "Discord
# broke my sound": the music drops to mono telephone quality and stays there.
# Keeping the headphones on their A2DP profile is written as a user-level
# drop-in, only when there is none already, so it is trivially reversible.
stable_audio_config() {
	[ "$WITH_AUDIO" -eq 1 ] || return 0
	[ "$(id -u)" -ne 0 ] || return 0
	dir=${XDG_CONFIG_HOME:-$HOME/.config}/wireplumber/wireplumber.conf.d
	file=$dir/50-gluewc-stable-audio.conf
	[ -e "$file" ] && return 0
	mkdir -p "$dir"
	cat >"$file" <<'CONF'
# Written by gluewc's install.sh. Delete this file to go back to the defaults.
# Do not drop Bluetooth headphones to the headset (HSP/HFP) profile whenever an
# application opens the microphone: it turns the music into mono telephone
# audio and it rarely switches back on its own.
wireplumber.settings = {
  bluetooth.autoswitch-to-headset-profile = false
}
CONF
	log "Wrote $file (keeps Bluetooth audio on its music profile)"
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
	stable_audio_config
fi

# Chimera installs GNU make as gmake and nothing as make.
MAKE=$(command -v gmake 2>/dev/null || command -v make 2>/dev/null || true)
[ -n "$MAKE" ] || die "missing tool: make"
for tool in cc git meson ninja pkg-config; do
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

# SceneFX's headers use glibc's __always_inline, which musl does not define;
# Chimera and Alpine get it spelled out for the SceneFX build and for gluewc,
# which includes those headers.
EXTRA_CFLAGS=
if ls /lib/ld-musl-*.so.1 >/dev/null 2>&1; then
	EXTRA_CFLAGS='-D__always_inline=inline'
fi

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
		-Dexamples=false -Dwerror=false ${EXTRA_CFLAGS:+-Dc_args=$EXTRA_CFLAGS}
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
"$MAKE" -C "$SOURCE_DIR" clean
"$MAKE" -C "$SOURCE_DIR" -j"$JOBS" PREFIX="$PREFIX" SESSIONDIR="$SESSIONDIR" \
	LDFLAGS="$RPATH_FLAGS" CFLAGS="$EXTRA_CFLAGS"

log "Installing gluewc to $PREFIX"
run_install "$MAKE" -C "$SOURCE_DIR" install PREFIX="$PREFIX" \
	SESSIONDIR="$SESSIONDIR" DESTDIR="$DESTDIR" LDFLAGS="$RPATH_FLAGS" \
	CFLAGS="$EXTRA_CFLAGS"

if [ "$UPDATE" -eq 1 ]; then
	printf '\n\033[1;32mgluewc is up to date.\033[0m Log out and back in to run the new build.\n'
else
	printf '\n\033[1;32mgluewc is installed.\033[0m Log out, select gluewc in your display manager, and log in.\n'
	printf 'TTY users can start it with: gluewc-session\n'
fi

if [ "$WITH_BAR" -eq 1 ]; then
	install_bar
fi
if [ "$WITH_DEPS" -eq 1 ]; then
	# a fresh system gets its config now, with the terminal and launcher
	# that were just installed
	seed_config
	report_apps
fi

config_report "$SOURCE_DIR/config.def.conf" "$USER_CONFIG"
