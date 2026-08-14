/* wshot — wlr-screencopy screenshot of the first output, written as PPM. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "screencopy-protocol.h"

static struct wl_shm *shm;
static struct wl_output *output;
static struct zwlr_screencopy_manager_v1 *mgr;
static struct wl_buffer *buf;
static void *bufdata;
static uint32_t bw, bh, bstride;
static int done, failed;

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_output_interface.name) && !output)
		output = wl_registry_bind(reg, name, &wl_output_interface, 1);
	else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
		mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_lis = { reg_global, reg_remove };

static void fbuffer(void *d, struct zwlr_screencopy_frame_v1 *f,
		uint32_t format, uint32_t width, uint32_t height, uint32_t stride)
{
	int fd = memfd_create("shot", 0);
	struct wl_shm_pool *pool;
	if (ftruncate(fd, (off_t)stride * height) < 0)
		exit(1);
	bufdata = mmap(NULL, (size_t)stride * height, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	pool = wl_shm_create_pool(shm, fd, (int)(stride * height));
	buf = wl_shm_pool_create_buffer(pool, 0, (int)width, (int)height, (int)stride, format);
	bw = width; bh = height; bstride = stride;
	zwlr_screencopy_frame_v1_copy(f, buf);
}
static void fflags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags) {}
static void fready(void *d, struct zwlr_screencopy_frame_v1 *f,
		uint32_t s, uint32_t ns, uint32_t nss) { done = 1; }
static void ffailed(void *d, struct zwlr_screencopy_frame_v1 *f) { failed = 1; }
static const struct zwlr_screencopy_frame_v1_listener flis = {
	.buffer = fbuffer, .flags = fflags, .ready = fready, .failed = ffailed,
};

int main(int argc, char *argv[])
{
	struct wl_display *dpy = wl_display_connect(NULL);
	struct wl_registry *reg;
	struct zwlr_screencopy_frame_v1 *frame;
	FILE *out;
	uint32_t x, y;

	if (!dpy) return 1;
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_lis, NULL);
	wl_display_roundtrip(dpy);
	if (!shm || !output || !mgr) { fprintf(stderr, "missing globals\n"); return 1; }
	frame = zwlr_screencopy_manager_v1_capture_output(mgr, 0, output);
	zwlr_screencopy_frame_v1_add_listener(frame, &flis, NULL);
	while (!done && !failed && wl_display_dispatch(dpy) != -1);
	if (failed) { fprintf(stderr, "capture failed\n"); return 1; }
	out = fopen(argv[1] ? argv[1] : "shot.ppm", "w");
	fprintf(out, "P6\n%u %u\n255\n", bw, bh);
	for (y = 0; y < bh; y++) {
		unsigned char *row = (unsigned char *)bufdata + (size_t)y * bstride;
		for (x = 0; x < bw; x++) {
			unsigned char px[3] = { row[x * 4 + 2], row[x * 4 + 1], row[x * 4] };
			fwrite(px, 1, 3, out);
		}
	}
	fclose(out);
	return 0;
}
