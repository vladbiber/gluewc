/* vptr — virtual-pointer injector for driving gluewc in headless tests.
 * Usage: vptr <cmd>...
 *   abs X Y            move to fraction of the layout (0.0–1.0)
 *   line X0 Y0 X1 Y1 N interpolated motion in N steps (20ms apart)
 *   press | release    left button
 *   rpress | rrelease  right button
 *   sleep MS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>
#include "vptr-protocol.h"

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *vpm;

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_seat_interface.name))
		seat = wl_registry_bind(reg, name, &wl_seat_interface, 7);
	else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
		vpm = wl_registry_bind(reg, name,
				&zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_lis = { reg_global, reg_remove };

int main(int argc, char *argv[])
{
	struct wl_display *dpy = wl_display_connect(NULL);
	struct wl_registry *reg;
	struct zwlr_virtual_pointer_v1 *p;
	uint32_t t = 100;
	int i;

	if (!dpy) { fprintf(stderr, "no wayland display\n"); return 1; }
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_lis, NULL);
	wl_display_roundtrip(dpy);
	if (!seat || !vpm) { fprintf(stderr, "missing seat/vpm\n"); return 1; }
	p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vpm, seat);
	wl_display_roundtrip(dpy);

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "abs") && i + 2 < argc) {
			double x = atof(argv[i + 1]), y = atof(argv[i + 2]);
			zwlr_virtual_pointer_v1_motion_absolute(p, t++,
					(uint32_t)(x * 10000), (uint32_t)(y * 10000),
					10000, 10000);
			zwlr_virtual_pointer_v1_frame(p);
			i += 2;
		} else if (!strcmp(argv[i], "line") && i + 5 < argc) {
			double x0 = atof(argv[i + 1]), y0 = atof(argv[i + 2]);
			double x1 = atof(argv[i + 3]), y1 = atof(argv[i + 4]);
			int n = atoi(argv[i + 5]), s;
			for (s = 0; s <= n; s++) {
				double f = (double)s / n;
				zwlr_virtual_pointer_v1_motion_absolute(p, t++,
						(uint32_t)((x0 + (x1 - x0) * f) * 10000),
						(uint32_t)((y0 + (y1 - y0) * f) * 10000),
						10000, 10000);
				zwlr_virtual_pointer_v1_frame(p);
				wl_display_roundtrip(dpy);
				usleep(20000);
			}
			i += 5;
		} else if (!strcmp(argv[i], "press") || !strcmp(argv[i], "release")
				|| !strcmp(argv[i], "rpress") || !strcmp(argv[i], "rrelease")) {
			int right = argv[i][0] == 'r';
			int down = !strcmp(argv[i] + right, "press");
			zwlr_virtual_pointer_v1_button(p, t++,
					right ? BTN_RIGHT : BTN_LEFT,
					down ? WL_POINTER_BUTTON_STATE_PRESSED
					     : WL_POINTER_BUTTON_STATE_RELEASED);
			zwlr_virtual_pointer_v1_frame(p);
		} else if (!strcmp(argv[i], "wheel") && i + 1 < argc) {
			/* notches: positive scrolls down/away from the user */
			int n = atoi(argv[i + 1]), s;
			for (s = 0; s < (n < 0 ? -n : n); s++) {
				zwlr_virtual_pointer_v1_axis_source(p,
						WL_POINTER_AXIS_SOURCE_WHEEL);
				zwlr_virtual_pointer_v1_axis_discrete(p, t,
						WL_POINTER_AXIS_VERTICAL_SCROLL,
						wl_fixed_from_double(n < 0 ? -15.0 : 15.0),
						n < 0 ? -1 : 1);
				zwlr_virtual_pointer_v1_frame(p);
				t += 20;
				wl_display_roundtrip(dpy);
				usleep(60000);
			}
			i += 1;
		} else if (!strcmp(argv[i], "fscroll") && i + 1 < argc) {
			/* touchpad-style (two-finger) vertical scrolling */
			double v = atof(argv[i + 1]);
			zwlr_virtual_pointer_v1_axis_source(p,
					WL_POINTER_AXIS_SOURCE_FINGER);
			zwlr_virtual_pointer_v1_axis(p, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
					wl_fixed_from_double(v));
			zwlr_virtual_pointer_v1_frame(p);
			t += 20;
			i += 1;
		} else if (!strcmp(argv[i], "sleep") && i + 1 < argc) {
			wl_display_roundtrip(dpy);
			usleep(atoi(argv[i + 1]) * 1000);
			i += 1;
		} else {
			fprintf(stderr, "bad command %s\n", argv[i]);
			return 1;
		}
		wl_display_roundtrip(dpy);
		usleep(30000);
	}
	wl_display_roundtrip(dpy);
	return 0;
}
