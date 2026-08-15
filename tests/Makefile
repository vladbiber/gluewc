PKG_CONFIG = pkg-config
WAYLAND_SCANNER = `$(PKG_CONFIG) --variable=wayland_scanner wayland-scanner`
CFLAGS_ALL = -D_GNU_SOURCE `$(PKG_CONFIG) --cflags wayland-client xkbcommon` $(CFLAGS)
LIBS = `$(PKG_CONFIG) --libs wayland-client xkbcommon`

all: vkbd wshot vptr

vkbd: vkbd.c vkbd-protocol.c vkbd-protocol.h
	$(CC) $(CFLAGS_ALL) -o $@ vkbd.c vkbd-protocol.c $(LIBS)
wshot: wshot.c screencopy-protocol.c screencopy-protocol.h
	$(CC) $(CFLAGS_ALL) -o $@ wshot.c screencopy-protocol.c $(LIBS)
vptr: vptr.c vptr-protocol.c vptr-protocol.h
	$(CC) $(CFLAGS_ALL) -o $@ vptr.c vptr-protocol.c $(LIBS)

vkbd-protocol.h: virtual-keyboard-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@
vkbd-protocol.c: virtual-keyboard-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@
screencopy-protocol.h: wlr-screencopy-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@
screencopy-protocol.c: wlr-screencopy-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@
vptr-protocol.h: wlr-virtual-pointer-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@
vptr-protocol.c: wlr-virtual-pointer-unstable-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

clean:
	rm -f vkbd wshot vptr *-protocol.c *-protocol.h
