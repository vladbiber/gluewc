/*
 * gluewc-msg — talk to gluewc over dwl-ipc-unstable-v2, the protocol the
 * compositor already speaks to bars.  This is what a shell without a native
 * dwl-ipc module (upstream quickshell, a script) uses to switch workspaces.
 *
 *   gluewc-msg [-o OUTPUT] workspace N     view workspace N (1-9)
 *   gluewc-msg [-o OUTPUT] move N          send the focused window to N
 *   gluewc-msg [-o OUTPUT] layout NAME     bsp, scroll or drift
 *   gluewc-msg status                      one line per output, then exit
 *   gluewc-msg outputs                     the monitor state file, verbatim
 *   gluewc-msg quit                        end the session
 *
 * Without -o the command goes to the first output the compositor lists.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include "dwl-ipc-unstable-v2-client-protocol.h"

#define MAXOUT 16

struct output {
	struct wl_output *wl;
	struct zdwl_ipc_output_v2 *ipc;
	char name[64];
	int active, layout, frames;
	unsigned int tags[32], ntags;
	unsigned int activetag;
};

static struct zdwl_ipc_manager_v2 *ipc_mgr;
static struct output outputs[MAXOUT];
static int noutputs;
static char layouts[8][16];
static unsigned int nlayouts, ntagcount;

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
		int32_t pw, int32_t ph, int32_t sub, const char *make,
		const char *model, int32_t tr) {}
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w,
		int32_t h, int32_t r) {}
static void out_done(void *d, struct wl_output *o) {}
static void out_scale(void *d, struct wl_output *o, int32_t s) {}
static void out_name(void *d, struct wl_output *o, const char *name)
{
	struct output *out = d;
	snprintf(out->name, sizeof out->name, "%s", name);
}
static void out_desc(void *d, struct wl_output *o, const char *desc) {}
static const struct wl_output_listener out_listener = {
	.geometry = out_geometry, .mode = out_mode, .done = out_done,
	.scale = out_scale, .name = out_name, .description = out_desc,
};

static void ipc_toggle_vis(void *d, struct zdwl_ipc_output_v2 *o) {}
static void ipc_active(void *d, struct zdwl_ipc_output_v2 *o, uint32_t active)
{
	((struct output *)d)->active = (int)active;
}
static void ipc_tag(void *d, struct zdwl_ipc_output_v2 *o, uint32_t tag,
		uint32_t state, uint32_t clients, uint32_t focused)
{
	struct output *out = d;
	if (tag < 32) {
		out->tags[tag] = clients;
		if (tag + 1 > out->ntags)
			out->ntags = tag + 1;
		if (state & ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE)
			out->activetag = tag;
	}
}
static void ipc_layout(void *d, struct zdwl_ipc_output_v2 *o, uint32_t layout)
{
	((struct output *)d)->layout = (int)layout;
}
static void ipc_title(void *d, struct zdwl_ipc_output_v2 *o, const char *t) {}
static void ipc_appid(void *d, struct zdwl_ipc_output_v2 *o, const char *a) {}
static void ipc_symbol(void *d, struct zdwl_ipc_output_v2 *o, const char *l) {}
static void ipc_frame(void *d, struct zdwl_ipc_output_v2 *o)
{
	((struct output *)d)->frames++;
}
static void ipc_fullscreen(void *d, struct zdwl_ipc_output_v2 *o, uint32_t f) {}
static void ipc_floating(void *d, struct zdwl_ipc_output_v2 *o, uint32_t f) {}
static const struct zdwl_ipc_output_v2_listener ipc_out_listener = {
	.toggle_visibility = ipc_toggle_vis, .active = ipc_active, .tag = ipc_tag,
	.layout = ipc_layout, .title = ipc_title, .appid = ipc_appid,
	.layout_symbol = ipc_symbol, .frame = ipc_frame,
	.fullscreen = ipc_fullscreen, .floating = ipc_floating,
};

static void mgr_tags(void *d, struct zdwl_ipc_manager_v2 *m, uint32_t n)
{
	ntagcount = n;
}
static void mgr_layout(void *d, struct zdwl_ipc_manager_v2 *m, const char *name)
{
	if (nlayouts < 8)
		snprintf(layouts[nlayouts++], sizeof layouts[0], "%s", name);
}
static const struct zdwl_ipc_manager_v2_listener mgr_listener = {
	.tags = mgr_tags, .layout = mgr_layout,
};

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
		const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_output_interface.name) && noutputs < MAXOUT) {
		struct output *out = &outputs[noutputs++];
		out->wl = wl_registry_bind(reg, name, &wl_output_interface,
				ver < 4 ? ver : 4);
		wl_output_add_listener(out->wl, &out_listener, out);
	} else if (!strcmp(iface, zdwl_ipc_manager_v2_interface.name)) {
		ipc_mgr = wl_registry_bind(reg, name, &zdwl_ipc_manager_v2_interface,
				ver < 2 ? ver : 2);
		zdwl_ipc_manager_v2_add_listener(ipc_mgr, &mgr_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) {}
static const struct wl_registry_listener reg_listener = {
	reg_global, reg_remove
};

static void
usage(void)
{
	fputs("usage: gluewc-msg [-o OUTPUT] workspace N | move N | layout NAME\n"
			"       gluewc-msg status | outputs | quit\n", stderr);
	exit(2);
}

int
main(int argc, char *argv[])
{
	struct wl_display *dpy;
	struct wl_registry *reg;
	const char *want = NULL, *cmd;
	struct output *target = NULL;
	int i, n;

	if (argc >= 3 && !strcmp(argv[1], "-o")) {
		want = argv[2];
		argv += 2;
		argc -= 2;
	}
	if (argc < 2)
		usage();
	cmd = argv[1];
	if (!strcmp(cmd, "outputs")) {
		/* what gluewc writes on every layout change: name, geometry,
		 * mode, scale, transform, mirror source and the mode list */
		char path[600], buf[4096];
		const char *env;
		FILE *f;
		size_t len;
		if ((env = getenv("XDG_STATE_HOME")) && *env)
			snprintf(path, sizeof path, "%s/gluewc/outputs", env);
		else if ((env = getenv("HOME")) && *env)
			snprintf(path, sizeof path, "%s/.local/state/gluewc/outputs", env);
		else
			return 1;
		if (!(f = fopen(path, "r"))) {
			perror(path);
			return 1;
		}
		while ((len = fread(buf, 1, sizeof buf, f)) > 0)
			fwrite(buf, 1, len, stdout);
		fclose(f);
		return 0;
	}

	if (!(dpy = wl_display_connect(NULL))) {
		fputs("gluewc-msg: no wayland display\n", stderr);
		return 1;
	}
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!ipc_mgr) {
		fputs("gluewc-msg: the compositor does not speak dwl-ipc\n", stderr);
		return 1;
	}
	wl_display_roundtrip(dpy); /* output names */
	for (i = 0; i < noutputs; i++) {
		outputs[i].ipc = zdwl_ipc_manager_v2_get_output(ipc_mgr, outputs[i].wl);
		zdwl_ipc_output_v2_add_listener(outputs[i].ipc, &ipc_out_listener,
				&outputs[i]);
	}
	wl_display_roundtrip(dpy); /* first status frame */

	if (!strcmp(cmd, "status")) {
		for (i = 0; i < noutputs; i++) {
			unsigned int t;
			printf("%s %d %u ", outputs[i].name, outputs[i].active,
					outputs[i].activetag);
			for (t = 0; t < outputs[i].ntags; t++)
				printf("%s%u", t ? "," : "", outputs[i].tags[t]);
			printf(" %s\n", outputs[i].layout >= 0
					&& (unsigned int)outputs[i].layout < nlayouts
					? layouts[outputs[i].layout] : "?");
		}
		return 0;
	}
	if (!strcmp(cmd, "quit")) {
		if (noutputs)
			zdwl_ipc_output_v2_quit(outputs[0].ipc);
		wl_display_roundtrip(dpy);
		return 0;
	}

	for (i = 0; i < noutputs; i++) {
		if (!want ? outputs[i].active : !strcmp(outputs[i].name, want)) {
			target = &outputs[i];
			break;
		}
	}
	if (!target && noutputs && !want)
		target = &outputs[0];
	if (!target) {
		fprintf(stderr, "gluewc-msg: no output named %s\n", want);
		return 1;
	}

	if (argc < 3)
		usage();
	if (!strcmp(cmd, "workspace") || !strcmp(cmd, "move")) {
		n = atoi(argv[2]);
		if (n < 1 || n > 31 || (ntagcount && (unsigned int)n > ntagcount)) {
			fprintf(stderr, "gluewc-msg: workspace must be 1..%u\n",
					ntagcount ? ntagcount : 9);
			return 1;
		}
		if (!strcmp(cmd, "workspace"))
			zdwl_ipc_output_v2_set_tags(target->ipc, 1u << (n - 1), 0);
		else
			zdwl_ipc_output_v2_set_client_tags(target->ipc, 0, 1u << (n - 1));
	} else if (!strcmp(cmd, "layout")) {
		unsigned int l;
		for (l = 0; l < nlayouts && strcmp(layouts[l], argv[2]); l++);
		if (l == nlayouts) {
			fprintf(stderr, "gluewc-msg: unknown layout %s\n", argv[2]);
			return 1;
		}
		zdwl_ipc_output_v2_set_layout(target->ipc, l);
	} else {
		usage();
	}
	wl_display_roundtrip(dpy);
	return 0;
}
