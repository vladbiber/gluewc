/* vkbd — virtual-keyboard injector for driving gluewc in headless tests.
 * Usage: vkbd [M+][S+][C+]<key>...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "vkbd-protocol.h"

static struct wl_seat *seat;
static struct zwp_virtual_keyboard_manager_v1 *vkm;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 7);
	else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
		vkm = wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_lis = { reg_global, reg_remove };

struct keymapent { const char *name; int code; };
static const struct keymapent keys[] = {
	{"1",2},{"2",3},{"3",4},{"4",5},{"5",6},{"6",7},{"7",8},{"8",9},{"9",10},
	{"q",16},{"t",20},{"g",34},{"i",23},{"s",31},{"f",33},{"h",35},{"j",36},{"k",37},{"l",38},
	{"c",46},{"v",47},{"b",48},{"r",19},{"o",24},{"m",50},{"n",49},{"w",17},
	{"0",11},{"minus",12},{"equal",13},
	{"comma",51},{"period",52},
	{"Return",28},{"Escape",1},{"Tab",15},
	{"Super",125},
	{"Left",105},{"Right",106},{"Up",103},{"Down",108},
	{"PgUp",104},{"PgDn",109},
};

static int keycode(const char *n)
{
	size_t i;
	for (i = 0; i < sizeof(keys)/sizeof(keys[0]); i++)
		if (!strcmp(keys[i].name, n))
			return keys[i].code;
	fprintf(stderr, "unknown key %s\n", n);
	exit(1);
}

int main(int argc, char *argv[])
{
	struct wl_display *dpy = wl_display_connect(NULL);
	struct wl_registry *reg;
	struct zwp_virtual_keyboard_v1 *kb;
	struct xkb_context *ctx;
	struct xkb_keymap *km;
	char *kmstr;
	size_t kmlen;
	int fd, i;
	uint32_t t = 100;

	if (!dpy) { fprintf(stderr, "no wayland display\n"); return 1; }
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_lis, NULL);
	wl_display_roundtrip(dpy);
	if (!seat || !vkm) { fprintf(stderr, "missing seat/vkm\n"); return 1; }

	kb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vkm, seat);

	ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	kmstr = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
	kmlen = strlen(kmstr) + 1;
	fd = memfd_create("km", 0);
	if (write(fd, kmstr, kmlen) != (ssize_t)kmlen) return 1;
	zwp_virtual_keyboard_v1_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, kmlen);
	wl_display_roundtrip(dpy);

	for (i = 1; i < argc; i++) {
		char buf[64];
		char *tok, *save = NULL, *last = NULL;
		int mods[4], nmods = 0, code, m;
		/* hold/release Super across other commands, and timed waits, so a
		 * concurrent vptr run can drag with the modifier held */
		if (!strcmp(argv[i], "MS-down")) {
			/* Super and Shift held together, for wheel and drag tests */
			zwp_virtual_keyboard_v1_key(kb, t++, 125, WL_KEYBOARD_KEY_STATE_PRESSED);
			zwp_virtual_keyboard_v1_key(kb, t++, 42, WL_KEYBOARD_KEY_STATE_PRESSED);
			zwp_virtual_keyboard_v1_modifiers(kb, 64 | 1, 0, 0, 0);
			wl_display_roundtrip(dpy);
			continue;
		}
		if (!strcmp(argv[i], "MS-up")) {
			zwp_virtual_keyboard_v1_key(kb, t++, 42, WL_KEYBOARD_KEY_STATE_RELEASED);
			zwp_virtual_keyboard_v1_key(kb, t++, 125, WL_KEYBOARD_KEY_STATE_RELEASED);
			zwp_virtual_keyboard_v1_modifiers(kb, 0, 0, 0, 0);
			wl_display_roundtrip(dpy);
			continue;
		}
		if (!strcmp(argv[i], "M-down")) {
			zwp_virtual_keyboard_v1_key(kb, t++, 125, WL_KEYBOARD_KEY_STATE_PRESSED);
			zwp_virtual_keyboard_v1_modifiers(kb, 64, 0, 0, 0);
			wl_display_roundtrip(dpy);
			continue;
		}
		if (!strcmp(argv[i], "M-up")) {
			zwp_virtual_keyboard_v1_key(kb, t++, 125, WL_KEYBOARD_KEY_STATE_RELEASED);
			zwp_virtual_keyboard_v1_modifiers(kb, 0, 0, 0, 0);
			wl_display_roundtrip(dpy);
			continue;
		}
		if (!strncmp(argv[i], "sleep", 5)) {
			wl_display_roundtrip(dpy);
			usleep(atoi(argv[i] + 5) * 1000);
			continue;
		}
		snprintf(buf, sizeof buf, "%s", argv[i]);
		for (tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
			if (last) {
				if (!strcmp(last, "M")) mods[nmods++] = 125;
				else if (!strcmp(last, "S")) mods[nmods++] = 42;
				else if (!strcmp(last, "C")) mods[nmods++] = 29;
				else { fprintf(stderr, "bad mod %s\n", last); return 1; }
			}
			last = tok;
		}
		code = keycode(last);
		{
			/* the compositor only sees modifier state we announce */
			uint32_t depressed = 0;
			for (m = 0; m < nmods; m++)
				depressed |= mods[m] == 125 ? 64 : mods[m] == 42 ? 1 : 4;
			for (m = 0; m < nmods; m++)
				zwp_virtual_keyboard_v1_key(kb, t++, mods[m], WL_KEYBOARD_KEY_STATE_PRESSED);
			zwp_virtual_keyboard_v1_modifiers(kb, depressed, 0, 0, 0);
			zwp_virtual_keyboard_v1_key(kb, t++, code, WL_KEYBOARD_KEY_STATE_PRESSED);
			zwp_virtual_keyboard_v1_key(kb, t++, code, WL_KEYBOARD_KEY_STATE_RELEASED);
			for (m = nmods - 1; m >= 0; m--)
				zwp_virtual_keyboard_v1_key(kb, t++, mods[m], WL_KEYBOARD_KEY_STATE_RELEASED);
			zwp_virtual_keyboard_v1_modifiers(kb, 0, 0, 0, 0);
		}
		wl_display_roundtrip(dpy);
		usleep(150000);
	}
	wl_display_roundtrip(dpy);
	return 0;
}
