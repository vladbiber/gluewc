_VERSION = 0.3.0
VERSION  = `git describe --tags --dirty 2>/dev/null || echo $(_VERSION)`

PKG_CONFIG = pkg-config

# paths
PREFIX = /usr/local
MANDIR = $(PREFIX)/share/man
DATADIR = $(PREFIX)/share
SESSIONDIR = /usr/share/wayland-sessions

WLR_INCS = `$(PKG_CONFIG) --cflags wlroots-0.20`
WLR_LIBS = `$(PKG_CONFIG) --libs wlroots-0.20`

# SceneFX provides the fx scene graph (rounded corners, blur, shadows).
# It must come before wlroots at link time so its wlr_scene_* symbols win.
SCENEFX_INCS = `$(PKG_CONFIG) --cflags scenefx-0.5`
SCENEFX_LIBS = `$(PKG_CONFIG) --libs scenefx-0.5`

# Allow using an alternative wlroots installation
# This has to have all the includes required by wlroots, e.g:
# Assuming wlroots git repo is "${PWD}/wlroots" and you only ran "meson setup build && ninja -C build"
#WLR_INCS = -I/usr/include/pixman-1 -I/usr/include/elogind -I/usr/include/libdrm \
#	-I$(PWD)/wlroots/include
# Set -rpath to avoid using the wrong library.
#WLR_LIBS = -Wl,-rpath,$(PWD)/wlroots/build -L$(PWD)/wlroots/build -lwlroots-0.20

# Assuming you ran "meson setup --prefix ${PWD}/0.20 build && ninja -C build install"
#WLR_INCS = -I/usr/include/pixman-1 -I/usr/include/elogind -I/usr/include/libdrm \
#	-I$(PWD)/wlroots/0.20/include/wlroots-0.20
#WLR_LIBS = -Wl,-rpath,$(PWD)/wlroots/0.20/lib64 -L$(PWD)/wlroots/0.20/lib64 -lwlroots-0.20

XWAYLAND = -DXWAYLAND
XLIBS = xcb xcb-icccm
# Uncomment to build XWayland support
#XWAYLAND = -DXWAYLAND
#XLIBS = xcb xcb-icccm

# gluewc itself only uses C99 features, but wlroots' headers use anonymous unions (C11).
# To avoid warnings about them, we do not use -std=c99 and instead of using the
# gmake default 'CC=c99', we use cc.
CC = cc
