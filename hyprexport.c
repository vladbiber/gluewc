/* hyprland_toplevel_export_v1 and hyprland_toplevel_mapping_v1: what a
 * Hyprland shell uses for live window thumbnails. A frame is the window as
 * drawn here, title bar included, without border or shadow; it is copied
 * when the window next commits (or at once with ignore_damage), so a
 * window that does not change costs nothing. */

#include <drm_fourcc.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_buffer.h>
#include "hyprland-toplevel-export-v1-protocol.h"
#include "hyprland-toplevel-mapping-v1-protocol.h"

typedef struct {
	struct wl_resource *resource;
	Client *c;
	int cursor, copied, ignore_damage;
	uint32_t seq;  /* surface commit the copy waits to pass */
	uint32_t since; /* when it started waiting, ms */
	struct wl_resource *buffer;
	struct wl_list link;
} ExportFrame;

static struct wl_list export_frames;
static struct wl_event_source *export_timer;
static struct wl_global *export_global, *mapping_global;

static Client *
exportftlclient(struct wl_resource *handle)
{
	Client *c;
	struct wl_resource *r;

	if (!handle)
		return NULL;
	wl_list_for_each(c, &clients, link) {
		if (!c->ftl)
			continue;
		wl_resource_for_each(r, &c->ftl->resources)
			if (r == handle)
				return c;
	}
	return NULL;
}

static void
exportframefree(ExportFrame *f)
{
	if (!f)
		return;
	wl_list_remove(&f->link);
	wl_resource_set_user_data(f->resource, NULL);
	free(f);
}

static void
exportframedestroy(struct wl_resource *resource)
{
	exportframefree(wl_resource_get_user_data(resource));
}

static void
exportsize(Client *c, int *w, int *h)
{
	struct wlr_box b;

	hyprboxof(c, &b);
	*w = MAX(1, b.width);
	*h = MAX(1, b.height);
}

typedef struct {
	struct wlr_render_pass *pass;
	int ox, oy, w, h;
	struct wlr_texture *tmp[32];
	int ntmp;
} ExportDraw;

static void
exportbuffer(ExportDraw *d, struct wlr_scene_buffer *sb, int x, int y)
{
	struct wlr_texture *tex = sb->WLR_PRIVATE.texture;
	struct wlr_box dst;
	int w, h;

	if (!sb->buffer)
		return;
	w = sb->dst_width > 0 ? sb->dst_width : sb->buffer->width;
	h = sb->dst_height > 0 ? sb->dst_height : sb->buffer->height;
	if (!tex) {
		struct wlr_client_buffer *cb = wlr_client_buffer_get(sb->buffer);
		if (cb && cb->texture)
			tex = cb->texture;
	}
	if (!tex && d->ntmp < (int)LENGTH(d->tmp)
			&& (tex = wlr_texture_from_buffer(drw, sb->buffer)))
		d->tmp[d->ntmp++] = tex;
	if (!tex)
		return;
	dst = (struct wlr_box){x - d->ox, y - d->oy, w, h};
	wlr_render_pass_add_texture(d->pass, &(struct wlr_render_texture_options){
		.texture = tex,
		.src_box = sb->src_box,
		.dst_box = dst,
		.transform = sb->transform,
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		.blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED,
	});
}

/* like wlr_scene_node_for_each_buffer, but a hidden window (another Space,
 * minimized) still gives its picture */
static void
exportwalk(ExportDraw *d, struct wlr_scene_node *node, int x, int y, int root)
{
	struct wlr_scene_node *child;

	if (!root && !node->enabled)
		return;
	x += node->x;
	y += node->y;
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		exportbuffer(d, wlr_scene_buffer_from_node(node), x, y);
	} else if (node->type == WLR_SCENE_NODE_TREE) {
		struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
		wl_list_for_each(child, &tree->children, link)
			exportwalk(d, child, x, y, 0);
	}
}

static int
exportdraw(Client *c, struct wlr_buffer *dst)
{
	ExportDraw d = {0};
	int i;

	exportsize(c, &d.w, &d.h);
	if (!(d.pass = wlr_renderer_begin_buffer_pass(drw, dst, NULL)))
		return 0;
	wlr_render_pass_add_rect(d.pass, &(struct wlr_render_rect_options){
		.box = {0, 0, dst->width, dst->height},
		.color = {0, 0, 0, 0},
		.blend_mode = WLR_RENDER_BLEND_MODE_NONE,
	});
	d.ox = (int)c->bw;
	d.oy = (int)c->bw;
	exportwalk(&d, &c->scene->node, -c->scene->node.x, -c->scene->node.y, 1);
	i = wlr_render_pass_submit(d.pass);
	while (d.ntmp > 0)
		wlr_texture_destroy(d.tmp[--d.ntmp]);
	return i;
}

