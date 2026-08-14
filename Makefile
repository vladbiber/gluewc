.POSIX:
.SUFFIXES:

include config.mk

# flags for compiling
GLUEWCCPPFLAGS = -I. -DWLR_USE_UNSTABLE -D_POSIX_C_SOURCE=200809L \
	-DVERSION=\"$(VERSION)\" $(XWAYLAND)
GLUEWCDEVCFLAGS = -g -Wpedantic -Wall -Wextra -Wdeclaration-after-statement \
	-Wno-unused-parameter -Wshadow -Wunused-macros -Werror=strict-prototypes \
	-Werror=implicit -Werror=return-type -Werror=incompatible-pointer-types \
	-Wfloat-conversion

# CFLAGS / LDFLAGS
PKGS      = wayland-server xkbcommon libinput $(XLIBS)
GLUEWCCFLAGS = `$(PKG_CONFIG) --cflags $(PKGS)` $(SCENEFX_INCS) $(WLR_INCS) $(GLUEWCCPPFLAGS) $(GLUEWCDEVCFLAGS) $(CFLAGS)
LDLIBS    = `$(PKG_CONFIG) --libs $(PKGS)` $(SCENEFX_LIBS) $(WLR_LIBS) -lm $(LIBS)

all: gluewc

check: gluewc
	sh -n install.sh gluewc-session tests/drm-test.sh
	$(MAKE) -C tests
gluewc: gluewc.o util.o dwl-ipc-unstable-v2-protocol.o
	$(CC) gluewc.o util.o dwl-ipc-unstable-v2-protocol.o $(GLUEWCCFLAGS) $(LDFLAGS) $(LDLIBS) -o $@
gluewc.o: gluewc.c client.h config.h config.mk cursor-shape-v1-protocol.h \
	dwl-ipc-unstable-v2-protocol.h \
	pointer-constraints-unstable-v1-protocol.h wlr-layer-shell-unstable-v1-protocol.h \
	wlr-output-power-management-unstable-v1-protocol.h xdg-shell-protocol.h
util.o: util.c util.h
dwl-ipc-unstable-v2-protocol.o: dwl-ipc-unstable-v2-protocol.c dwl-ipc-unstable-v2-protocol.h

# wayland-scanner is a tool which generates C headers and rigging for Wayland
# protocols, which are specified in XML. wlroots requires you to rig these up
# to your build system yourself and provide them in the include path.
WAYLAND_SCANNER   = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
WAYLAND_PROTOCOLS = `$(PKG_CONFIG) --variable=pkgdatadir wayland-protocols`

cursor-shape-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/staging/cursor-shape/cursor-shape-v1.xml $@
pointer-constraints-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		$(WAYLAND_PROTOCOLS)/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml $@
wlr-layer-shell-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) enum-header \
		protocols/wlr-layer-shell-unstable-v1.xml $@
wlr-output-power-management-unstable-v1-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/wlr-output-power-management-unstable-v1.xml $@
xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		$(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@
dwl-ipc-unstable-v2-protocol.h:
	$(WAYLAND_SCANNER) server-header \
		protocols/dwl-ipc-unstable-v2.xml $@
dwl-ipc-unstable-v2-protocol.c:
	$(WAYLAND_SCANNER) private-code \
		protocols/dwl-ipc-unstable-v2.xml $@

config.h:
	cp config.def.h $@
clean:
	rm -f gluewc *.o *-protocol.h *-protocol.c

dist: clean
	mkdir -p gluewc-$(VERSION)
	cp -R .github docs LICENSE* Makefile CHANGELOG.md CONTRIBUTING.md \
		README.md SECURITY.md install.sh client.h config.def.h \
		config.def.conf config.mk protocols gluewc.1 gluewc.c util.c util.h \
		gluewc.desktop gluewc-session gluewc-$(VERSION)
	tar -caf gluewc-$(VERSION).tar.gz gluewc-$(VERSION)
	rm -rf gluewc-$(VERSION)

install: gluewc
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	rm -f $(DESTDIR)$(PREFIX)/bin/gluewc
	cp -f gluewc $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/gluewc
	cp -f gluewc-session $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/gluewc-session
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp -f gluewc.1 $(DESTDIR)$(MANDIR)/man1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/gluewc.1
	mkdir -p $(DESTDIR)$(DATADIR)/gluewc
	cp -f config.def.conf $(DESTDIR)$(DATADIR)/gluewc/config.def.conf
	chmod 644 $(DESTDIR)$(DATADIR)/gluewc/config.def.conf
	mkdir -p $(DESTDIR)$(DATADIR)/doc/gluewc
	cp -f README.md CONTRIBUTING.md SECURITY.md $(DESTDIR)$(DATADIR)/doc/gluewc
	cp -R docs $(DESTDIR)$(DATADIR)/doc/gluewc
	mkdir -p $(DESTDIR)$(SESSIONDIR)
	cp -f gluewc.desktop $(DESTDIR)$(SESSIONDIR)/gluewc.desktop
	chmod 644 $(DESTDIR)$(SESSIONDIR)/gluewc.desktop
	sync
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/gluewc $(DESTDIR)$(PREFIX)/bin/gluewc-session \
		$(DESTDIR)$(MANDIR)/man1/gluewc.1 \
		$(DESTDIR)$(DATADIR)/gluewc/config.def.conf \
		$(DESTDIR)$(SESSIONDIR)/gluewc.desktop
	rm -rf $(DESTDIR)$(DATADIR)/doc/gluewc

.PHONY: all check clean dist install uninstall

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CPPFLAGS) $(GLUEWCCFLAGS) -o $@ -c $<