static int
exportcopyshm(Client *c, struct wlr_buffer *dst)
{
	struct wlr_buffer *tmp;
	struct wlr_texture *tex;
	struct wlr_drm_format_set set = {0};
	void *data;
	uint32_t format;
	size_t stride;
	int ok = 0;

	/* the implicit modifier first: what the GPU picks for itself */
	wlr_drm_format_set_add(&set, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID);
	tmp = wlr_allocator_create_buffer(alloc, dst->width, dst->height,
			wlr_drm_format_set_get(&set, DRM_FORMAT_ARGB8888));
	wlr_drm_format_set_finish(&set);
	if (!tmp) {
		wlr_drm_format_set_add(&set, DRM_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR);
		tmp = wlr_allocator_create_buffer(alloc, dst->width, dst->height,
				wlr_drm_format_set_get(&set, DRM_FORMAT_ARGB8888));
		wlr_drm_format_set_finish(&set);
	}
	if (!tmp)
		return 0;
	if (exportdraw(c, tmp) && (tex = wlr_texture_from_buffer(drw, tmp))) {
		if (wlr_buffer_begin_data_ptr_access(dst, WLR_BUFFER_DATA_PTR_ACCESS_WRITE,
				&data, &format, &stride)) {
			ok = wlr_texture_read_pixels(tex, &(struct wlr_texture_read_pixels_options){
				.data = data, .format = format, .stride = (uint32_t)stride});
			wlr_buffer_end_data_ptr_access(dst);
		}
		wlr_texture_destroy(tex);
	}
	wlr_buffer_drop(tmp);
	return ok;
}

static void
exportfinish(ExportFrame *f)
{
	struct wlr_buffer *dst;
	struct timespec now;
	int w, h, ok = 0;

	if (!f->c || !f->buffer || !(dst = wlr_buffer_try_from_resource(f->buffer))) {
		hyprland_toplevel_export_frame_v1_send_failed(f->resource);
		exportframefree(f);
		return;
	}
	exportsize(f->c, &w, &h);
	if (dst->width == w && dst->height == h) {
		void *data;
		uint32_t format;
		size_t stride;
		if (wlr_buffer_begin_data_ptr_access(dst, WLR_BUFFER_DATA_PTR_ACCESS_READ,
				&data, &format, &stride)) {
			wlr_buffer_end_data_ptr_access(dst);
			ok = exportcopyshm(f->c, dst);
		} else {
			ok = exportdraw(f->c, dst);
		}
	}
	wlr_buffer_unlock(dst);
	if (!ok) {
		hyprland_toplevel_export_frame_v1_send_failed(f->resource);
		exportframefree(f);
		return;
	}
	clock_gettime(CLOCK_MONOTONIC, &now);
	hyprland_toplevel_export_frame_v1_send_flags(f->resource, 0);
	hyprland_toplevel_export_frame_v1_send_damage(f->resource, 0, 0, (uint32_t)w, (uint32_t)h);
	hyprland_toplevel_export_frame_v1_send_ready(f->resource,
			(uint32_t)((uint64_t)now.tv_sec >> 32), (uint32_t)now.tv_sec,
			(uint32_t)now.tv_nsec);
	exportframefree(f);
}

static uint32_t
exportseq(Client *c)
{
	struct wlr_surface *s = client_surface(c);
	return s ? s->current.seq : 0;
}

/* copies waiting for their window to change are checked every frame-ish,
 * an unchanged window still gets one every half second; the timer only
 * runs while one is waiting */
static int
exporttick(void *data)
{
	ExportFrame *f, *tmp;
	int waiting = 0;

	wl_list_for_each_safe(f, tmp, &export_frames, link) {
		if (!f->copied)
			continue;
		if (!f->c) {
			hyprland_toplevel_export_frame_v1_send_failed(f->resource);
			exportframefree(f);
		} else if (f->ignore_damage || exportseq(f->c) != f->seq
				|| now_ms() - f->since >= 500) {
			exportfinish(f);
		} else {
			waiting = 1;
		}
	}
	if (waiting)
		wl_event_source_timer_update(export_timer, 16);
	return 0;
}

static void
exportframecopy(struct wl_client *client, struct wl_resource *resource,
		struct wl_resource *buffer, int32_t ignore_damage)
{
	ExportFrame *f = wl_resource_get_user_data(resource);

	if (!f)
		return;
	if (f->copied) {
		wl_resource_post_error(resource, HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_ERROR_ALREADY_USED,
				"frame already used");
		return;
	}
	f->copied = 1;
	f->buffer = buffer;
	f->ignore_damage = ignore_damage;
	f->since = now_ms();
	if (f->c)
		f->seq = exportseq(f->c);
	wl_event_source_timer_update(export_timer, 1);
}

static void
exportframedestroyreq(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_export_frame_v1_interface export_frame_impl = {
	.copy = exportframecopy,
	.destroy = exportframedestroyreq,
};

static void
exportcapture(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		int32_t paint_cursor, Client *c)
{
	ExportFrame *f;
	struct wl_resource *res;
	int w, h;

	res = wl_resource_create(client, &hyprland_toplevel_export_frame_v1_interface,
			wl_resource_get_version(resource), id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	f = ecalloc(1, sizeof(*f));
	f->resource = res;
	f->c = c;
	f->cursor = paint_cursor;
	wl_list_insert(&export_frames, &f->link);
	wl_resource_set_implementation(res, &export_frame_impl, f, exportframedestroy);
	if (!c) {
		hyprland_toplevel_export_frame_v1_send_failed(res);
		exportframefree(f);
		return;
	}
	exportsize(c, &w, &h);
	hyprland_toplevel_export_frame_v1_send_buffer(res, WL_SHM_FORMAT_ARGB8888,
			(uint32_t)w, (uint32_t)h, (uint32_t)w * 4);
	if (wl_resource_get_version(res) >= 2) {
		hyprland_toplevel_export_frame_v1_send_linux_dmabuf(res, DRM_FORMAT_ARGB8888,
				(uint32_t)w, (uint32_t)h);
		hyprland_toplevel_export_frame_v1_send_buffer_done(res);
	}
}

static void
exportcapturehandle(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		int32_t paint_cursor, uint32_t handle)
{
	Client *c, *hit = NULL;

	wl_list_for_each(c, &clients, link)
		if ((uint32_t)(uintptr_t)c == handle && hyprlisted(c))
			hit = c;
	exportcapture(client, resource, id, paint_cursor, hit);
}

static void
exportcapturewlr(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		int32_t paint_cursor, struct wl_resource *handle)
{
	exportcapture(client, resource, id, paint_cursor, exportftlclient(handle));
}

static void
exportmgrdestroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_export_manager_v1_interface export_mgr_impl = {
	.capture_toplevel = exportcapturehandle,
	.destroy = exportmgrdestroy,
	.capture_toplevel_with_wlr_toplevel_handle = exportcapturewlr,
};

static void
exportbind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(client,
			&hyprland_toplevel_export_manager_v1_interface, (int)version, id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &export_mgr_impl, NULL, NULL);
}

static void
mappinghandledestroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_window_mapping_handle_v1_interface mapping_handle_impl = {
	.destroy = mappinghandledestroy,
};

static void
mappingreply(struct wl_client *client, struct wl_resource *resource, uint32_t id, Client *c)
{
	struct wl_resource *res = wl_resource_create(client,
			&hyprland_toplevel_window_mapping_handle_v1_interface,
			wl_resource_get_version(resource), id);
	uint64_t a = (uint64_t)(uintptr_t)c;

	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &mapping_handle_impl, NULL, NULL);
	if (c)
		hyprland_toplevel_window_mapping_handle_v1_send_window_address(res,
				(uint32_t)(a >> 32), (uint32_t)a);
	else
		hyprland_toplevel_window_mapping_handle_v1_send_failed(res);
}

static void
mappingext(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *toplevel)
{
	mappingreply(client, resource, id, NULL);
}

static void
mappingwlr(struct wl_client *client, struct wl_resource *resource, uint32_t id,
		struct wl_resource *toplevel)
{
	mappingreply(client, resource, id, exportftlclient(toplevel));
}

static void
mappingmgrdestroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct hyprland_toplevel_mapping_manager_v1_interface mapping_mgr_impl = {
	.get_window_for_toplevel = mappingext,
	.get_window_for_toplevel_wlr = mappingwlr,
	.destroy = mappingmgrdestroy,
};

static void
mappingbind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(client,
			&hyprland_toplevel_mapping_manager_v1_interface, (int)version, id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &mapping_mgr_impl, NULL, NULL);
}

/* a window going away fails the copies still waiting on it */
static void
exportforget(Client *c)
{
	ExportFrame *f;

	wl_list_for_each(f, &export_frames, link)
		if (f->c == c)
			f->c = NULL;
}

static void
exportsetup(void)
{
	wl_list_init(&export_frames);
	export_timer = wl_event_loop_add_timer(wl_display_get_event_loop(dpy), exporttick, NULL);
	export_global = wl_global_create(dpy, &hyprland_toplevel_export_manager_v1_interface,
			2, NULL, exportbind);
	mapping_global = wl_global_create(dpy, &hyprland_toplevel_mapping_manager_v1_interface,
			1, NULL, mappingbind);
}
