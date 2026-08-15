/*
 * See LICENSE file for copyright and license details.
 */
#include <getopt.h>
#include <limits.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_alpha_modifier_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_drm.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_linux_drm_syncobj_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/types/wlr_output_power_management_v1.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <scenefx/types/wlr_scene.h>
#include <scenefx/types/fx/corner_location.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/util/region.h>
#include <xkbcommon/xkbcommon.h>
#ifdef XWAYLAND
#include <wlr/xwayland.h>
#include <xcb/xcb.h>
#endif

#include "util.h"
#include "dwl-ipc-unstable-v2-protocol.h"

/* macros */
#define MAX(A, B)               ((A) > (B) ? (A) : (B))
#define MIN(A, B)               ((A) < (B) ? (A) : (B))
#define CLEANMASK(mask)         (mask & ~WLR_MODIFIER_CAPS)
#define VISIBLEON(C, M)         ((M) && (C)->mon == (M) && (C)->ws == (M)->ws)
#define LENGTH(X)               (sizeof X / sizeof X[0])
#define END(A)                  ((A) + LENGTH(A))
#define NUMWS                   9
#define LISTEN(E, L, H)         wl_signal_add((E), ((L)->notify = (H), (L)))
#define LISTEN_STATIC(E, H)     do { struct wl_listener *_l = ecalloc(1, sizeof(*_l)); _l->notify = (H); wl_signal_add((E), _l); } while (0)

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize }; /* cursor */
enum { XDGShell, LayerShell, X11 }; /* client types */
enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrOverlay, LyrBlock, NUM_LAYERS }; /* scene layers */
enum { ModeInsert, ModeNormal }; /* input modes */
enum { DirLeft, DirRight, DirUp, DirDown }; /* focus/swap directions */

typedef union {
	int i;
	uint32_t ui;
	float f;
	const void *v;
} Arg;

typedef struct {
	unsigned int mod;
	unsigned int button;
	void (*func)(const Arg *);
	const Arg arg;
} Button;

typedef struct Monitor Monitor;
typedef struct Client Client;

typedef struct Node Node;
struct Node {
	int leaf, horiz;
	float ratio;
	Node *a, *b, *par;
	Client *c;
};

struct Client {
	/* Must keep this field first */
	unsigned int type; /* XDGShell or X11* */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct {
		struct wlr_scene_rect *top, *bottom, *left, *right;
		struct wlr_scene_rect *top_left, *top_right;
		struct wlr_scene_rect *bottom_right, *bottom_left;
	} border; /* edge-only frame plus four rounded outer-corner pieces */
	struct wlr_scene_tree *scene_surface;
	struct wlr_scene_buffer *surfbuf; /* main surface buffer, for fx */
	struct wlr_foreign_toplevel_handle_v1 *ftl;
	struct wl_list link;
	struct wl_list flink;
	struct wlr_box geom; /* layout-relative, includes border */
	struct wlr_box prev; /* layout-relative, includes border */
	struct wlr_box bounds; /* only width and height are used */
	union {
		struct wlr_xdg_surface *xdg;
		struct wlr_xwayland_surface *xwayland;
	} surface;
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener commit;
	struct wl_listener precommit;
	struct wl_listener map;
	struct wl_listener maximize;
	struct wl_listener unmap;
	struct wl_listener destroy;
	struct wl_listener set_title;
	struct wl_listener fullscreen;
	struct wl_listener set_decoration_mode;
	struct wl_listener destroy_decoration;
	struct wl_listener ftl_activate;
	struct wl_listener ftl_close;
#ifdef XWAYLAND
	struct wl_listener activate;
	struct wl_listener associate;
	struct wl_listener dissociate;
	struct wl_listener configure;
#endif
	unsigned int bw;
	unsigned int ws;
	Node *node;
	int isfloating, isfullscreen, isfakefull;
	struct {
		int active, fadein, workspace, hide, closing;
		float t; /* progress 0..1, advanced only on rendered frames */
		struct wlr_box from, to; /* only x/y are used */
	} anim;
	uint32_t resize; /* configure serial of a pending resize */
};

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Key;

typedef struct {
	uint32_t mod;
	xkb_keysym_t keysym;
	void (*func)(const Arg *);
	Arg arg;
} Bind;

typedef struct {
	struct wlr_keyboard_group *wlr_group;

	int nsyms;
	const xkb_keysym_t *keysyms; /* invalid if nsyms == 0 */
	uint32_t mods; /* invalid if nsyms == 0 */
	struct wl_event_source *key_repeat_source;
	int super_down, super_alone;
	uint32_t overview_keycode;

	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
} KeyboardGroup;

typedef struct {
	/* Must keep this field first */
	unsigned int type; /* LayerShell */

	Monitor *mon;
	struct wlr_scene_tree *scene;
	struct wlr_scene_tree *popups;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_list link;
	int mapped;
	struct wlr_layer_surface_v1 *layer_surface;

	struct wl_listener destroy;
	struct wl_listener unmap;
	struct wl_listener surface_commit;
	struct wl_listener surface_precommit;
	struct {
		int active, closing, from_y, to_y;
		float t;
	} anim;
} LayerSurface;

struct Monitor {
	struct wl_list link;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_rect *fullscreen_bg; /* See createmon() for info */
	struct wl_listener frame;
	struct wl_listener destroy;
	struct wl_listener request_state;
	struct wl_listener destroy_lock_surface;
	struct wlr_session_lock_surface_v1 *lock_surface;
	struct wlr_box m; /* monitor area, layout-relative */
	struct wlr_box w; /* window area, layout-relative */
	struct wl_list layers[4]; /* LayerSurface.link */
	unsigned int ws; /* current workspace */
	Node *tree[NUMWS]; /* BSP tree per workspace */
	int gamma_lut_changed;
	uint32_t lastanimtick;
	int asleep;
	struct wlr_box overview[3];
	struct wlr_box overview_from[3];
	struct wlr_box overview_to[3];
	float overview_opacity[3];
	float overview_opacity_from[3];
	float overview_opacity_to[3];
	float overview_dim, overview_dim_from, overview_dim_to;
	float overview_anim_t;
	int overview_animating;
};

typedef struct {
	const char *name;
	float scale;
	enum wl_output_transform rr;
	int x, y;
} MonitorRule;

typedef struct {
	struct wlr_pointer_constraint_v1 *constraint;
	struct wl_listener destroy;
} PointerConstraint;

typedef struct {
	const char *id;
	const char *title;
	int ws; /* 1-9, 0 = current */
	int isfloating;
	int monitor;
} Rule;

typedef struct {
	struct wlr_scene_tree *scene;

	struct wlr_session_lock_v1 *lock;
	struct wl_listener new_surface;
	struct wl_listener unlock;
	struct wl_listener destroy;
} SessionLock;

typedef struct {
	struct wl_list link;
	struct wl_resource *resource;
	Monitor *mon;
} IpcOutput;

typedef struct {
	struct wlr_scene_tree *tree;
	float scalex, scaley, opacity;
	int srcx, srcy, x, y, radius, count;
} OverviewClone;

typedef struct {
	struct wl_list link;
	struct wlr_scene_buffer *scene;
	float opacity;
	int x, y, width, height;
} CloseBuffer;

typedef struct {
	struct wl_list link;
	struct wl_list buffers;
	struct wlr_scene_tree *tree;
	Monitor *mon;
	float t;
	int x, y, minx, miny, maxx, maxy;
} CloseAnim;

/* function declarations */
static void animkick(Monitor *m);
static void animstop(Monitor *m);
static void applybounds(Client *c, struct wlr_box *bbox);
static void applyeffects(Client *c);
static void applyrules(Client *c);
static void arrange(Monitor *m);
static void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void axisnotify(struct wl_listener *listener, void *data);
static void bsp_attach(Monitor *m, unsigned int ws, Client *c);
static void bsp_detach(Client *c);
static Node *bsp_first(Node *n);
static int bsp_has_tiled(Node *n);
static Node *bsp_nextleaf(Node *tree, Node *cur);
static Node *bsp_prevleaf(Node *tree, Node *cur);
static void bsp_tile(Node *n, struct wlr_box box);
static void buttonpress(struct wl_listener *listener, void *data);
static void chvt(const Arg *arg);
static void checkidleinhibitor(struct wlr_surface *exclude);
static float clientopacity(Client *c);
static int closeanimadvance(Monitor *m, float dt);
static void closeanimclear(Monitor *m);
static int closeanimstart(struct wlr_scene_node *node,
		struct wlr_scene_tree *parent, Monitor *m);
static void clientcloseanim(Client *c);
static void cleanup(void);
static void cleanupmon(struct wl_listener *listener, void *data);
static void cleanuplisteners(void);
static void closemon(Monitor *m);
static void commitlayersurfacenotify(struct wl_listener *listener, void *data);
static void commitnotify(struct wl_listener *listener, void *data);
static void precommitlayersurfacenotify(struct wl_listener *listener, void *data);
static void precommitnotify(struct wl_listener *listener, void *data);
static void commitpopup(struct wl_listener *listener, void *data);
static void createdecoration(struct wl_listener *listener, void *data);
static void createidleinhibitor(struct wl_listener *listener, void *data);
static void createkeyboard(struct wlr_keyboard *keyboard);
static KeyboardGroup *createkeyboardgroup(void);
static void createlayersurface(struct wl_listener *listener, void *data);
static void createlocksurface(struct wl_listener *listener, void *data);
static void createmon(struct wl_listener *listener, void *data);
static void createnotify(struct wl_listener *listener, void *data);
static void createpointer(struct wlr_pointer *pointer);
static void createpointerconstraint(struct wl_listener *listener, void *data);
static void createpopup(struct wl_listener *listener, void *data);
static void cursorconstrain(struct wlr_pointer_constraint_v1 *constraint);
static void cursorframe(struct wl_listener *listener, void *data);
static void cursorwarptohint(void);
static void destroydecoration(struct wl_listener *listener, void *data);
static void destroydragicon(struct wl_listener *listener, void *data);
static void destroyidleinhibitor(struct wl_listener *listener, void *data);
static void destroylayersurfacenotify(struct wl_listener *listener, void *data);
static void destroylock(SessionLock *lock, int unlocked);
static void destroylocksurface(struct wl_listener *listener, void *data);
static void destroynotify(struct wl_listener *listener, void *data);
static void destroypointerconstraint(struct wl_listener *listener, void *data);
static void destroysessionlock(struct wl_listener *listener, void *data);
static void destroykeyboardgroup(struct wl_listener *listener, void *data);
static Client *dirpick(Monitor *m, Client *from, int dir);
static void entermode(const Arg *arg);
static void focusclient(Client *c, int lift);
static const float *focuscolorfor(void);
static void focusdir(const Arg *arg);
static void focusnext(const Arg *arg);
static Client *focustop(Monitor *m);
static void ftlactivatenotify(struct wl_listener *listener, void *data);
static void ftlclosenotify(struct wl_listener *listener, void *data);
static void fullscreennotify(struct wl_listener *listener, void *data);
static void gpureset(struct wl_listener *listener, void *data);
static void handlesig(int signo);
static void inputdevice(struct wl_listener *listener, void *data);
static void ipcmgrbind(struct wl_client *client, void *data, uint32_t version, uint32_t id);
static void ipcnotifyall(void);
static void ipcstatus(IpcOutput *io);
static int keybinding(uint32_t mods, xkb_keysym_t sym);
static void keypress(struct wl_listener *listener, void *data);
static void keypressmod(struct wl_listener *listener, void *data);
static int keyrepeat(void *data);
static void killclient(const Arg *arg);
static int layeranimadvance(Monitor *m, float dt);
static void locksession(struct wl_listener *listener, void *data);
static void mapnotify(struct wl_listener *listener, void *data);
static void maximizenotify(struct wl_listener *listener, void *data);
static void motionabsolute(struct wl_listener *listener, void *data);
static void movetows(const Arg *arg);
static uint32_t now_ms(void);
static void toggleopacity(const Arg *arg);
static void motionnotify(uint32_t time, struct wlr_input_device *device, double sx,
		double sy, double sx_unaccel, double sy_unaccel);
static void motionrelative(struct wl_listener *listener, void *data);
static void moveresize(const Arg *arg);
static void movetowsfollow(const Arg *arg);
static void movewsstep(const Arg *arg);
static void outputmgrapply(struct wl_listener *listener, void *data);
static void outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test);
static void outputmgrtest(struct wl_listener *listener, void *data);
static void overviewbuild(void);
static void overviewbutton(struct wlr_pointer_button_event *event);
static void overviewclone(struct wlr_scene_buffer *buffer, int sx, int sy, void *data);
static int overviewkey(xkb_keysym_t sym);
static int overviewmotion(void);
static void overviewnavigate(unsigned int ws, int dir);
static void overviewrelayout(void);
static void overviewset(int active);
static void overviewtoggle(const Arg *arg);
static int overviewvalid(Monitor *m);
static void workspacestep(int dir);
static void swipebeginnotify(struct wl_listener *listener, void *data);
static void swipeendnotify(struct wl_listener *listener, void *data);
static void swipeupdatenotify(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
static void powermgrsetmode(struct wl_listener *listener, void *data);
static void quit(const Arg *arg);
static void readconfig(void);
static void reloadconfig(const Arg *arg);
static void rendermon(struct wl_listener *listener, void *data);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void requestmonstate(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int interact);
static void run(char *startup_cmd);
static void setcursor(struct wl_listener *listener, void *data);
static void setcursorshape(struct wl_listener *listener, void *data);
static void setfloating(Client *c, int floating);
static void setfullscreen(Client *c, int fullscreen);
static void setmon(Client *c, Monitor *m, int ws);
static void setpsel(struct wl_listener *listener, void *data);
static void setratio(const Arg *arg);
static void setsel(struct wl_listener *listener, void *data);
static void setup(void);
static void spawn(const Arg *arg);
static void startdrag(struct wl_listener *listener, void *data);
static void swapdir(const Arg *arg);
static void swapnodes(Client *sel, Client *t);
static void swapstack(const Arg *arg);
static void toggledecor(const Arg *arg);
static void togglefakefullscreen(const Arg *arg);
static void togglefloating(const Arg *arg);
static void togglefullscreen(const Arg *arg);
static void togglesplit(const Arg *arg);
static void unlocksession(struct wl_listener *listener, void *data);
static void unmaplayersurfacenotify(struct wl_listener *listener, void *data);
static void unmapnotify(struct wl_listener *listener, void *data);
static void updatemons(struct wl_listener *listener, void *data);
static void updatetitle(struct wl_listener *listener, void *data);
static void viewws(const Arg *arg);
static void virtualkeyboard(struct wl_listener *listener, void *data);
static void virtualpointer(struct wl_listener *listener, void *data);
static void warpto(Client *c);
static void wsstep(const Arg *arg);
static Monitor *xytomon(double x, double y);
static void xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny);

/* variables */
static pid_t child_pid = -1;
static int locked;
static int inputmode = ModeInsert;
static int decorhidden;

/* runtime configuration (~/.config/gluewc/config.conf) */
static Bind *runkeys, *runnormalkeys;
static size_t nrunkeys, nrunnormalkeys;
static char **autostarts;
static size_t nautostarts;
static char xkb_layout_buf[64], xkb_variant_buf[64], xkb_options_buf[128];

/* status bar IPC (dwl-ipc-unstable-v2, same protocol mmsg/waybar speak) */
static struct wl_list ipc_outputs;
static struct wlr_foreign_toplevel_manager_v1 *ftl_mgr;
static void *exclusive_focus;
static struct wl_display *dpy;
static struct wl_event_loop *event_loop;
static struct wlr_backend *backend;
static struct wlr_scene *scene;
static struct wlr_scene_tree *layers[NUM_LAYERS];
static struct wlr_scene_tree *drag_icon;
static struct wlr_scene_tree *overview_dim_scene;
static struct wlr_scene_tree *overview_scene;
static struct wlr_scene_tree *overview_panels_scene;
static struct wlr_scene_tree *overview_drag_scene;
static struct wl_list close_anims;
static int overview_active;
static int overview_visible;
static int overview_button_swallow;
static double overview_axis_dx, overview_axis_dy;
static uint32_t overview_axis_time;
static int overview_axis_triggered;
static double overview_swipe_dx, overview_swipe_dy;
static int overview_swipe_active, overview_swipe_triggered;
static Client *overview_drag_client;
static Client *overview_focus_client;
static int overview_dragging;
static double overview_press_x, overview_press_y;
static int overview_grab_x, overview_grab_y;
static struct wlr_box overview_drag_box;
/* Map from ZWLR_LAYER_SHELL_* constants to Lyr* enum */
static const int layermap[] = { LyrBg, LyrBottom, LyrTop, LyrOverlay };
static struct wlr_renderer *drw;
static struct wlr_allocator *alloc;
static struct wlr_compositor *compositor;
static struct wlr_session *session;

static struct wlr_xdg_shell *xdg_shell;
static struct wlr_xdg_decoration_manager_v1 *xdg_decoration_mgr;
static struct wl_list clients; /* tiling order */
static struct wl_list fstack;  /* focus order */
static struct wlr_idle_notifier_v1 *idle_notifier;
static struct wlr_idle_inhibit_manager_v1 *idle_inhibit_mgr;
static struct wlr_layer_shell_v1 *layer_shell;
static struct wlr_output_manager_v1 *output_mgr;
static struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
static struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
static struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr;
static struct wlr_output_power_manager_v1 *power_mgr;

static struct wlr_pointer_constraints_v1 *pointer_constraints;
static struct wlr_relative_pointer_manager_v1 *relative_pointer_mgr;
static struct wlr_pointer_constraint_v1 *active_constraint;

static struct wlr_cursor *cursor;
static struct wlr_xcursor_manager *cursor_mgr;

static struct wlr_scene_rect *root_bg;
static struct wlr_session_lock_manager_v1 *session_lock_mgr;
static struct wlr_scene_rect *locked_bg;
static struct wlr_session_lock_v1 *cur_lock;

static struct wlr_seat *seat;
static KeyboardGroup *kb_group;
static KeyboardGroup *super_group;
static unsigned int cursor_mode;
static Client *grabc;
static int grabcx, grabcy; /* client-relative */

static struct wlr_output_layout *output_layout;
static struct wlr_box sgeom;
static struct wl_list mons;
static Monitor *selmon;

/* global event handlers */
static struct wl_listener cursor_axis = {.notify = axisnotify};
static struct wl_listener cursor_button = {.notify = buttonpress};
static struct wl_listener cursor_frame = {.notify = cursorframe};
static struct wl_listener cursor_motion = {.notify = motionrelative};
static struct wl_listener cursor_motion_absolute = {.notify = motionabsolute};
static struct wl_listener cursor_swipe_begin = {.notify = swipebeginnotify};
static struct wl_listener cursor_swipe_end = {.notify = swipeendnotify};
static struct wl_listener cursor_swipe_update = {.notify = swipeupdatenotify};
static struct wl_listener gpu_reset = {.notify = gpureset};
static struct wl_listener layout_change = {.notify = updatemons};
static struct wl_listener new_idle_inhibitor = {.notify = createidleinhibitor};
static struct wl_listener new_input_device = {.notify = inputdevice};
static struct wl_listener new_virtual_keyboard = {.notify = virtualkeyboard};
static struct wl_listener new_virtual_pointer = {.notify = virtualpointer};
static struct wl_listener new_pointer_constraint = {.notify = createpointerconstraint};
static struct wl_listener new_output = {.notify = createmon};
static struct wl_listener new_xdg_toplevel = {.notify = createnotify};
static struct wl_listener new_xdg_popup = {.notify = createpopup};
static struct wl_listener new_xdg_decoration = {.notify = createdecoration};
static struct wl_listener new_layer_surface = {.notify = createlayersurface};
static struct wl_listener output_mgr_apply = {.notify = outputmgrapply};
static struct wl_listener output_mgr_test = {.notify = outputmgrtest};
static struct wl_listener output_power_mgr_set_mode = {.notify = powermgrsetmode};
static struct wl_listener request_cursor = {.notify = setcursor};
static struct wl_listener request_set_psel = {.notify = setpsel};
static struct wl_listener request_set_sel = {.notify = setsel};
static struct wl_listener request_set_cursor_shape = {.notify = setcursorshape};
static struct wl_listener request_start_drag = {.notify = requeststartdrag};
static struct wl_listener start_drag = {.notify = startdrag};
static struct wl_listener new_session_lock = {.notify = locksession};

#ifdef XWAYLAND
static void activatex11(struct wl_listener *listener, void *data);
static void associatex11(struct wl_listener *listener, void *data);
static void configurex11(struct wl_listener *listener, void *data);
static void createnotifyx11(struct wl_listener *listener, void *data);
static void dissociatex11(struct wl_listener *listener, void *data);
static void xwaylandready(struct wl_listener *listener, void *data);
static struct wl_listener new_xwayland_surface = {.notify = createnotifyx11};
static struct wl_listener xwayland_ready = {.notify = xwaylandready};
static struct wlr_xwayland *xwayland;
#endif

/* configuration, allows nested code to access above variables */
#include "config.h"

/* attempt to encapsulate suck into one file */
#include "client.h"

/* function implementations */
void
animkick(Monitor *m)
{
	if (!m)
		return;
	m->lastanimtick = now_ms();
	wlr_output_schedule_frame(m->wlr_output);
}

void
animstop(Monitor *m)
{
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (c->mon != m)
			continue;
		c->anim.active = 0;
		c->anim.fadein = 0;
		c->anim.workspace = 0;
		c->anim.hide = 0;
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		if (c->surfbuf)
			wlr_scene_buffer_set_opacity(c->surfbuf, clientopacity(c));
	}
}

uint32_t
now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void
logfilter(enum wlr_log_importance importance, const char *fmt, va_list args)
{
	struct timespec ts;
	/* scenefx spams this once per frame while blur is visible */
	if (strstr(fmt, "Failed to use optimized blur"))
		return;
	if (importance > wlr_log_get_verbosity())
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	fprintf(stderr, "%02u:%02u:%02u.%03u ", (unsigned)(ts.tv_sec / 3600) % 100,
			(unsigned)(ts.tv_sec / 60) % 60, (unsigned)ts.tv_sec % 60,
			(unsigned)(ts.tv_nsec / 1000000));
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);
}

static void
findsurfbuf(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	Client *c = data;
	struct wlr_scene_surface *s = wlr_scene_surface_try_from_buffer(buffer);
	if (s && s->surface == client_surface(c))
		c->surfbuf = buffer;
}

static float
clientopacity(Client *c)
{
	if (!opacityenabled || c->isfullscreen || c->isfakefull)
		return 1.0f;
	return win_opacity;
}

void
applyeffects(Client *c)
{
	int fs = c->isfullscreen || c->isfakefull;
	int r = fs ? 0 : MAX(0, corner_radius);
	int visible = !decorhidden && !fs && (unfocused_borders
			|| client_surface(c) == seat->keyboard_state.focused_surface);

	if (c->border.top) {
		client_set_border_enabled(c, visible);

		/* Edges are square; only the four outer pieces carry the radius. */
		wlr_scene_rect_set_corner_radius(c->border.top, 0, CORNER_LOCATION_NONE);
		wlr_scene_rect_set_corner_radius(c->border.bottom, 0, CORNER_LOCATION_NONE);
		wlr_scene_rect_set_corner_radius(c->border.left, 0, CORNER_LOCATION_NONE);
		wlr_scene_rect_set_corner_radius(c->border.right, 0, CORNER_LOCATION_NONE);
		wlr_scene_rect_set_corner_radius(c->border.top_left, r, CORNER_LOCATION_TOP_LEFT);
		wlr_scene_rect_set_corner_radius(c->border.top_right, r, CORNER_LOCATION_TOP_RIGHT);
		wlr_scene_rect_set_corner_radius(c->border.bottom_right, r, CORNER_LOCATION_BOTTOM_RIGHT);
		wlr_scene_rect_set_corner_radius(c->border.bottom_left, r, CORNER_LOCATION_BOTTOM_LEFT);
	}
	if (c->surfbuf) {
		wlr_scene_buffer_set_corner_radius(c->surfbuf,
				MAX(0, r - (int)c->bw), CORNER_LOCATION_ALL);
		wlr_scene_buffer_set_backdrop_blur(c->surfbuf, blurenabled && !fs);
		wlr_scene_buffer_set_backdrop_blur_optimized(c->surfbuf, 0);
		wlr_scene_buffer_set_backdrop_blur_ignore_transparent(c->surfbuf, 1);
		if (!c->anim.fadein)
			wlr_scene_buffer_set_opacity(c->surfbuf, clientopacity(c));
	}
}

void
toggleopacity(const Arg *arg)
{
	Client *c;
	opacityenabled ^= 1;
	wl_list_for_each(c, &clients, link)
		applyeffects(c);
}

static bool
closeaniminput(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	return false;
}

static void
closeanimclone(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	CloseAnim *a = data;
	CloseBuffer *b;
	int width, height;

	if (!buffer->buffer)
		return;
	width = buffer->dst_width > 0 ? buffer->dst_width
			: buffer->src_box.width > 0 ? (int)round(buffer->src_box.width)
			: buffer->buffer->width;
	height = buffer->dst_height > 0 ? buffer->dst_height
			: buffer->src_box.height > 0 ? (int)round(buffer->src_box.height)
			: buffer->buffer->height;
	if (width <= 0 || height <= 0)
		return;

	b = ecalloc(1, sizeof(*b));
	b->scene = wlr_scene_buffer_create(a->tree, buffer->buffer);
	b->scene->point_accepts_input = closeaniminput;
	if (buffer->src_box.width > 0 && buffer->src_box.height > 0)
		wlr_scene_buffer_set_source_box(b->scene, &buffer->src_box);
	wlr_scene_buffer_set_transform(b->scene, buffer->transform);
	wlr_scene_buffer_set_dest_size(b->scene, width, height);
	wlr_scene_buffer_set_opacity(b->scene, buffer->opacity);
	wlr_scene_buffer_set_filter_mode(b->scene, WLR_SCALE_FILTER_BILINEAR);
	wlr_scene_buffer_set_corner_radius(b->scene, buffer->corner_radius,
			buffer->corners);
	wlr_scene_buffer_set_backdrop_blur(b->scene, buffer->backdrop_blur);
	wlr_scene_buffer_set_backdrop_blur_optimized(b->scene,
			buffer->backdrop_blur_optimized);
	wlr_scene_buffer_set_backdrop_blur_ignore_transparent(b->scene,
			buffer->backdrop_blur_ignore_transparent);
	wlr_scene_buffer_set_opaque_region(b->scene, &buffer->opaque_region);
	b->opacity = buffer->opacity;
	b->x = sx - a->x;
	b->y = sy - a->y;
	b->width = width;
	b->height = height;
	wlr_scene_node_set_position(&b->scene->node, b->x, b->y);
	a->minx = MIN(a->minx, b->x);
	a->miny = MIN(a->miny, b->y);
	a->maxx = MAX(a->maxx, b->x + width);
	a->maxy = MAX(a->maxy, b->y + height);
	wl_list_insert(&a->buffers, &b->link);
}

static void
closeanimdestroy(CloseAnim *a)
{
	CloseBuffer *b, *tmp;

	wl_list_remove(&a->link);
	wl_list_for_each_safe(b, tmp, &a->buffers, link) {
		wl_list_remove(&b->link);
		free(b);
	}
	wlr_scene_node_destroy(&a->tree->node);
	free(a);
}

static int
closeanimstart(struct wlr_scene_node *node, struct wlr_scene_tree *parent,
		Monitor *m)
{
	CloseAnim *a;

	if (!animations || animation_duration <= 0 || overview_visible
			|| !node || !parent || !overviewvalid(m))
		return 0;
	a = ecalloc(1, sizeof(*a));
	if (!wlr_scene_node_coords(node, &a->x, &a->y)) {
		free(a);
		return 0;
	}
	a->tree = wlr_scene_tree_create(parent);
	a->mon = m;
	a->minx = a->miny = INT_MAX;
	a->maxx = a->maxy = INT_MIN;
	wl_list_init(&a->buffers);
	wlr_scene_node_for_each_buffer(node, closeanimclone, a);
	if (wl_list_empty(&a->buffers)) {
		wlr_scene_node_destroy(&a->tree->node);
		free(a);
		return 0;
	}
	wlr_scene_node_set_position(&a->tree->node, a->x, a->y);
	wl_list_insert(&close_anims, &a->link);
	animkick(m);
	return 1;
}

static void
clientcloseanim(Client *c)
{
	Monitor *m;
	struct wlr_scene_tree *parent;

	if (!c || !c->scene || c->anim.closing)
		return;
	m = c->mon ? c->mon : xytomon(c->geom.x + c->geom.width / 2.0,
			c->geom.y + c->geom.height / 2.0);
	parent = client_is_unmanaged(c) ? layers[LyrFloat]
			: layers[c->isfullscreen ? LyrFS : LyrFloat];
	c->anim.closing = closeanimstart(&c->scene->node, parent, m);
}

static int
closeanimadvance(Monitor *m, float dt)
{
	CloseAnim *a, *tmp;
	CloseBuffer *b;
	float e, scale;
	int cx, cy, duration, pending = 0;

	duration = MAX(140, animation_duration * 5 / 8);
	wl_list_for_each_safe(a, tmp, &close_anims, link) {
		if (a->mon != m)
			continue;
		a->t += duration > 0 ? dt / (float)duration : 1.0f;
		if (a->t >= 1.0f) {
			closeanimdestroy(a);
			continue;
		}
		pending = 1;
		e = a->t * a->t * (3.0f - 2.0f * a->t);
		scale = 1.0f - 0.045f * e;
		cx = (a->minx + a->maxx) / 2;
		cy = (a->miny + a->maxy) / 2;
		wlr_scene_node_set_position(&a->tree->node, a->x,
				a->y + (int)roundf(18.0f * e));
		wl_list_for_each(b, &a->buffers, link) {
			wlr_scene_node_set_position(&b->scene->node,
					cx + (int)roundf((b->x - cx) * scale),
					cy + (int)roundf((b->y - cy) * scale));
			wlr_scene_buffer_set_dest_size(b->scene,
					MAX(1, (int)roundf(b->width * scale)),
					MAX(1, (int)roundf(b->height * scale)));
			wlr_scene_buffer_set_opacity(b->scene, b->opacity * (1.0f - e));
		}
	}
	return pending;
}

static void
closeanimclear(Monitor *m)
{
	CloseAnim *a, *tmp;

	wl_list_for_each_safe(a, tmp, &close_anims, link)
		if (!m || a->mon == m)
			closeanimdestroy(a);
}

static void
layersetopacity(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	float opacity = *(float *)data;
	wlr_scene_buffer_set_opacity(buffer, opacity);
}

static int
layeranimated(LayerSurface *l)
{
	return l->layer_surface->current.keyboard_interactive
			!= ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
			&& l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP;
}

static void
layeropacity(LayerSurface *l, float opacity)
{
	wlr_scene_node_for_each_buffer(&l->scene->node, layersetopacity, &opacity);
	wlr_scene_node_for_each_buffer(&l->popups->node, layersetopacity, &opacity);
}

static void
layeranimstart(LayerSurface *l)
{
	if (!animations || animation_duration <= 0 || overview_visible
			|| !layeranimated(l))
		return;
	l->anim.active = 1;
	l->anim.t = 0.0f;
	l->anim.to_y = l->scene->node.y;
	l->anim.from_y = l->anim.to_y + 18;
	wlr_scene_node_set_position(&l->scene->node,
			l->scene->node.x, l->anim.from_y);
	wlr_scene_node_set_position(&l->popups->node,
			l->scene->node.x, l->anim.from_y);
	layeropacity(l, 0.0f);
	animkick(l->mon);
}

static int
layeranimadvance(Monitor *m, float dt)
{
	LayerSurface *l;
	float e, opacity;
	int i, duration, pending = 0;

	duration = MAX(160, animation_duration * 3 / 4);
	for (i = 0; i < 4; i++) {
		wl_list_for_each(l, &m->layers[i], link) {
			if (!l->anim.active)
				continue;
			l->anim.t += duration > 0 ? dt / (float)duration : 1.0f;
			if (l->anim.t >= 1.0f) {
				l->anim.active = 0;
				wlr_scene_node_set_position(&l->scene->node,
						l->scene->node.x, l->anim.to_y);
				wlr_scene_node_set_position(&l->popups->node,
						l->scene->node.x, l->anim.to_y);
				layeropacity(l, 1.0f);
				continue;
			}
			pending = 1;
			e = 1.0f - powf(1.0f - l->anim.t, 3.0f);
			opacity = l->anim.t * (2.0f - l->anim.t);
			wlr_scene_node_set_position(&l->scene->node, l->scene->node.x,
					l->anim.from_y
					+ (int)roundf((l->anim.to_y - l->anim.from_y) * e));
			wlr_scene_node_set_position(&l->popups->node,
					l->scene->node.x, l->scene->node.y);
			layeropacity(l, opacity);
		}
	}
	return pending;
}

void
applybounds(Client *c, struct wlr_box *bbox)
{
	/* set minimum possible */
	c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
	c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);

	if (c->geom.x >= bbox->x + bbox->width)
		c->geom.x = bbox->x + bbox->width - c->geom.width;
	if (c->geom.y >= bbox->y + bbox->height)
		c->geom.y = bbox->y + bbox->height - c->geom.height;
	if (c->geom.x + c->geom.width <= bbox->x)
		c->geom.x = bbox->x;
	if (c->geom.y + c->geom.height <= bbox->y)
		c->geom.y = bbox->y;
}

void
applyrules(Client *c)
{
	/* rule matching */
	const char *appid, *title;
	int i, ws = -1;
	const Rule *r;
	Monitor *mon = selmon, *m;

	appid = client_get_appid(c);
	title = client_get_title(c);

	for (r = rules; r < END(rules); r++) {
		if ((!r->title || strstr(title, r->title))
				&& (!r->id || strstr(appid, r->id))) {
			c->isfloating = r->isfloating;
			if (r->ws > 0 && r->ws <= NUMWS)
				ws = r->ws - 1;
			i = 0;
			wl_list_for_each(m, &mons, link) {
				if (r->monitor == i++)
					mon = m;
			}
		}
	}

	c->isfloating |= client_is_float_type(c);
	setmon(c, mon, ws);
}

void
arrange(Monitor *m)
{
	Client *c, *fs = NULL;

	if (!m->wlr_output->enabled)
		return;

	/* a fullscreen client (real or fake) is shown alone on its workspace */
	wl_list_for_each(c, &clients, link) {
		if (!fs && VISIBLEON(c, m) && (c->isfullscreen || c->isfakefull))
			fs = c;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			int vis = VISIBLEON(c, m) && (!fs || c == fs);
			int render = vis || (c->anim.workspace && c->anim.hide);
			wlr_scene_node_set_enabled(&c->scene->node, render);
			client_set_suspended(c, !vis);
		}
	}

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, fs && fs->isfullscreen);

	if (fs) {
		if (fs->isfakefull)
			resize(fs, m->w, 0);
	} else {
		bsp_tile(m->tree[m->ws], m->w);
	}
	motionnotify(0, NULL, 0, 0, 0, 0);
	checkidleinhibitor(NULL);
	ipcnotifyall();
}

void
arrangelayer(Monitor *m, struct wl_list *list, struct wlr_box *usable_area, int exclusive)
{
	LayerSurface *l;
	struct wlr_box full_area = m->m;

	wl_list_for_each(l, list, link) {
		struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;

		if (!layer_surface->initialized)
			continue;

		if (exclusive != (layer_surface->current.exclusive_zone > 0))
			continue;

		wlr_scene_layer_surface_v1_configure(l->scene_layer, &full_area, usable_area);
		wlr_scene_node_set_position(&l->popups->node, l->scene->node.x, l->scene->node.y);
	}
}

void
arrangelayers(Monitor *m)
{
	int i;
	struct wlr_box usable_area = m->m;
	LayerSurface *l;
	uint32_t layers_above_shell[] = {
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
	};
	if (!m->wlr_output->enabled)
		return;

	/* Arrange exclusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 1);

	if (!wlr_box_equal(&usable_area, &m->w)) {
		m->w = usable_area;
		arrange(m);
		if (overview_visible)
			overviewrelayout();
	}

	/* Arrange non-exlusive surfaces from top->bottom */
	for (i = 3; i >= 0; i--)
		arrangelayer(m, &m->layers[i], &usable_area, 0);

	/* Find topmost keyboard interactive layer, if such a layer exists */
	for (i = 0; i < (int)LENGTH(layers_above_shell); i++) {
		wl_list_for_each_reverse(l, &m->layers[layers_above_shell[i]], link) {
			if (locked || !l->layer_surface->current.keyboard_interactive || !l->mapped)
				continue;
			/* Deactivate the focused client. */
			focusclient(NULL, 0);
			exclusive_focus = l;
			client_notify_enter(l->layer_surface->surface, wlr_seat_get_keyboard(seat));
			return;
		}
	}
}

void
axisnotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_axis_event *event = data;
	double delta;
	int dir;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	delta = event->delta != 0.0 ? event->delta : event->delta_discrete;
	dir = delta > 0.0 ? 1 : delta < 0.0 ? -1 : 0;
	if (super_group && super_group->super_down
			&& (event->source == WL_POINTER_AXIS_SOURCE_WHEEL
				|| event->source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT)) {
		super_group->super_alone = 0;
		if (dir)
			workspacestep(dir);
		return;
	}
	if (overview_visible) {
		if (!overview_active)
			return;
		if (event->source == WL_POINTER_AXIS_SOURCE_WHEEL
				|| event->source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT) {
			if (dir)
				workspacestep(dir);
			return;
		}
		if (event->source != WL_POINTER_AXIS_SOURCE_FINGER)
			return;
		if (event->delta == 0.0) {
			if (event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL) {
				overview_axis_dx = overview_axis_dy = 0.0;
				overview_axis_triggered = 0;
			}
			return;
		}
		if ((uint32_t)(event->time_msec - overview_axis_time) > 180) {
			overview_axis_dx = overview_axis_dy = 0.0;
			overview_axis_triggered = 0;
		}
		overview_axis_time = event->time_msec;
		delta = event->relative_direction
				== WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED
				? -event->delta : event->delta;
		if (event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
			overview_axis_dx += delta;
		else
			overview_axis_dy += delta;
		if (!overview_axis_triggered
				&& fabs(overview_axis_dx) >= 48.0
				&& fabs(overview_axis_dx) > fabs(overview_axis_dy) * 1.2) {
			overview_axis_triggered = 1;
			workspacestep(overview_axis_dx < 0.0 ? 1 : -1);
		}
		return;
	}
	wlr_seat_pointer_notify_axis(seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

void
swipebeginnotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_begin_event *event = data;
	Monitor *m;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	overview_swipe_active = !locked && (event->fingers == 3
			|| (overview_active && event->fingers == 2));
	overview_swipe_triggered = 0;
	overview_swipe_dx = overview_swipe_dy = 0.0;
	if (!overview_swipe_active)
		return;
	m = xytomon(cursor->x, cursor->y);
	if (overviewvalid(m))
		selmon = m;
}

void
swipeupdatenotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_update_event *event = data;

	if (!overview_swipe_active || (event->fingers != 2 && event->fingers != 3)
			|| !overviewvalid(selmon))
		return;
	overview_swipe_dx += event->dx;
	overview_swipe_dy += event->dy;
	if (overview_swipe_triggered)
		return;
	if (fabs(overview_swipe_dx) >= 48.0
			&& fabs(overview_swipe_dx) > fabs(overview_swipe_dy) * 1.2) {
		overview_swipe_triggered = 1;
		workspacestep(overview_swipe_dx < 0.0 ? 1 : -1);
	} else if (event->fingers == 3 && fabs(overview_swipe_dy) >= 48.0
			&& fabs(overview_swipe_dy) > fabs(overview_swipe_dx) * 1.2) {
		overview_swipe_triggered = 1;
		overviewset(overview_swipe_dy < 0.0);
	}
}

void
swipeendnotify(struct wl_listener *listener, void *data)
{
	overview_swipe_active = 0;
	overview_swipe_triggered = 0;
	overview_swipe_dx = overview_swipe_dy = 0.0;
}

void
bsp_attach(Monitor *m, unsigned int ws, Client *c)
{
	Node *leaf, *t = NULL, *sp;
	Client *f;

	leaf = ecalloc(1, sizeof(Node));
	leaf->leaf = 1;
	leaf->ratio = 0.5f;
	leaf->c = c;
	c->node = leaf;
	c->ws = ws;

	if (!m->tree[ws]) {
		m->tree[ws] = leaf;
		return;
	}

	/* split the most recently focused leaf on this workspace */
	wl_list_for_each(f, &fstack, flink) {
		if (f != c && f->mon == m && f->ws == ws && f->node) {
			t = f->node;
			break;
		}
	}
	if (!t)
		t = bsp_first(m->tree[ws]);

	sp = ecalloc(1, sizeof(Node));
	sp->ratio = 0.5f;
	sp->horiz = t->c->geom.width > 0 ? t->c->geom.width >= t->c->geom.height
			: m->w.width >= m->w.height;
	sp->par = t->par;
	sp->a = t;
	sp->b = leaf;
	if (!t->par)
		m->tree[ws] = sp;
	else if (t->par->a == t)
		t->par->a = sp;
	else
		t->par->b = sp;
	t->par = sp;
	leaf->par = sp;
}

void
bsp_detach(Client *c)
{
	Node *n = c->node, *p, *sib;
	Monitor *m = c->mon;

	if (!n)
		return;
	c->node = NULL;
	if (!n->par) {
		if (m)
			m->tree[c->ws] = NULL;
		free(n);
		return;
	}
	p = n->par;
	sib = p->a == n ? p->b : p->a;
	sib->par = p->par;
	if (!p->par) {
		if (m)
			m->tree[c->ws] = sib;
	} else if (p->par->a == p) {
		p->par->a = sib;
	} else {
		p->par->b = sib;
	}
	free(p);
	free(n);
}

Node *
bsp_first(Node *n)
{
	while (n && !n->leaf)
		n = n->a;
	return n;
}

int
bsp_has_tiled(Node *n)
{
	if (!n)
		return 0;
	if (n->leaf)
		return !n->c->isfloating;
	return bsp_has_tiled(n->a) || bsp_has_tiled(n->b);
}

Node *
bsp_nextleaf(Node *tree, Node *cur)
{
	Node *n = cur;
	if (!cur || !tree)
		return bsp_first(tree);
	while (n->par) {
		if (n->par->a == n) {
			Node *r = bsp_first(n->par->b);
			if (r)
				return r;
		}
		n = n->par;
	}
	return bsp_first(tree);
}

Node *
bsp_prevleaf(Node *tree, Node *cur)
{
	Node *prev = NULL, *n;
	if (!tree)
		return NULL;
	n = bsp_first(tree);
	while (n) {
		if (n == cur)
			break;
		prev = n;
		n = bsp_nextleaf(tree, n);
		if (n == bsp_first(tree))
			break;
	}
	if (prev)
		return prev;
	/* cur is the first leaf: wrap to the last one */
	n = bsp_first(tree);
	prev = n;
	while ((n = bsp_nextleaf(tree, prev)) && n != bsp_first(tree))
		prev = n;
	return prev;
}

void
bsp_tile(Node *n, struct wlr_box box)
{
	/* Hiding borders is a visual choice; keep the tile spacing intact. */
	int gap = gappx;

	if (!n)
		return;
	if (n->leaf) {
		if (!n->c->isfloating) {
			resize(n->c, (struct wlr_box){.x = box.x + gap, .y = box.y + gap,
				.width = MAX(1, box.width - 2 * gap),
				.height = MAX(1, box.height - 2 * gap)}, 0);
			wlr_log(WLR_DEBUG, "bsp: ws=%u %dx%d%+d%+d", n->c->ws,
					n->c->geom.width, n->c->geom.height,
					n->c->geom.x, n->c->geom.y);
		}
		return;
	}
	if (!bsp_has_tiled(n->a) && !bsp_has_tiled(n->b))
		return;
	if (!bsp_has_tiled(n->a)) {
		bsp_tile(n->b, box);
		return;
	}
	if (!bsp_has_tiled(n->b)) {
		bsp_tile(n->a, box);
		return;
	}
	if (n->horiz) {
		int wa = (int)(box.width * n->ratio);
		bsp_tile(n->a, (struct wlr_box){box.x, box.y, wa, box.height});
		bsp_tile(n->b, (struct wlr_box){box.x + wa, box.y, box.width - wa, box.height});
	} else {
		int ha = (int)(box.height * n->ratio);
		bsp_tile(n->a, (struct wlr_box){box.x, box.y, box.width, ha});
		bsp_tile(n->b, (struct wlr_box){box.x, box.y + ha, box.width, box.height - ha});
	}
}

void
buttonpress(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_button_event *event = data;
	struct wlr_keyboard *keyboard;
	uint32_t mods;
	Client *c;
	const Button *b;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	if (overview_visible || overview_button_swallow) {
		overviewbutton(event);
		return;
	}

	switch (event->state) {
	case WL_POINTER_BUTTON_STATE_PRESSED:
		cursor_mode = CurPressed;
		selmon = xytomon(cursor->x, cursor->y);
		if (locked)
			break;

		/* Change focus if the button was _pressed_ over a client */
		xytonode(cursor->x, cursor->y, NULL, &c, NULL, NULL, NULL);
		if (c && (!client_is_unmanaged(c) || client_wants_focus(c)))
			focusclient(c, 1);

		keyboard = wlr_seat_get_keyboard(seat);
		mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
		for (b = buttons; b < END(buttons); b++) {
			if (CLEANMASK(mods) == CLEANMASK(b->mod) &&
					event->button == b->button && b->func) {
				b->func(&b->arg);
				return;
			}
		}
		break;
	case WL_POINTER_BUTTON_STATE_RELEASED:
		/* If you released any buttons, we exit interactive move/resize mode. */
		/* TODO: should reset to the pointer focus's current setcursor */
		if (!locked && cursor_mode != CurNormal && cursor_mode != CurPressed) {
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
			cursor_mode = CurNormal;
			/* Drop the window off on its new monitor */
			selmon = xytomon(cursor->x, cursor->y);
			setmon(grabc, selmon, -1);
			grabc = NULL;
			return;
		}
		cursor_mode = CurNormal;
		break;
	}
	/* If the event wasn't handled by the compositor, notify the client with
	 * pointer focus that a button press has occurred */
	wlr_seat_pointer_notify_button(seat,
			event->time_msec, event->button, event->state);
}

void
chvt(const Arg *arg)
{
	wlr_session_change_vt(session, arg->ui);
}

void
checkidleinhibitor(struct wlr_surface *exclude)
{
	int inhibited = 0, unused_lx, unused_ly;
	struct wlr_idle_inhibitor_v1 *inhibitor;
	wl_list_for_each(inhibitor, &idle_inhibit_mgr->inhibitors, link) {
		struct wlr_surface *surface = wlr_surface_get_root_surface(inhibitor->surface);
		struct wlr_scene_tree *tree = surface->data;
		if (exclude != surface && (bypass_surface_visibility || (!tree
				|| wlr_scene_node_coords(&tree->node, &unused_lx, &unused_ly)))) {
			inhibited = 1;
			break;
		}
	}

	wlr_idle_notifier_v1_set_inhibited(idle_notifier, inhibited);
}

void
cleanup(void)
{
	cleanuplisteners();
#ifdef XWAYLAND
	wlr_xwayland_destroy(xwayland);
	xwayland = NULL;
#endif
	wl_display_destroy_clients(dpy);
	closeanimclear(NULL);
	if (child_pid > 0) {
		kill(-child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}
	wlr_xcursor_manager_destroy(cursor_mgr);

	destroykeyboardgroup(&kb_group->destroy, NULL);

	/* If it's not destroyed manually, it will cause a use-after-free of wlr_seat.
	 * Destroy it until it's fixed on the wlroots side */
	wlr_backend_destroy(backend);

	wl_display_destroy(dpy);
	/* Destroy after the wayland display (when the monitors are already destroyed)
	   to avoid destroying them with an invalid scene output. */
	wlr_scene_node_destroy(&scene->tree.node);
}

void
cleanupmon(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy);
	LayerSurface *l, *tmp;
	IpcOutput *io;
	size_t i;

	wl_list_for_each(io, &ipc_outputs, link)
		if (io->mon == m)
			io->mon = NULL;

	/* m->layers[i] are intentionally not unlinked */
	for (i = 0; i < LENGTH(m->layers); i++) {
		wl_list_for_each_safe(l, tmp, &m->layers[i], link)
			wlr_layer_surface_v1_destroy(l->layer_surface);
	}

	wl_list_remove(&m->destroy.link);
	wl_list_remove(&m->frame.link);
	wl_list_remove(&m->link);
	wl_list_remove(&m->request_state.link);
	if (m->lock_surface)
		destroylocksurface(&m->destroy_lock_surface, NULL);
	m->wlr_output->data = NULL;
	wlr_output_layout_remove(output_layout, m->wlr_output);
	wlr_scene_output_destroy(m->scene_output);

	closemon(m);
	closeanimclear(m);
	wlr_scene_node_destroy(&m->fullscreen_bg->node);
	free(m);
}

void
cleanuplisteners(void)
{
	wl_list_remove(&cursor_axis.link);
	wl_list_remove(&cursor_button.link);
	wl_list_remove(&cursor_frame.link);
	wl_list_remove(&cursor_motion.link);
	wl_list_remove(&cursor_motion_absolute.link);
	wl_list_remove(&cursor_swipe_begin.link);
	wl_list_remove(&cursor_swipe_end.link);
	wl_list_remove(&cursor_swipe_update.link);
	wl_list_remove(&gpu_reset.link);
	wl_list_remove(&new_idle_inhibitor.link);
	wl_list_remove(&layout_change.link);
	wl_list_remove(&new_input_device.link);
	wl_list_remove(&new_virtual_keyboard.link);
	wl_list_remove(&new_virtual_pointer.link);
	wl_list_remove(&new_pointer_constraint.link);
	wl_list_remove(&new_output.link);
	wl_list_remove(&new_xdg_toplevel.link);
	wl_list_remove(&new_xdg_decoration.link);
	wl_list_remove(&new_xdg_popup.link);
	wl_list_remove(&new_layer_surface.link);
	wl_list_remove(&output_mgr_apply.link);
	wl_list_remove(&output_mgr_test.link);
	wl_list_remove(&output_power_mgr_set_mode.link);
	wl_list_remove(&request_cursor.link);
	wl_list_remove(&request_set_psel.link);
	wl_list_remove(&request_set_sel.link);
	wl_list_remove(&request_set_cursor_shape.link);
	wl_list_remove(&request_start_drag.link);
	wl_list_remove(&start_drag.link);
	wl_list_remove(&new_session_lock.link);
#ifdef XWAYLAND
	wl_list_remove(&new_xwayland_surface.link);
	wl_list_remove(&xwayland_ready.link);
#endif
}

void
closemon(Monitor *m)
{
	/* update selmon if needed and
	 * move closed monitor's clients to the focused one */
	Client *c;
	int i = 0, nmons = wl_list_length(&mons);
	if (!nmons) {
		selmon = NULL;
	} else if (m == selmon) {
		do /* don't switch to disabled mons */
			selmon = wl_container_of(mons.next, selmon, link);
		while (!selmon->wlr_output->enabled && i++ < nmons);

		if (!selmon->wlr_output->enabled)
			selmon = NULL;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->isfloating && c->geom.x > m->m.width)
			resize(c, (struct wlr_box){.x = c->geom.x - m->w.width, .y = c->geom.y,
					.width = c->geom.width, .height = c->geom.height}, 0);
		if (c->mon == m)
			setmon(c, selmon, c->ws);
	}
	focusclient(focustop(selmon), 1);
}

void
commitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, surface_commit);
	struct wlr_layer_surface_v1 *layer_surface = l->layer_surface;
	struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->current.layer]];
	struct wlr_layer_surface_v1_state old_state;
	int wasmapped;

	if (l->layer_surface->initial_commit) {
		client_set_scale(layer_surface->surface, l->mon->wlr_output->scale);

		/* Temporarily set the layer's current state to pending
		 * so that we can easily arrange it */
		old_state = l->layer_surface->current;
		l->layer_surface->current = l->layer_surface->pending;
		arrangelayers(l->mon);
		l->layer_surface->current = old_state;
		return;
	}

	if (layer_surface->current.committed == 0 && l->mapped == layer_surface->surface->mapped)
		return;
	wasmapped = l->mapped;
	l->mapped = layer_surface->surface->mapped;

	if (scene_layer != l->scene->node.parent) {
		wlr_scene_node_reparent(&l->scene->node, scene_layer);
		wl_list_remove(&l->link);
		wl_list_insert(&l->mon->layers[layer_surface->current.layer], &l->link);
		wlr_scene_node_reparent(&l->popups->node, (layer_surface->current.layer
				< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer));
	}

	arrangelayers(l->mon);
	if (!wasmapped && l->mapped)
		layeranimstart(l);
}

void
commitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, commit);

	if (c->surface.xdg->initial_commit) {
		/*
		 * Get the monitor this client will be rendered on
		 * Note that if the user set a rule in which the client is placed on
		 * a different monitor based on its title, this will likely select
		 * a wrong monitor.
		 */
		applyrules(c);
		if (c->mon) {
			client_set_scale(client_surface(c), c->mon->wlr_output->scale);
		}
		setmon(c, NULL, 0); /* Make sure to reapply rules in mapnotify() */

		wlr_xdg_toplevel_set_wm_capabilities(c->surface.xdg->toplevel,
				WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN);
		if (c->decoration)
			requestdecorationmode(&c->set_decoration_mode, c->decoration);
		wlr_xdg_toplevel_set_size(c->surface.xdg->toplevel, 0, 0);
		return;
	}

	resize(c, c->geom, (c->isfloating && !c->isfullscreen));

	/* mark a pending resize as completed */
	if (c->resize && c->resize <= c->surface.xdg->current.configure_serial)
		c->resize = 0;
}

static void
precommitnotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, precommit);
	struct wlr_surface *surface = client_surface(c);

	if (surface->mapped
			&& (surface->pending.committed & WLR_SURFACE_STATE_BUFFER)
			&& !surface->pending.buffer)
		clientcloseanim(c);
}

static void
precommitlayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, surface_precommit);
	struct wlr_surface *surface = l->layer_surface->surface;

	if (!l->anim.closing && surface->mapped && layeranimated(l)
			&& (surface->pending.committed & WLR_SURFACE_STATE_BUFFER)
			&& !surface->pending.buffer)
		l->anim.closing = closeanimstart(&l->scene->node,
				layers[layermap[l->layer_surface->current.layer]], l->mon);
}

void
commitpopup(struct wl_listener *listener, void *data)
{
	struct wlr_surface *surface = data;
	struct wlr_xdg_popup *popup = wlr_xdg_popup_try_from_wlr_surface(surface);
	LayerSurface *l = NULL;
	Client *c = NULL;
	struct wlr_box box;
	int type = -1;

	if (!popup->base->initial_commit)
		return;

	type = toplevel_from_wlr_surface(popup->base->surface, &c, &l);
	if (!popup->parent || type < 0)
		return;
	popup->base->surface->data = wlr_scene_xdg_surface_create(
			popup->parent->data, popup->base);
	if ((l && !l->mon) || (c && !c->mon)) {
		wlr_xdg_popup_destroy(popup);
		return;
	}
	box = type == LayerShell ? l->mon->m : c->mon->w;
	box.x -= (type == LayerShell ? l->scene->node.x : c->geom.x);
	box.y -= (type == LayerShell ? l->scene->node.y : c->geom.y);
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
	wl_list_remove(&listener->link);
	free(listener);
}

void
createdecoration(struct wl_listener *listener, void *data)
{
	struct wlr_xdg_toplevel_decoration_v1 *deco = data;
	Client *c = deco->toplevel->base->data;
	c->decoration = deco;

	LISTEN(&deco->events.request_mode, &c->set_decoration_mode, requestdecorationmode);
	LISTEN(&deco->events.destroy, &c->destroy_decoration, destroydecoration);

	requestdecorationmode(&c->set_decoration_mode, deco);
}

void
createidleinhibitor(struct wl_listener *listener, void *data)
{
	struct wlr_idle_inhibitor_v1 *idle_inhibitor = data;
	LISTEN_STATIC(&idle_inhibitor->events.destroy, destroyidleinhibitor);

	checkidleinhibitor(NULL);
}

void
createkeyboard(struct wlr_keyboard *keyboard)
{
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(keyboard, kb_group->wlr_group->keyboard.keymap);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(kb_group->wlr_group, keyboard);
}

KeyboardGroup *
createkeyboardgroup(void)
{
	KeyboardGroup *group = ecalloc(1, sizeof(*group));
	struct xkb_context *context;
	struct xkb_keymap *keymap;

	group->wlr_group = wlr_keyboard_group_create();
	group->wlr_group->data = group;

	/* Prepare an XKB keymap and assign it to the keyboard group. */
	context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!(keymap = xkb_keymap_new_from_names(context, &xkb_rules,
				XKB_KEYMAP_COMPILE_NO_FLAGS)))
		die("failed to compile keymap");

	wlr_keyboard_set_keymap(&group->wlr_group->keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);

	wlr_keyboard_set_repeat_info(&group->wlr_group->keyboard, repeat_rate, repeat_delay);

	/* Set up listeners for keyboard events */
	LISTEN(&group->wlr_group->keyboard.events.key, &group->key, keypress);
	LISTEN(&group->wlr_group->keyboard.events.modifiers, &group->modifiers, keypressmod);

	group->key_repeat_source = wl_event_loop_add_timer(event_loop, keyrepeat, group);

	/* A seat can only have one keyboard, but this is a limitation of the
	 * Wayland protocol - not wlroots. We assign all connected keyboards to the
	 * same wlr_keyboard_group, which provides a single wlr_keyboard interface for
	 * all of them. Set this combined wlr_keyboard as the seat keyboard.
	 */
	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	return group;
}

void
createlayersurface(struct wl_listener *listener, void *data)
{
	struct wlr_layer_surface_v1 *layer_surface = data;
	LayerSurface *l;
	struct wlr_surface *surface = layer_surface->surface;
	struct wlr_scene_tree *scene_layer = layers[layermap[layer_surface->pending.layer]];

	if (!layer_surface->output
			&& !(layer_surface->output = selmon ? selmon->wlr_output : NULL)) {
		wlr_layer_surface_v1_destroy(layer_surface);
		return;
	}

	l = layer_surface->data = ecalloc(1, sizeof(*l));
	l->type = LayerShell;
	LISTEN(&surface->events.client_commit, &l->surface_precommit,
			precommitlayersurfacenotify);
	LISTEN(&surface->events.commit, &l->surface_commit, commitlayersurfacenotify);
	LISTEN(&surface->events.unmap, &l->unmap, unmaplayersurfacenotify);
	LISTEN(&layer_surface->events.destroy, &l->destroy, destroylayersurfacenotify);

	l->layer_surface = layer_surface;
	l->mon = layer_surface->output->data;
	l->scene_layer = wlr_scene_layer_surface_v1_create(scene_layer, layer_surface);
	l->scene = l->scene_layer->tree;
	l->popups = surface->data = wlr_scene_tree_create(layer_surface->current.layer
			< ZWLR_LAYER_SHELL_V1_LAYER_TOP ? layers[LyrTop] : scene_layer);
	l->scene->node.data = l->popups->node.data = l;

	wl_list_insert(&l->mon->layers[layer_surface->pending.layer],&l->link);
	wlr_surface_send_enter(surface, layer_surface->output);
}

void
createlocksurface(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, new_surface);
	struct wlr_session_lock_surface_v1 *lock_surface = data;
	Monitor *m = lock_surface->output->data;
	struct wlr_scene_tree *scene_tree = lock_surface->surface->data
			= wlr_scene_subsurface_tree_create(lock->scene, lock_surface->surface);
	m->lock_surface = lock_surface;

	wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
	wlr_session_lock_surface_v1_configure(lock_surface, m->m.width, m->m.height);

	LISTEN(&lock_surface->events.destroy, &m->destroy_lock_surface, destroylocksurface);

	if (m == selmon)
		client_notify_enter(lock_surface->surface, wlr_seat_get_keyboard(seat));
}

void
createmon(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new output (aka a display or
	 * monitor) becomes available. */
	struct wlr_output *wlr_output = data;
	const MonitorRule *r;
	size_t i;
	struct wlr_output_state state;
	Monitor *m;

	if (!wlr_output_init_render(wlr_output, alloc, drw))
		return;

	m = wlr_output->data = ecalloc(1, sizeof(*m));
	m->wlr_output = wlr_output;

	for (i = 0; i < LENGTH(m->layers); i++)
		wl_list_init(&m->layers[i]);

	wlr_output_state_init(&state);
	/* Initialize monitor state using configured rules */
	for (r = monrules; r < END(monrules); r++) {
		if (!r->name || strstr(wlr_output->name, r->name)) {
			m->m.x = r->x;
			m->m.y = r->y;
			wlr_output_state_set_scale(&state, r->scale);
			wlr_output_state_set_transform(&state, r->rr);
			break;
		}
	}

	/* The mode is a tuple of (width, height, refresh rate), and each
	 * monitor supports only a specific set of modes. We just pick the
	 * monitor's preferred mode; a more sophisticated compositor would let
	 * the user configure it. */
	wlr_output_state_set_mode(&state, wlr_output_preferred_mode(wlr_output));

	/* Set up event listeners */
	LISTEN(&wlr_output->events.frame, &m->frame, rendermon);
	LISTEN(&wlr_output->events.destroy, &m->destroy, cleanupmon);
	LISTEN(&wlr_output->events.request_state, &m->request_state, requestmonstate);

	wlr_output_state_set_enabled(&state, 1);
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	wl_list_insert(&mons, &m->link);

	/* The xdg-protocol specifies:
	 *
	 * If the fullscreened surface is not opaque, the compositor must make
	 * sure that other screen content not part of the same surface tree (made
	 * up of subsurfaces, popups or similarly coupled surfaces) are not
	 * visible below the fullscreened surface.
	 *
	 */
	/* updatemons() will resize and set correct position */
	m->fullscreen_bg = wlr_scene_rect_create(layers[LyrFS], 0, 0, fullscreen_bg);
	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, 0);

	/* Adds this to the output layout in the order it was configured.
	 *
	 * The output layout utility automatically adds a wl_output global to the
	 * display, which Wayland clients can see to find out information about the
	 * output (such as DPI, scale factor, manufacturer, etc).
	 */
	m->scene_output = wlr_scene_output_create(scene, wlr_output);
	if (m->m.x == -1 && m->m.y == -1)
		wlr_output_layout_add_auto(output_layout, wlr_output);
	else
		wlr_output_layout_add(output_layout, wlr_output, m->m.x, m->m.y);
}

void
createnotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client creates a new toplevel (application window). */
	struct wlr_xdg_toplevel *toplevel = data;
	Client *c = NULL;

	/* Allocate a Client for this surface */
	c = toplevel->base->data = ecalloc(1, sizeof(*c));
	c->surface.xdg = toplevel->base;
	c->bw = borderpx;

	LISTEN(&toplevel->base->surface->events.client_commit, &c->precommit,
			precommitnotify);
	LISTEN(&toplevel->base->surface->events.commit, &c->commit, commitnotify);
	LISTEN(&toplevel->base->surface->events.map, &c->map, mapnotify);
	LISTEN(&toplevel->base->surface->events.unmap, &c->unmap, unmapnotify);
	LISTEN(&toplevel->events.destroy, &c->destroy, destroynotify);
	LISTEN(&toplevel->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&toplevel->events.request_maximize, &c->maximize, maximizenotify);
	LISTEN(&toplevel->events.set_title, &c->set_title, updatetitle);
}

void
createpointer(struct wlr_pointer *pointer)
{
	struct libinput_device *device;
	if (wlr_input_device_is_libinput(&pointer->base)
			&& (device = wlr_libinput_get_device_handle(&pointer->base))) {

		if (libinput_device_config_tap_get_finger_count(device)) {
			libinput_device_config_tap_set_enabled(device, tap_to_click);
			libinput_device_config_tap_set_drag_enabled(device, tap_and_drag);
			libinput_device_config_tap_set_drag_lock_enabled(device, drag_lock);
			libinput_device_config_tap_set_button_map(device, button_map);
		}

		if (libinput_device_config_scroll_has_natural_scroll(device))
			libinput_device_config_scroll_set_natural_scroll_enabled(device, natural_scrolling);

		if (libinput_device_config_dwt_is_available(device))
			libinput_device_config_dwt_set_enabled(device, disable_while_typing);

		if (libinput_device_config_left_handed_is_available(device))
			libinput_device_config_left_handed_set(device, left_handed);

		if (libinput_device_config_middle_emulation_is_available(device))
			libinput_device_config_middle_emulation_set_enabled(device, middle_button_emulation);

		if (libinput_device_config_scroll_get_methods(device) != LIBINPUT_CONFIG_SCROLL_NO_SCROLL)
			libinput_device_config_scroll_set_method(device, scroll_method);

		if (libinput_device_config_click_get_methods(device) != LIBINPUT_CONFIG_CLICK_METHOD_NONE)
			libinput_device_config_click_set_method(device, click_method);

		if (libinput_device_config_send_events_get_modes(device))
			libinput_device_config_send_events_set_mode(device, send_events_mode);

		if (libinput_device_config_accel_is_available(device)) {
			libinput_device_config_accel_set_profile(device, accel_profile);
			libinput_device_config_accel_set_speed(device, accel_speed);
		}
	}

	wlr_cursor_attach_input_device(cursor, &pointer->base);
}

void
createpointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = ecalloc(1, sizeof(*pointer_constraint));
	pointer_constraint->constraint = data;
	LISTEN(&pointer_constraint->constraint->events.destroy,
			&pointer_constraint->destroy, destroypointerconstraint);
}

void
createpopup(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client (either xdg-shell or layer-shell)
	 * creates a new popup. */
	struct wlr_xdg_popup *popup = data;
	LISTEN_STATIC(&popup->base->surface->events.commit, commitpopup);
}

void
cursorconstrain(struct wlr_pointer_constraint_v1 *constraint)
{
	if (active_constraint == constraint)
		return;

	if (active_constraint)
		wlr_pointer_constraint_v1_send_deactivated(active_constraint);

	active_constraint = constraint;
	wlr_pointer_constraint_v1_send_activated(constraint);
}

void
cursorframe(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a frame
	 * event. Frame events are sent after regular pointer events to group
	 * multiple events together. For instance, two axis events may happen at the
	 * same time, in which case a frame event won't be sent in between. */
	/* Notify the client with pointer focus of the frame event. */
	wlr_seat_pointer_notify_frame(seat);
}

void
cursorwarptohint(void)
{
	Client *c = NULL;
	double sx = active_constraint->current.cursor_hint.x;
	double sy = active_constraint->current.cursor_hint.y;

	toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
	if (c && active_constraint->current.cursor_hint.enabled) {
		wlr_cursor_warp(cursor, NULL, sx + c->geom.x + c->bw, sy + c->geom.y + c->bw);
		wlr_seat_pointer_warp(active_constraint->seat, sx, sy);
	}
}

void
destroydecoration(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, destroy_decoration);

	wl_list_remove(&c->destroy_decoration.link);
	wl_list_remove(&c->set_decoration_mode.link);
}

void
destroydragicon(struct wl_listener *listener, void *data)
{
	/* Focus enter isn't sent during drag, so refocus the focused node. */
	focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroyidleinhibitor(struct wl_listener *listener, void *data)
{
	/* `data` is the wlr_surface of the idle inhibitor being destroyed,
	 * at this point the idle inhibitor is still in the list of the manager */
	checkidleinhibitor(wlr_surface_get_root_surface(data));
	wl_list_remove(&listener->link);
	free(listener);
}

void
destroylayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, destroy);

	wl_list_remove(&l->link);
	wl_list_remove(&l->destroy.link);
	wl_list_remove(&l->unmap.link);
	wl_list_remove(&l->surface_precommit.link);
	wl_list_remove(&l->surface_commit.link);
	wlr_scene_node_destroy(&l->scene->node);
	wlr_scene_node_destroy(&l->popups->node);
	free(l);
}

void
destroylock(SessionLock *lock, int unlock)
{
	wlr_seat_keyboard_notify_clear_focus(seat);
	if ((locked = !unlock))
		goto destroy;

	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	focusclient(focustop(selmon), 0);
	motionnotify(0, NULL, 0, 0, 0, 0);

destroy:
	wl_list_remove(&lock->new_surface.link);
	wl_list_remove(&lock->unlock.link);
	wl_list_remove(&lock->destroy.link);

	wlr_scene_node_destroy(&lock->scene->node);
	cur_lock = NULL;
	free(lock);
}

void
destroylocksurface(struct wl_listener *listener, void *data)
{
	Monitor *m = wl_container_of(listener, m, destroy_lock_surface);
	struct wlr_session_lock_surface_v1 *surface, *lock_surface = m->lock_surface;

	m->lock_surface = NULL;
	wl_list_remove(&m->destroy_lock_surface.link);

	if (lock_surface->surface != seat->keyboard_state.focused_surface)
		return;

	if (locked && cur_lock && !wl_list_empty(&cur_lock->surfaces)) {
		surface = wl_container_of(cur_lock->surfaces.next, surface, link);
		client_notify_enter(surface->surface, wlr_seat_get_keyboard(seat));
	} else if (!locked) {
		focusclient(focustop(selmon), 1);
	} else {
		wlr_seat_keyboard_clear_focus(seat);
	}
}

void
destroynotify(struct wl_listener *listener, void *data)
{
	/* Called when the xdg_toplevel is destroyed. */
	Client *c = wl_container_of(listener, c, destroy);
	wl_list_remove(&c->destroy.link);
	wl_list_remove(&c->set_title.link);
	wl_list_remove(&c->fullscreen.link);
#ifdef XWAYLAND
	if (c->type != XDGShell) {
		wl_list_remove(&c->activate.link);
		wl_list_remove(&c->associate.link);
		wl_list_remove(&c->configure.link);
		wl_list_remove(&c->dissociate.link);
	} else
#endif
	{
		wl_list_remove(&c->precommit.link);
		wl_list_remove(&c->commit.link);
		wl_list_remove(&c->map.link);
		wl_list_remove(&c->unmap.link);
		wl_list_remove(&c->maximize.link);
	}
	free(c);
}

void
destroypointerconstraint(struct wl_listener *listener, void *data)
{
	PointerConstraint *pointer_constraint = wl_container_of(listener, pointer_constraint, destroy);

	if (active_constraint == pointer_constraint->constraint) {
		cursorwarptohint();
		active_constraint = NULL;
	}

	wl_list_remove(&pointer_constraint->destroy.link);
	free(pointer_constraint);
}

void
destroysessionlock(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, destroy);
	destroylock(lock, 0);
}

void
destroykeyboardgroup(struct wl_listener *listener, void *data)
{
	KeyboardGroup *group = wl_container_of(listener, group, destroy);
	if (super_group == group)
		super_group = NULL;
	wl_event_source_remove(group->key_repeat_source);
	wl_list_remove(&group->key.link);
	wl_list_remove(&group->modifiers.link);
	wl_list_remove(&group->destroy.link);
	wlr_keyboard_group_destroy(group->wlr_group);
	free(group);
}

Client *
dirpick(Monitor *m, Client *from, int dir)
{
	Client *n, *best = NULL;
	long best_primary = 0, best_dist = 0;
	int fx1, fy1, fx2, fy2, fcx, fcy;

	if (!from)
		return NULL;
	fx1 = from->geom.x;
	fy1 = from->geom.y;
	fx2 = from->geom.x + from->geom.width;
	fy2 = from->geom.y + from->geom.height;
	fcx = from->geom.x + from->geom.width / 2;
	fcy = from->geom.y + from->geom.height / 2;

	wl_list_for_each(n, &clients, link) {
		int ncx, ncy, dx, dy, primary, o1, o2, overlap, valid;
		long dist;
		if (n == from || !VISIBLEON(n, m) || n->isfloating)
			continue;
		ncx = n->geom.x + n->geom.width / 2;
		ncy = n->geom.y + n->geom.height / 2;
		dx = ncx - fcx;
		dy = ncy - fcy;

		if (dir == DirLeft || dir == DirRight) {
			valid = dir == DirLeft ? dx < 0 : dx > 0;
			primary = dx < 0 ? -dx : dx;
			o1 = MAX(fy1, n->geom.y);
			o2 = MIN(fy2, n->geom.y + n->geom.height);
		} else {
			valid = dir == DirUp ? dy < 0 : dy > 0;
			primary = dy < 0 ? -dy : dy;
			o1 = MAX(fx1, n->geom.x);
			o2 = MIN(fx2, n->geom.x + n->geom.width);
		}
		overlap = o2 - o1;

		if (!valid || overlap <= 0)
			continue;
		dist = (long)dx * dx + (long)dy * dy;
		if (!best || primary < best_primary ||
				(primary == best_primary && dist < best_dist)) {
			best = n;
			best_primary = primary;
			best_dist = dist;
		}
	}
	return best;
}

void
entermode(const Arg *arg)
{
	Client *sel;
	inputmode = arg->i;
	wlr_log(WLR_DEBUG, "mode: %s", inputmode == ModeNormal ? "normal" : "insert");
	if ((sel = focustop(selmon)))
		client_set_border_color(sel, focuscolorfor());
}

const float *
focuscolorfor(void)
{
	return inputmode == ModeNormal ? normalmodecolor : focuscolor;
}

void
focusclient(Client *c, int lift)
{
	struct wlr_surface *old = seat->keyboard_state.focused_surface;
	int unused_lx, unused_ly, old_client_type;
	Client *old_c = NULL;
	LayerSurface *old_l = NULL;

	if (locked)
		return;

	/* Raise client in stacking order if requested */
	if (c && lift)
		wlr_scene_node_raise_to_top(&c->scene->node);

	if (c && client_surface(c) == old)
		return;

	if ((old_client_type = toplevel_from_wlr_surface(old, &old_c, &old_l)) == XDGShell) {
		struct wlr_xdg_popup *popup, *tmp;
		wl_list_for_each_safe(popup, tmp, &old_c->surface.xdg->popups, link)
			wlr_xdg_popup_destroy(popup);
	}

	/* Put the new client atop the focus stack and select its monitor */
	if (c && !client_is_unmanaged(c)) {
		wl_list_remove(&c->flink);
		wl_list_insert(&fstack, &c->flink);
		selmon = c->mon;
		if (c->ftl)
			wlr_foreign_toplevel_handle_v1_set_activated(c->ftl, 1);

		/* Don't change border color if there is an exclusive focus or we are
		 * handling a drag operation */
		if (!exclusive_focus && !seat->drag)
			client_set_border_color(c, focuscolorfor());
		if (!exclusive_focus && !seat->drag)
			client_set_border_enabled(c, !decorhidden && !c->isfullscreen
					&& !c->isfakefull);
	}

	/* Deactivate old client if focus is changing */
	if (old && (!c || client_surface(c) != old)) {
		/* If an overlay is focused, don't focus or activate the client,
		 * but only update its position in fstack to render its border with focuscolor
		 * and focus it after the overlay is closed. */
		if (old_client_type == LayerShell && wlr_scene_node_coords(
					&old_l->scene->node, &unused_lx, &unused_ly)
				&& old_l->layer_surface->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
			return;
		} else if (old_c && old_c == exclusive_focus && client_wants_focus(old_c)) {
			return;
		/* Don't deactivate old client if the new one wants focus, as this causes issues with winecfg
		 * and probably other clients */
		} else if (old_c && !client_is_unmanaged(old_c) && (!c || !client_wants_focus(c))) {
			client_set_border_color(old_c, bordercolor);
			client_set_border_enabled(old_c, !decorhidden && unfocused_borders
					&& !old_c->isfullscreen && !old_c->isfakefull);
			if (old_c->ftl)
				wlr_foreign_toplevel_handle_v1_set_activated(old_c->ftl, 0);

			client_activate_surface(old, 0);
		}
	}
	ipcnotifyall();

	if (!c) {
		/* With no client, all we have left is to clear focus */
		wlr_seat_keyboard_notify_clear_focus(seat);
		return;
	}

	/* Change cursor surface */
	motionnotify(0, NULL, 0, 0, 0, 0);

	/* Have a client, so focus its top-level wlr_surface */
	client_notify_enter(client_surface(c), wlr_seat_get_keyboard(seat));

	/* Activate the new client */
	client_activate_surface(client_surface(c), 1);
}

void
focusdir(const Arg *arg)
{
	Client *sel = focustop(selmon), *c;
	if (selmon && (!sel || sel->isfullscreen || sel->isfakefull
			|| !(c = dirpick(selmon, sel, arg->i)))) {
		/* nothing in that direction: step to the neighbouring workspace */
		if (arg->i == DirLeft || arg->i == DirRight) {
			Arg a = {.i = arg->i == DirLeft ? -1 : +1};
			wsstep(&a);
		}
		return;
	}
	if (!selmon)
		return;
	focusclient(c, 1);
	warpto(c);
}

void
focusnext(const Arg *arg)
{
	Client *sel = focustop(selmon);
	Node *n;
	if (!sel || sel->isfullscreen || sel->isfakefull || !sel->node || !selmon)
		return;
	n = bsp_nextleaf(selmon->tree[sel->ws], sel->node);
	if (n && n->c != sel) {
		focusclient(n->c, 1);
		warpto(n->c);
	}
}

/* We probably should change the name of this: it sounds like it
 * will focus the topmost client of this mon, when actually will
 * only return that client */
Client *
focustop(Monitor *m)
{
	Client *c;
	wl_list_for_each(c, &fstack, flink) {
		if (VISIBLEON(c, m))
			return c;
	}
	return NULL;
}

void
ftlactivatenotify(struct wl_listener *listener, void *data)
{
	/* A taskbar/dock asked to focus this window */
	Client *c = wl_container_of(listener, c, ftl_activate);
	Arg a;
	if (!c->mon)
		return;
	selmon = c->mon;
	a.ui = c->ws;
	viewws(&a);
	focusclient(c, 1);
}

void
ftlclosenotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, ftl_close);
	clientcloseanim(c);
	client_send_close(c);
}

void
fullscreennotify(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, fullscreen);
	setfullscreen(c, client_wants_fullscreen(c));
}

void
gpureset(struct wl_listener *listener, void *data)
{
	struct wlr_renderer *old_drw = drw;
	struct wlr_allocator *old_alloc = alloc;
	struct Monitor *m;
	if (!(drw = fx_renderer_create(backend)))
		die("couldn't recreate renderer");

	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't recreate allocator");

	wl_list_remove(&gpu_reset.link);
	wl_signal_add(&drw->events.lost, &gpu_reset);

	wlr_compositor_set_renderer(compositor, drw);

	wl_list_for_each(m, &mons, link) {
		wlr_output_init_render(m->wlr_output, alloc, drw);
	}

	wlr_allocator_destroy(old_alloc);
	wlr_renderer_destroy(old_drw);
}

void
handlesig(int signo)
{
	if (signo == SIGCHLD) {
		while (waitpid(-1, NULL, WNOHANG) > 0);
	} else if (signo == SIGINT || signo == SIGTERM) {
		/* The session wrapper records the clean exit.  wlr_log() is not
		 * async-signal-safe, so it must not be called from this handler. */
		quit(NULL);
	}
}

static void
handlecrash(int signo)
{
	/* A signal can interrupt malloc/stdio/libwayland.  Do not call into any
	 * of those from here: write(), signal() and raise() are async-signal-safe.
	 * The session wrapper records the resulting exit status, while the reset
	 * disposition below lets the kernel produce the actual core dump. */
	static const char msg[] = "gluewc: fatal signal; core dump follows\n";
	if (write(STDERR_FILENO, msg, sizeof(msg) - 1) < 0)
		{}
	signal(signo, SIG_DFL);
	raise(signo);
}

void
inputdevice(struct wl_listener *listener, void *data)
{
	/* This event is raised by the backend when a new input device becomes
	 * available. */
	struct wlr_input_device *device = data;
	uint32_t caps;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		createkeyboard(wlr_keyboard_from_input_device(device));
		break;
	case WLR_INPUT_DEVICE_POINTER:
		createpointer(wlr_pointer_from_input_device(device));
		break;
	default:
		/* TODO handle other input device types */
		break;
	}

	/* We need to let the wlr_seat know what our capabilities are, which is
	 * communiciated to the client. In gluewc we always have a cursor, even if
	 * there are no pointer devices, so we always include that capability. */
	/* TODO do we actually require a cursor? */
	caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&kb_group->wlr_group->devices))
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(seat, caps);
}

/* --- dwl-ipc-unstable-v2: status/tag info for bars (mmsg, waybar) --- */
static void
ipcoutputdestroy(struct wl_resource *resource)
{
	IpcOutput *io = wl_resource_get_user_data(resource);
	if (io) {
		wl_list_remove(&io->link);
		free(io);
	}
}

static void
ipcrelease(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static void
ipcsettags(struct wl_client *client, struct wl_resource *resource,
		uint32_t tagmask, uint32_t toggle_tagset)
{
	IpcOutput *io = wl_resource_get_user_data(resource);
	Arg a;
	int i;
	if (!io || !io->mon || !tagmask)
		return;
	for (i = 0; i < NUMWS && !(tagmask & (1u << i)); i++);
	if (i >= NUMWS)
		return;
	selmon = io->mon;
	a.ui = (uint32_t)i;
	viewws(&a);
}

static void
ipcsetclienttags(struct wl_client *client, struct wl_resource *resource,
		uint32_t and_tags, uint32_t xor_tags)
{
	IpcOutput *io = wl_resource_get_user_data(resource);
	Client *sel;
	uint32_t newtags;
	Arg a;
	int i;
	if (!io || !io->mon)
		return;
	selmon = io->mon;
	if (!(sel = focustop(selmon)))
		return;
	newtags = ((1u << sel->ws) & and_tags) ^ xor_tags;
	if (!newtags)
		return;
	for (i = 0; i < NUMWS && !(newtags & (1u << i)); i++);
	if (i >= NUMWS)
		return;
	a.ui = (uint32_t)i;
	movetows(&a);
}

static void
ipcsetlayout(struct wl_client *client, struct wl_resource *resource, uint32_t index)
{
}

static void
ipcquit(struct wl_client *client, struct wl_resource *resource)
{
	pid_t pid = 0;
	wl_client_get_credentials(client, &pid, NULL, NULL);
	wlr_log(WLR_ERROR, "exiting: ipc quit from client pid %d", pid);
	quit(NULL);
}

static void
ipcdispatch(struct wl_client *client, struct wl_resource *resource,
		const char *dispatch, const char *arg1, const char *arg2,
		const char *arg3, const char *arg4, const char *arg5)
{
}

static const struct zdwl_ipc_output_v2_interface ipc_output_impl = {
	.release = ipcrelease,
	.set_tags = ipcsettags,
	.set_client_tags = ipcsetclienttags,
	.set_layout = ipcsetlayout,
	.quit = ipcquit,
	.dispatch = ipcdispatch,
};

void
ipcstatus(IpcOutput *io)
{
	Monitor *m = io->mon;
	Client *c, *sel;
	uint32_t wscnt[NUMWS] = {0};
	const char *title, *appid;
	int i, version;

	if (!m)
		return;
	version = wl_resource_get_version(io->resource);
	sel = focustop(m);
	wl_list_for_each(c, &clients, link) {
		if (c->mon == m && c->ws < NUMWS)
			wscnt[c->ws]++;
	}

	zdwl_ipc_output_v2_send_active(io->resource, m == selmon);
	for (i = 0; i < NUMWS; i++) {
		zdwl_ipc_output_v2_send_tag(io->resource, (uint32_t)i,
				(unsigned int)i == m->ws ? ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE
						: ZDWL_IPC_OUTPUT_V2_TAG_STATE_NONE,
				wscnt[i], sel && sel->ws == (unsigned int)i);
	}
	zdwl_ipc_output_v2_send_layout(io->resource, 0);
	title = sel ? client_get_title(sel) : NULL;
	appid = sel ? client_get_appid(sel) : NULL;
	zdwl_ipc_output_v2_send_title(io->resource, title ? title : "");
	if (version >= ZDWL_IPC_OUTPUT_V2_APPID_SINCE_VERSION)
		zdwl_ipc_output_v2_send_appid(io->resource, appid ? appid : "");
	if (version >= ZDWL_IPC_OUTPUT_V2_LAYOUT_SYMBOL_SINCE_VERSION)
		zdwl_ipc_output_v2_send_layout_symbol(io->resource, "bsp");
	if (version >= ZDWL_IPC_OUTPUT_V2_FULLSCREEN_SINCE_VERSION)
		zdwl_ipc_output_v2_send_fullscreen(io->resource,
				sel && (sel->isfullscreen || sel->isfakefull));
	if (version >= ZDWL_IPC_OUTPUT_V2_FLOATING_SINCE_VERSION)
		zdwl_ipc_output_v2_send_floating(io->resource, sel && sel->isfloating);
	zdwl_ipc_output_v2_send_frame(io->resource);
}

void
ipcnotifyall(void)
{
	IpcOutput *io;
	wl_list_for_each(io, &ipc_outputs, link)
		ipcstatus(io);
}

static void
ipcgetoutput(struct wl_client *client, struct wl_resource *resource,
		uint32_t id, struct wl_resource *output)
{
	struct wlr_output *wlr_output = wlr_output_from_resource(output);
	struct wl_resource *res;
	IpcOutput *io;

	res = wl_resource_create(client, &zdwl_ipc_output_v2_interface,
			wl_resource_get_version(resource), id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	io = ecalloc(1, sizeof(*io));
	io->resource = res;
	io->mon = wlr_output ? wlr_output->data : NULL;
	wl_resource_set_implementation(res, &ipc_output_impl, io, ipcoutputdestroy);
	wl_list_insert(&ipc_outputs, &io->link);
	ipcstatus(io);
}

static const struct zdwl_ipc_manager_v2_interface ipc_manager_impl = {
	.release = ipcrelease,
	.get_output = ipcgetoutput,
};

void
ipcmgrbind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
	struct wl_resource *res = wl_resource_create(client,
			&zdwl_ipc_manager_v2_interface, (int)version, id);
	if (!res) {
		wl_client_post_no_memory(client);
		return;
	}
	wl_resource_set_implementation(res, &ipc_manager_impl, NULL, NULL);
	zdwl_ipc_manager_v2_send_tags(res, NUMWS);
	zdwl_ipc_manager_v2_send_layout(res, "bsp");
}
/* --- end dwl-ipc --- */

static void
overviewclear(void)
{
	struct wlr_scene_node *node, *tmp;
	wl_list_for_each_safe(node, tmp, &overview_dim_scene->children, link)
		wlr_scene_node_destroy(node);
	wl_list_for_each_safe(node, tmp, &overview_panels_scene->children, link)
		wlr_scene_node_destroy(node);
}

static void
overviewdragclear(void)
{
	struct wlr_scene_node *node, *tmp;

	wl_list_for_each_safe(node, tmp, &overview_drag_scene->children, link)
		wlr_scene_node_destroy(node);
	wlr_scene_node_set_position(&overview_drag_scene->node, 0, 0);
}

void
overviewclone(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	OverviewClone *oc = data;
	struct wlr_scene_buffer *clone;
	struct wlr_scene_buffer_set_buffer_options options;
	enum corner_location corners;
	float scale;
	int width, height, radius;

	if (!buffer->buffer)
		return;
	width = buffer->dst_width > 0 ? buffer->dst_width
			: buffer->src_box.width > 0 ? (int)round(buffer->src_box.width)
			: buffer->buffer->width;
	height = buffer->dst_height > 0 ? buffer->dst_height
			: buffer->src_box.height > 0 ? (int)round(buffer->src_box.height)
			: buffer->buffer->height;
	if (width <= 0 || height <= 0)
		return;

	options = (struct wlr_scene_buffer_set_buffer_options){
		.wait_timeline = buffer->WLR_PRIVATE.wait_timeline,
		.wait_point = buffer->WLR_PRIVATE.wait_point,
	};
	clone = wlr_scene_buffer_create(oc->tree, NULL);
	wlr_scene_buffer_set_buffer_with_options(clone, buffer->buffer, &options);
	if (buffer->src_box.width > 0 && buffer->src_box.height > 0)
		wlr_scene_buffer_set_source_box(clone, &buffer->src_box);
	wlr_scene_buffer_set_transform(clone, buffer->transform);
	wlr_scene_buffer_set_dest_size(clone, MAX(1, (int)roundf(width * oc->scalex)),
			MAX(1, (int)roundf(height * oc->scaley)));
	wlr_scene_buffer_set_opacity(clone, buffer->opacity * oc->opacity);
	wlr_scene_buffer_set_filter_mode(clone, WLR_SCALE_FILTER_BILINEAR);
	scale = MIN(oc->scalex, oc->scaley);
	radius = oc->radius >= 0 ? oc->radius
			: MAX(0, (int)roundf(buffer->corner_radius * scale));
	corners = oc->radius >= 0 ? CORNER_LOCATION_ALL : buffer->corners;
	wlr_scene_buffer_set_corner_radius(clone, radius, corners);
	wlr_scene_buffer_set_backdrop_blur(clone, buffer->backdrop_blur);
	wlr_scene_buffer_set_backdrop_blur_optimized(clone,
			buffer->backdrop_blur_optimized);
	wlr_scene_buffer_set_backdrop_blur_ignore_transparent(clone,
			buffer->backdrop_blur_ignore_transparent);
	wlr_scene_buffer_set_opaque_region(clone, &buffer->opaque_region);
	wlr_scene_node_set_position(&clone->node,
			oc->x + (int)roundf((sx - oc->srcx) * oc->scalex),
			oc->y + (int)roundf((sy - oc->srcy) * oc->scaley));
	oc->count++;
}

static struct wlr_box
overviewwindowbox(Monitor *m, Client *c, const struct wlr_box *panel)
{
	struct wlr_box geo = (c->isfullscreen || c->isfakefull) ? c->prev : c->geom;
	float scalex = (float)panel->width / m->w.width;
	float scaley = (float)panel->height / m->w.height;

	if (geo.width <= 0 || geo.height <= 0)
		geo = m->w;
	return (struct wlr_box){
		.x = panel->x + (int)roundf((geo.x - m->w.x) * scalex),
		.y = panel->y + (int)roundf((geo.y - m->w.y) * scaley),
		.width = MAX(4, (int)roundf(geo.width * scalex)),
		.height = MAX(4, (int)roundf(geo.height * scaley)),
	};
}

static void
overviewwindowdraw(struct wlr_scene_tree *tree, Client *c,
		const struct wlr_box *box, float opacity)
{
	float shadowcolor[] = {0.0f, 0.0f, 0.0f, 0.55f * opacity};
	float windowcolor[] = {0.055f * opacity, 0.055f * opacity,
		0.065f * opacity, 0.96f * opacity};
	struct wlr_scene_tree *content;
	struct wlr_scene_rect *back;
	struct wlr_scene_shadow *shadow;
	OverviewClone oc;
	float scale;
	int enabled, radius;

	if (opacity <= 0.0f)
		return;
	scale = MIN((float)box->width / MAX(1, c->geom.width),
			(float)box->height / MAX(1, c->geom.height));
	radius = MAX(3, (int)roundf(MAX(4, corner_radius) * scale));
	shadow = wlr_scene_shadow_create(tree, box->width, box->height,
			radius, 12.0f, shadowcolor);
	wlr_scene_node_set_position(&shadow->node, box->x, box->y + 3);
	content = wlr_scene_tree_create(tree);
	back = wlr_scene_rect_create(content, box->width, box->height, windowcolor);
	wlr_scene_rect_set_corner_radius(back, radius, CORNER_LOCATION_ALL);
	wlr_scene_node_set_position(&back->node, box->x, box->y);

	if (!c->scene)
		return;
	oc = (OverviewClone){
		.tree = content,
		.scalex = (float)box->width / MAX(1, c->geom.width),
		.scaley = (float)box->height / MAX(1, c->geom.height),
		.opacity = opacity,
		.srcx = c->scene->node.x, .srcy = c->scene->node.y,
		.x = box->x, .y = box->y,
		.radius = -1,
	};
	enabled = c->scene->node.enabled;
	if (!enabled)
		wlr_scene_node_set_enabled(&c->scene->node, 1);
	wlr_scene_node_for_each_buffer(&c->scene->node, overviewclone, &oc);
	if (!enabled)
		wlr_scene_node_set_enabled(&c->scene->node, 0);
	if (oc.count)
		wlr_scene_node_destroy(&back->node);
}

static void
overviewwindow(struct wlr_scene_tree *tree, Monitor *m, Client *c,
		const struct wlr_box *panel, float opacity)
{
	struct wlr_box box = overviewwindowbox(m, c, panel);

	overviewwindowdraw(tree, c, &box, opacity);
}

static void
overviewpanel(Monitor *m, unsigned int ws, const struct wlr_box *box, float opacity)
{
	float panelcolor[] = {0.015f * opacity, 0.015f * opacity,
		0.018f * opacity, opacity};
	float shadecolor[] = {0.0f, 0.0f, 0.0f, 0.10f * opacity};
	float shadowcolor[] = {0.0f, 0.0f, 0.0f, 0.62f * opacity};
	struct wlr_scene_tree *tree;
	struct wlr_scene_rect *panel, *shade;
	struct wlr_scene_shadow *shadow;
	LayerSurface *l;
	Client *c;
	OverviewClone oc;

	if (opacity <= 0.0f)
		return;
	tree = wlr_scene_tree_create(overview_panels_scene);
	oc = (OverviewClone){
		.tree = tree,
		.scalex = (float)box->width / m->m.width,
		.scaley = (float)box->height / m->m.height,
		.opacity = opacity,
		.srcx = m->m.x, .srcy = m->m.y,
		.x = box->x, .y = box->y,
		.radius = MAX(6, (int)roundf(16.0f * box->width / m->m.width)),
	};

	shadow = wlr_scene_shadow_create(tree, box->width, box->height,
			oc.radius, 24.0f, shadowcolor);
	wlr_scene_node_set_position(&shadow->node, box->x, box->y + 8);
	panel = wlr_scene_rect_create(tree, box->width, box->height, panelcolor);
	wlr_scene_rect_set_corner_radius(panel, oc.radius, CORNER_LOCATION_ALL);
	wlr_scene_node_set_position(&panel->node, box->x, box->y);

	wl_list_for_each(l, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND], link) {
		if (l->mapped)
			wlr_scene_node_for_each_buffer(&l->scene->node, overviewclone, &oc);
	}
	shade = wlr_scene_rect_create(tree, box->width, box->height, shadecolor);
	wlr_scene_rect_set_corner_radius(shade, oc.radius, CORNER_LOCATION_ALL);
	wlr_scene_node_set_position(&shade->node, box->x, box->y);

	wl_list_for_each_reverse(c, &clients, link) {
		if (c->mon == m && c->ws == ws
				&& (!overview_dragging || c != overview_drag_client))
			overviewwindow(tree, m, c, box, opacity);
	}
}

static int
overviewvalid(Monitor *m)
{
	return m && m->wlr_output->enabled && m->m.width > 0 && m->m.height > 0
			&& m->w.width > 0 && m->w.height > 0;
}

static void
overviewtargets(Monitor *m, struct wlr_box boxes[3])
{
	struct wlr_box *left = &boxes[0];
	struct wlr_box *center = &boxes[1];
	struct wlr_box *right = &boxes[2];
	float scale;
	int gap, margin, width, height, sidewidth, sideheight;

	margin = MAX(24, MIN(64, MIN(m->w.width, m->w.height) / 18));
	gap = MAX(24, margin / 2);
	scale = MIN((m->w.height - 2.0f * margin) / m->m.height,
			(m->w.width * 0.72f) / m->m.width);
	scale = MIN(0.88f, MAX(0.20f, scale));
	width = MAX(1, (int)roundf(m->m.width * scale));
	height = MAX(1, (int)roundf(m->m.height * scale));
	sidewidth = MAX(1, (int)roundf(width * 0.90f));
	sideheight = MAX(1, (int)roundf(height * 0.90f));
	*center = (struct wlr_box){
		.x = m->w.x + (m->w.width - width) / 2,
		.y = m->w.y + (m->w.height - height) / 2,
		.width = width, .height = height,
	};
	*left = *right = (struct wlr_box){
		.y = m->w.y + (m->w.height - sideheight) / 2,
		.width = sidewidth, .height = sideheight,
	};
	left->x = center->x - sidewidth - gap;
	right->x = center->x + width + gap;
}

static struct wlr_box
overviewboxlerp(const struct wlr_box *from, const struct wlr_box *to, float t)
{
	return (struct wlr_box){
		.x = from->x + (int)roundf((to->x - from->x) * t),
		.y = from->y + (int)roundf((to->y - from->y) * t),
		.width = MAX(1, from->width + (int)roundf((to->width - from->width) * t)),
		.height = MAX(1, from->height + (int)roundf((to->height - from->height) * t)),
	};
}

static void
overviewtransition(Monitor *m, const struct wlr_box boxes[3],
		const float opacity[3], float dim)
{
	int i;

	for (i = 0; i < 3; i++) {
		m->overview_from[i] = m->overview[i];
		m->overview_to[i] = boxes[i];
		m->overview_opacity_from[i] = m->overview_opacity[i];
		m->overview_opacity_to[i] = opacity[i];
	}
	m->overview_dim_from = m->overview_dim;
	m->overview_dim_to = dim;
	if (animations && animation_duration > 0) {
		m->overview_anim_t = 0.0f;
		m->overview_animating = 1;
		animkick(m);
	} else {
		for (i = 0; i < 3; i++) {
			m->overview[i] = boxes[i];
			m->overview_opacity[i] = opacity[i];
		}
		m->overview_dim = dim;
		m->overview_animating = 0;
	}
}

static int
overviewadvance(Monitor *m, float dt)
{
	float e;
	int i;

	if (!overview_visible || !m->overview_animating)
		return 0;
	m->overview_anim_t += dt / (float)MAX(180, animation_duration * 3 / 2);
	if (m->overview_anim_t >= 1.0f) {
		m->overview_anim_t = 1.0f;
		m->overview_animating = 0;
	}
	e = 1.0f - powf(1.0f - m->overview_anim_t, 3.0f);
	for (i = 0; i < 3; i++) {
		m->overview[i] = overviewboxlerp(&m->overview_from[i],
				&m->overview_to[i], e);
		m->overview_opacity[i] = m->overview_opacity_from[i]
				+ (m->overview_opacity_to[i] - m->overview_opacity_from[i]) * e;
	}
	m->overview_dim = m->overview_dim_from
			+ (m->overview_dim_to - m->overview_dim_from) * e;
	return 1;
}

static int
overviewallidle(void)
{
	Monitor *m;

	wl_list_for_each(m, &mons, link) {
		if (overviewvalid(m) && m->overview_animating)
			return 0;
	}
	return 1;
}

static void
overviewfinish(void)
{
	Client *c, *focus = NULL;
	Monitor *m;

	if (overview_active || !overview_visible || !overviewallidle())
		return;
	overview_visible = 0;
	wlr_scene_node_set_enabled(&overview_scene->node, 0);
	wlr_scene_node_set_enabled(&overview_dim_scene->node, 0);
	wlr_scene_node_set_enabled(&layers[LyrTile]->node, 1);
	wlr_scene_node_set_enabled(&layers[LyrFloat]->node, 1);
	wlr_scene_node_set_enabled(&layers[LyrFS]->node, 1);
	wl_list_for_each(m, &mons, link)
		arrange(m);
	if (overview_focus_client) {
		wl_list_for_each(c, &clients, link) {
			if (c == overview_focus_client && VISIBLEON(c, selmon)) {
				focus = c;
				break;
			}
		}
	}
	if (!focus)
		focus = focustop(selmon);
	overview_focus_client = NULL;
	focusclient(focus, 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
overviewbuild(void)
{
	Monitor *m;

	overviewclear();
	wl_list_for_each(m, &mons, link) {
		float dimcolor[] = {0.06f * m->overview_dim,
			0.06f * m->overview_dim, 0.065f * m->overview_dim,
			0.30f * m->overview_dim};
		struct wlr_scene_rect *dim;

		if (!overviewvalid(m))
			continue;
		dim = wlr_scene_rect_create(overview_dim_scene,
				m->m.width, m->m.height, dimcolor);
		wlr_scene_node_set_position(&dim->node, m->m.x, m->m.y);
		overviewpanel(m, (m->ws + NUMWS - 1) % NUMWS,
				&m->overview[0], m->overview_opacity[0]);
		overviewpanel(m, (m->ws + 1) % NUMWS,
				&m->overview[2], m->overview_opacity[2]);
		overviewpanel(m, m->ws, &m->overview[1], m->overview_opacity[1]);
		wlr_output_schedule_frame(m->wlr_output);
	}
}

static void
overviewrelayout(void)
{
	struct wlr_box target[3];
	Monitor *m;
	int i;

	if (!overview_visible)
		return;
	wl_list_for_each(m, &mons, link) {
		if (!overviewvalid(m))
			continue;
		overviewtargets(m, target);
		for (i = 0; i < 3; i++) {
			m->overview[i] = target[i];
			m->overview_opacity[i] = 1.0f;
		}
		m->overview_dim = 1.0f;
		m->overview_animating = 0;
	}
	overviewbuild();
}

static int
overviewpanelat(Monitor *m, double x, double y)
{
	int i;

	for (i = 0; i < 3; i++) {
		if (wlr_box_contains_point(&m->overview[i], x, y))
			return i;
	}
	return -1;
}

static unsigned int
overviewpanelws(Monitor *m, int panel)
{
	if (panel == 0)
		return (m->ws + NUMWS - 1) % NUMWS;
	if (panel == 2)
		return (m->ws + 1) % NUMWS;
	return m->ws;
}

static Client *
overviewclientat(Monitor *m, double x, double y, int *panel,
		struct wlr_box *box)
{
	Client *c;
	unsigned int ws;
	int p = overviewpanelat(m, x, y);

	if (p < 0)
		return NULL;
	ws = overviewpanelws(m, p);
	wl_list_for_each(c, &clients, link) {
		struct wlr_box b;
		if (c->mon != m || c->ws != ws)
			continue;
		b = overviewwindowbox(m, c, &m->overview[p]);
		if (!wlr_box_contains_point(&b, x, y))
			continue;
		if (panel)
			*panel = p;
		if (box)
			*box = b;
		return c;
	}
	return NULL;
}

static void
overviewmovetows(Client *c, Monitor *m, unsigned int ws)
{
	if (c->mon != m) {
		setmon(c, m, (int)ws);
	} else if (c->ws != ws) {
		bsp_detach(c);
		bsp_attach(m, ws, c);
		arrange(m);
	}
}

static void
overviewbutton(struct wlr_pointer_button_event *event)
{
	Monitor *m;
	Client *c;
	struct wlr_box box;
	int panel;

	if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		overview_button_swallow = 1;
		if (!overview_active || event->button != BTN_LEFT)
			return;
		m = xytomon(cursor->x, cursor->y);
		if (!overviewvalid(m) || m->overview_animating)
			return;
		selmon = m;
		c = overviewclientat(m, cursor->x, cursor->y, &panel, &box);
		if (c) {
			overview_drag_client = c;
			overview_drag_box = box;
			overview_press_x = cursor->x;
			overview_press_y = cursor->y;
			overview_grab_x = (int)round(cursor->x) - box.x;
			overview_grab_y = (int)round(cursor->y) - box.y;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "grab");
			return;
		}
		panel = overviewpanelat(m, cursor->x, cursor->y);
		if (panel == 0)
			overviewnavigate((m->ws + NUMWS - 1) % NUMWS, -1);
		else if (panel == 2)
			overviewnavigate((m->ws + 1) % NUMWS, 1);
		else if (panel == 1)
			overviewset(0);
		return;
	}

	if (overview_drag_client) {
		c = overview_drag_client;
		m = xytomon(cursor->x, cursor->y);
		if (overview_dragging && overviewvalid(m)
				&& (panel = overviewpanelat(m, cursor->x, cursor->y)) >= 0)
			overviewmovetows(c, m, overviewpanelws(m, panel));
		if (!overview_dragging) {
			selmon = c->mon;
			if (selmon->ws != c->ws) {
				Arg a = {.ui = c->ws};
				viewws(&a);
			}
			overview_focus_client = c;
		}
		overview_drag_client = NULL;
		overview_dragging = 0;
		overviewdragclear();
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");
		if (overview_active) {
			overviewbuild();
			if (overview_focus_client)
				overviewset(0);
		}
	}
	overview_button_swallow = 0;
}

static int
overviewmotion(void)
{
	Monitor *m;
	Client *c;

	if (!overview_visible)
		return 0;
	if (!overview_active)
		return 1;
	if (overview_drag_client) {
		if (!overview_dragging && (fabs(cursor->x - overview_press_x) >= 7.0
				|| fabs(cursor->y - overview_press_y) >= 7.0)) {
			overview_dragging = 1;
			overviewdragclear();
			overview_drag_box.x = overview_drag_box.y = 0;
			overviewwindowdraw(overview_drag_scene, overview_drag_client,
					&overview_drag_box, 0.96f);
			overviewbuild();
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "grabbing");
		}
		if (overview_dragging)
			wlr_scene_node_set_position(&overview_drag_scene->node,
					(int)round(cursor->x) - overview_grab_x,
					(int)round(cursor->y) - overview_grab_y);
		return 1;
	}
	m = xytomon(cursor->x, cursor->y);
	c = overviewvalid(m) && !m->overview_animating
			? overviewclientat(m, cursor->x, cursor->y, NULL, NULL) : NULL;
	wlr_cursor_set_xcursor(cursor, cursor_mgr, c ? "grab" : "default");
	return 1;
}

void
overviewset(int active)
{
	static const float shown[] = {1.0f, 1.0f, 1.0f};
	static const float closing[] = {0.0f, 1.0f, 0.0f};
	struct wlr_box target[3], end[3];
	Monitor *m;
	int i;

	active = !!active;
	if (active == overview_active || (active && (locked || !selmon)))
		return;
	overview_active = active;
	overview_axis_dx = overview_axis_dy = 0.0;
	overview_axis_triggered = 0;
	overview_swipe_active = overview_swipe_triggered = 0;
	overview_swipe_dx = overview_swipe_dy = 0.0;
	if (active) {
		overview_focus_client = NULL;
		if (!overview_visible) {
			overview_visible = 1;
			focusclient(NULL, 0);
			wlr_seat_pointer_notify_clear_focus(seat);
			wlr_scene_node_set_enabled(&layers[LyrTile]->node, 0);
			wlr_scene_node_set_enabled(&layers[LyrFloat]->node, 0);
			wlr_scene_node_set_enabled(&layers[LyrFS]->node, 0);
			wlr_scene_node_set_enabled(&overview_dim_scene->node, 1);
			wlr_scene_node_set_enabled(&overview_scene->node, 1);
			wl_list_for_each(m, &mons, link) {
				if (!overviewvalid(m))
					continue;
				overviewtargets(m, target);
				m->overview[0] = target[0];
				m->overview[0].x -= m->m.width;
				m->overview[1] = m->m;
				m->overview[2] = target[2];
				m->overview[2].x += m->m.width;
				m->overview_opacity[0] = 0.0f;
				m->overview_opacity[1] = 1.0f;
				m->overview_opacity[2] = 0.0f;
				m->overview_dim = 0.0f;
				overviewtransition(m, target, shown, 1.0f);
			}
		} else {
			wl_list_for_each(m, &mons, link) {
				if (!overviewvalid(m))
					continue;
				overviewtargets(m, target);
				overviewtransition(m, target, shown, 1.0f);
			}
		}
	} else {
		overview_drag_client = NULL;
		overview_dragging = 0;
		overviewdragclear();
		wl_list_for_each(m, &mons, link) {
			if (!overviewvalid(m))
				continue;
			for (i = 0; i < 3; i++)
				end[i] = m->overview[i];
			end[0].x -= m->m.width;
			end[1] = m->w;
			end[2].x += m->m.width;
			overviewtransition(m, end, closing, 0.0f);
		}
	}
	overviewbuild();
	overviewfinish();
}

void
overviewtoggle(const Arg *arg)
{
	overviewset(!overview_active);
}

static void
workspacestep(int dir)
{
	Arg a = {.i = dir};
	Monitor *m = xytomon(cursor->x, cursor->y);

	if (overviewvalid(m))
		selmon = m;
	if (!overviewvalid(selmon) || !dir)
		return;
	if (overview_active)
		overviewnavigate((selmon->ws + NUMWS + dir) % NUMWS, dir);
	else if (!overview_visible)
		wsstep(&a);
}

static void
overviewnavigate(unsigned int ws, int dir)
{
	static const float shown[] = {1.0f, 1.0f, 1.0f};
	struct wlr_box old[3], target[3];
	float oldopacity[3];
	Arg a = {.ui = ws};
	Monitor *m = selmon;
	unsigned int oldws, adjacent;
	int i, distance;

	if (!overview_active || !overviewvalid(m) || m->ws == ws)
		return;
	oldws = m->ws;
	adjacent = dir > 0 ? (oldws + 1) % NUMWS : (oldws + NUMWS - 1) % NUMWS;
	for (i = 0; i < 3; i++) {
		old[i] = m->overview[i];
		oldopacity[i] = m->overview_opacity[i];
	}
	viewws(&a);
	overviewtargets(m, target);
	if (ws != adjacent) {
		for (i = 0; i < 3; i++) {
			m->overview[i] = target[i];
			m->overview_opacity[i] = 0.0f;
		}
		overviewtransition(m, target, shown, 1.0f);
		overviewbuild();
		return;
	}
	if (dir > 0) {
		m->overview[0] = old[1];
		m->overview[1] = old[2];
		m->overview[2] = target[2];
		distance = old[2].x - old[1].x;
		m->overview[2].x = old[2].x + distance;
		m->overview_opacity[0] = oldopacity[1];
		m->overview_opacity[1] = oldopacity[2];
		m->overview_opacity[2] = 0.0f;
	} else {
		m->overview[2] = old[1];
		m->overview[1] = old[0];
		m->overview[0] = target[0];
		distance = old[1].x - old[0].x;
		m->overview[0].x = old[0].x - distance;
		m->overview_opacity[2] = oldopacity[1];
		m->overview_opacity[1] = oldopacity[0];
		m->overview_opacity[0] = 0.0f;
	}
	overviewtransition(m, target, shown, 1.0f);
	overviewbuild();
}

int
overviewkey(xkb_keysym_t sym)
{
	sym = xkb_keysym_to_lower(sym);
	if (sym == XKB_KEY_Escape || sym == XKB_KEY_Return) {
		overviewset(0);
	} else if (sym == XKB_KEY_Left || sym == XKB_KEY_h) {
		overviewnavigate((selmon->ws + NUMWS - 1) % NUMWS, -1);
	} else if (sym == XKB_KEY_Right || sym == XKB_KEY_l) {
		overviewnavigate((selmon->ws + 1) % NUMWS, 1);
	} else if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
		unsigned int ws = (uint32_t)(sym - XKB_KEY_1);
		overviewnavigate(ws, ws > selmon->ws ? 1 : -1);
	}
	return 1;
}

int
keybinding(uint32_t mods, xkb_keysym_t sym)
{
	/*
	 * Here we handle compositor keybindings. This is when the compositor is
	 * processing keys, rather than passing them on to the client for its own
	 * processing.
	 */
	int handled = 0;
	const Bind *b;
	for (b = runkeys; b < runkeys + nrunkeys; b++) {
		if (CLEANMASK(mods) == CLEANMASK(b->mod)
				&& xkb_keysym_to_lower(sym) == xkb_keysym_to_lower(b->keysym)
				&& b->func) {
			b->func(&b->arg);
			handled = 1;
			break;
		}
	}
	if (inputmode == ModeNormal) {
		if (!handled) {
			for (b = runnormalkeys; b < runnormalkeys + nrunnormalkeys; b++) {
				if (sym == b->keysym && b->func) {
					b->func(&b->arg);
					break;
				}
			}
		}
		return 1; /* normal mode swallows everything, like nvwm's keyboard grab */
	}
	return handled;
}

void
keypress(struct wl_listener *listener, void *data)
{
	int i, super;
	/* This event is raised when a key is pressed or released. */
	KeyboardGroup *group = wl_container_of(listener, group, key);
	struct wlr_keyboard_key_event *event = data;

	/* Translate libinput keycode -> xkbcommon */
	uint32_t keycode = event->keycode + 8;
	/* Get a list of keysyms based on the keymap for this keyboard */
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
			group->wlr_group->keyboard.xkb_state, keycode, &syms);

	int handled = 0;
	uint32_t mods = wlr_keyboard_get_modifiers(&group->wlr_group->keyboard);
	super = event->keycode == KEY_LEFTMETA || event->keycode == KEY_RIGHTMETA;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		if (super) {
			group->super_down = group->super_alone = 1;
			super_group = group;
			handled = 1;
		} else if (group->super_down) {
			group->super_alone = 0;
		}
	} else if (!locked && event->state == WL_KEYBOARD_KEY_STATE_RELEASED
			&& super && group->super_down) {
		if (group->super_alone)
			overviewtoggle(NULL);
		group->super_down = group->super_alone = 0;
		if (super_group == group)
			super_group = NULL;
		handled = 1;
	}

	/* On _press_ if there is no active screen locker,
	 * attempt to process a compositor keybinding. */
	if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED && !super) {
		if (overview_visible)
			group->overview_keycode = event->keycode;
		for (i = 0; i < nsyms; i++)
			handled = (overview_visible ? (overview_active ? overviewkey(syms[i]) : 1)
					: keybinding(mods, syms[i])) || handled;
	}
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED
			&& (overview_visible || event->keycode == group->overview_keycode)) {
		handled = 1;
		if (event->keycode == group->overview_keycode)
			group->overview_keycode = 0;
	}

	if (handled && !super && !group->overview_keycode
			&& group->wlr_group->keyboard.repeat_info.delay > 0) {
		group->mods = mods;
		group->keysyms = syms;
		group->nsyms = nsyms;
		wl_event_source_timer_update(group->key_repeat_source,
				group->wlr_group->keyboard.repeat_info.delay);
	} else {
		group->nsyms = 0;
		wl_event_source_timer_update(group->key_repeat_source, 0);
	}

	if (handled)
		return;

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Pass unhandled keycodes along to the client. */
	wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
}

void
keypressmod(struct wl_listener *listener, void *data)
{
	/* This event is raised when a modifier key, such as shift or alt, is
	 * pressed. We simply communicate this to the client. */
	KeyboardGroup *group = wl_container_of(listener, group, modifiers);

	wlr_seat_set_keyboard(seat, &group->wlr_group->keyboard);
	/* Send modifiers to the client. */
	wlr_seat_keyboard_notify_modifiers(seat,
			&group->wlr_group->keyboard.modifiers);
}

int
keyrepeat(void *data)
{
	KeyboardGroup *group = data;
	int i;
	if (!group->nsyms || group->wlr_group->keyboard.repeat_info.rate <= 0)
		return 0;

	wl_event_source_timer_update(group->key_repeat_source,
			1000 / group->wlr_group->keyboard.repeat_info.rate);

	for (i = 0; i < group->nsyms; i++)
		keybinding(group->mods, group->keysyms[i]);

	return 0;
}

void
killclient(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel) {
		clientcloseanim(sel);
		client_send_close(sel);
	}
}

void
locksession(struct wl_listener *listener, void *data)
{
	struct wlr_session_lock_v1 *session_lock = data;
	SessionLock *lock;
	overviewset(0);
	wlr_scene_node_set_enabled(&locked_bg->node, 1);
	if (cur_lock) {
		wlr_session_lock_v1_destroy(session_lock);
		return;
	}
	lock = session_lock->data = ecalloc(1, sizeof(*lock));
	focusclient(NULL, 0);

	lock->scene = wlr_scene_tree_create(layers[LyrBlock]);
	cur_lock = lock->lock = session_lock;
	locked = 1;

	LISTEN(&session_lock->events.new_surface, &lock->new_surface, createlocksurface);
	LISTEN(&session_lock->events.destroy, &lock->destroy, destroysessionlock);
	LISTEN(&session_lock->events.unlock, &lock->unlock, unlocksession);

	wlr_session_lock_v1_send_locked(session_lock);
}

void
mapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is mapped, or ready to display on-screen. */
	Client *p = NULL;
	Client *w, *c = wl_container_of(listener, c, map);
	Monitor *m;

	/* Create scene tree for this client and its border */
	c->scene = client_surface(c)->data = wlr_scene_tree_create(layers[LyrTile]);
	/* Enabled later by a call to arrange() */
	wlr_scene_node_set_enabled(&c->scene->node, client_is_unmanaged(c));
	c->scene_surface = c->type == XDGShell
			? wlr_scene_xdg_surface_create(c->scene, c->surface.xdg)
			: wlr_scene_subsurface_tree_create(c->scene, client_surface(c));
	c->scene->node.data = c->scene_surface->node.data = c;

	client_get_geometry(c, &c->geom);

	/* Handle unmanaged clients first so we can return prior create borders */
	if (client_is_unmanaged(c)) {
		/* Unmanaged clients always are floating */
		wlr_scene_node_reparent(&c->scene->node, layers[LyrFloat]);
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
		client_set_size(c, c->geom.width, c->geom.height);
		if (client_wants_focus(c)) {
			focusclient(c, 1);
			exclusive_focus = c;
		}
		goto unset_fullscreen;
	}

	/* Draw an edge-only frame.  Separate corner pieces make a real rounded
	 * outer edge without painting a full rectangle behind transparent clients. */
	c->border.top = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
	c->border.bottom = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
	c->border.left = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
	c->border.right = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
	c->border.top_left = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
	c->border.top_right = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
	c->border.bottom_right = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
	c->border.bottom_left = wlr_scene_rect_create(c->scene, 0, 0, bordercolor);
	c->border.top->node.data = c;
	c->border.bottom->node.data = c;
	c->border.left->node.data = c;
	c->border.right->node.data = c;
	c->border.top_left->node.data = c;
	c->border.top_right->node.data = c;
	c->border.bottom_right->node.data = c;
	c->border.bottom_left->node.data = c;
	wlr_scene_node_lower_to_bottom(&c->border.top->node);
	wlr_scene_node_lower_to_bottom(&c->border.bottom->node);
	wlr_scene_node_lower_to_bottom(&c->border.left->node);
	wlr_scene_node_lower_to_bottom(&c->border.right->node);
	wlr_scene_node_lower_to_bottom(&c->border.top_left->node);
	wlr_scene_node_lower_to_bottom(&c->border.top_right->node);
	wlr_scene_node_lower_to_bottom(&c->border.bottom_right->node);
	wlr_scene_node_lower_to_bottom(&c->border.bottom_left->node);

	/* Find the main surface buffer for rounded corners/blur/fade */
	wlr_scene_node_for_each_buffer(&c->scene_surface->node, findsurfbuf, c);
	applyeffects(c);

	/* Initialize client geometry with room for border */
	client_set_tiled(c, WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
	c->geom.width += 2 * c->bw;
	c->geom.height += 2 * c->bw;

	/* Insert this client into client lists. */
	wl_list_insert(&clients, &c->link);
	wl_list_insert(&fstack, &c->flink);

	if ((c->ftl = wlr_foreign_toplevel_handle_v1_create(ftl_mgr))) {
		const char *title = client_get_title(c), *appid = client_get_appid(c);
		if (title)
			wlr_foreign_toplevel_handle_v1_set_title(c->ftl, title);
		if (appid)
			wlr_foreign_toplevel_handle_v1_set_app_id(c->ftl, appid);
		LISTEN(&c->ftl->events.request_activate, &c->ftl_activate, ftlactivatenotify);
		LISTEN(&c->ftl->events.request_close, &c->ftl_close, ftlclosenotify);
	}

	/* Set initial monitor, tags, floating status, and focus:
	 * we always consider floating, clients that have parent and thus
	 * we set the same tags and monitor as its parent.
	 * If there is no parent, apply rules */
	if ((p = client_get_parent(c))) {
		c->isfloating = 1;
		setmon(c, p->mon, p->ws);
	} else {
		applyrules(c);
	}

	/* Slide + fade the new window in */
	if (animations && c->mon && VISIBLEON(c, c->mon)) {
		c->anim.from.x = c->geom.x;
		c->anim.from.y = c->geom.y + MAX(14, MIN(32, c->geom.height / 18));
		c->anim.fadein = 1;
		c->anim.workspace = 0;
		c->anim.hide = 0;
		c->anim.active = 1;
		c->anim.t = 0;
		wlr_scene_node_set_position(&c->scene->node, c->anim.from.x, c->anim.from.y);
		if (c->surfbuf)
			wlr_scene_buffer_set_opacity(c->surfbuf, 0);
		animkick(c->mon);
	}

unset_fullscreen:
	m = c->mon ? c->mon : xytomon(c->geom.x, c->geom.y);
	wl_list_for_each(w, &clients, link) {
		if (w != c && w != p && m == w->mon && w->ws == c->ws) {
			if (w->isfullscreen)
				setfullscreen(w, 0);
			if (w->isfakefull) {
				w->isfakefull = 0;
				arrange(m);
			}
		}
	}
	if (overview_visible)
		overviewbuild();
}

void
maximizenotify(struct wl_listener *listener, void *data)
{
	/* This event is raised when a client would like to maximize itself,
	 * typically because the user clicked on the maximize button on
	 * client-side decorations. gluewc does not support maximization, but
	 * to conform to xdg-shell protocol we still must send a configure.
	 * Since xdg-shell protocol v5 we should ignore request of unsupported
	 * capabilities, just schedule a empty configure when the client uses <5
	 * protocol version
	 * wlr_xdg_surface_schedule_configure() is used to send an empty reply. */
	Client *c = wl_container_of(listener, c, maximize);
	if (c->surface.xdg->initialized
			&& wl_resource_get_version(c->surface.xdg->toplevel->resource)
					< XDG_TOPLEVEL_WM_CAPABILITIES_SINCE_VERSION)
		wlr_xdg_surface_schedule_configure(c->surface.xdg);
}

void
motionabsolute(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits an _absolute_
	 * motion event, from 0..1 on each axis. This happens, for example, when
	 * wlroots is running under a Wayland window rather than KMS+DRM, and you
	 * move the mouse over the window. You could enter the window from any edge,
	 * so we have to warp the mouse there. Also, some hardware emits these events. */
	struct wlr_pointer_motion_absolute_event *event = data;
	double lx, ly, dx, dy;

	if (!event->time_msec) /* this is 0 with virtual pointers */
		wlr_cursor_warp_absolute(cursor, &event->pointer->base, event->x, event->y);

	wlr_cursor_absolute_to_layout_coords(cursor, &event->pointer->base, event->x, event->y, &lx, &ly);
	dx = lx - cursor->x;
	dy = ly - cursor->y;
	motionnotify(event->time_msec, &event->pointer->base, dx, dy, dx, dy);
}

void
motionnotify(uint32_t time, struct wlr_input_device *device, double dx, double dy,
		double dx_unaccel, double dy_unaccel)
{
	double sx = 0, sy = 0, sx_confined, sy_confined;
	Client *c = NULL, *w = NULL;
	LayerSurface *l = NULL;
	struct wlr_surface *surface = NULL;
	struct wlr_pointer_constraint_v1 *constraint;

	/* Find the client under the pointer and send the event along. */
	xytonode(cursor->x, cursor->y, &surface, &c, NULL, &sx, &sy);

	if (cursor_mode == CurPressed && !seat->drag
			&& surface != seat->pointer_state.focused_surface
			&& toplevel_from_wlr_surface(seat->pointer_state.focused_surface, &w, &l) >= 0) {
		c = w;
		surface = seat->pointer_state.focused_surface;
		sx = cursor->x - (l ? l->scene->node.x : w->geom.x);
		sy = cursor->y - (l ? l->scene->node.y : w->geom.y);
	}

	/* time is 0 in internal calls meant to restore pointer focus. */
	if (time) {
		wlr_relative_pointer_manager_v1_send_relative_motion(
				relative_pointer_mgr, seat, (uint64_t)time * 1000,
				dx, dy, dx_unaccel, dy_unaccel);

		wl_list_for_each(constraint, &pointer_constraints->constraints, link)
			cursorconstrain(constraint);

		if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove) {
			toplevel_from_wlr_surface(active_constraint->surface, &c, NULL);
			if (c && active_constraint->surface == seat->pointer_state.focused_surface) {
				sx = cursor->x - c->geom.x - c->bw;
				sy = cursor->y - c->geom.y - c->bw;
				if (wlr_region_confine(&active_constraint->region, sx, sy,
						sx + dx, sy + dy, &sx_confined, &sy_confined)) {
					dx = sx_confined - sx;
					dy = sy_confined - sy;
				}

				if (active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED)
					return;
			}
		}

		wlr_cursor_move(cursor, device, dx, dy);
		wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);

		/* Update selmon (even while dragging a window) */
		if (sloppyfocus)
			selmon = xytomon(cursor->x, cursor->y);
	}
	if (overviewmotion())
		return;

	/* Update drag icon's position */
	wlr_scene_node_set_position(&drag_icon->node, (int)round(cursor->x), (int)round(cursor->y));

	/* If we are currently grabbing the mouse, handle and return */
	if (cursor_mode == CurMove) {
		/* Move the grabbed client to the new position. */
		resize(grabc, (struct wlr_box){.x = (int)round(cursor->x) - grabcx, .y = (int)round(cursor->y) - grabcy,
			.width = grabc->geom.width, .height = grabc->geom.height}, 1);
		return;
	} else if (cursor_mode == CurResize) {
		resize(grabc, (struct wlr_box){.x = grabc->geom.x, .y = grabc->geom.y,
			.width = (int)round(cursor->x) - grabc->geom.x, .height = (int)round(cursor->y) - grabc->geom.y}, 1);
		return;
	}

	/* If there's no client surface under the cursor, set the cursor image to a
	 * default. This is what makes the cursor image appear when you move it
	 * off of a client or over its border. */
	if (!surface && !seat->drag)
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	pointerfocus(c, surface, sx, sy, time);
}

void
motionrelative(struct wl_listener *listener, void *data)
{
	/* This event is forwarded by the cursor when a pointer emits a _relative_
	 * pointer motion event (i.e. a delta) */
	struct wlr_pointer_motion_event *event = data;
	/* The cursor doesn't move unless we tell it to. The cursor automatically
	 * handles constraining the motion to the output layout, as well as any
	 * special configuration applied for the specific input device which
	 * generated the event. You can pass NULL for the device if you want to move
	 * the cursor around without any input. */
	motionnotify(event->time_msec, &event->pointer->base, event->delta_x, event->delta_y,
			event->unaccel_dx, event->unaccel_dy);
}

void
moveresize(const Arg *arg)
{
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	xytonode(cursor->x, cursor->y, NULL, &grabc, NULL, NULL, NULL);
	if (!grabc || client_is_unmanaged(grabc) || grabc->isfullscreen)
		return;

	/* Float the window and tell motionnotify to grab it */
	setfloating(grabc, 1);
	switch (cursor_mode = arg->ui) {
	case CurMove:
		grabcx = (int)round(cursor->x) - grabc->geom.x;
		grabcy = (int)round(cursor->y) - grabc->geom.y;
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "all-scroll");
		break;
	case CurResize:
		/* Doesn't work for X11 output - the next absolute motion event
		 * returns the cursor to where it started */
		wlr_cursor_warp_closest(cursor, NULL,
				grabc->geom.x + grabc->geom.width,
				grabc->geom.y + grabc->geom.height);
		wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
		break;
	}
}

void
outputmgrapply(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 0);
}

void
outputmgrapplyortest(struct wlr_output_configuration_v1 *config, int test)
{
	/*
	 * Called when a client such as wlr-randr requests a change in output
	 * configuration. This is only one way that the layout can be changed,
	 * so any Monitor information should be updated by updatemons() after an
	 * output_layout.change event, not here.
	 */
	struct wlr_output_configuration_head_v1 *config_head;
	int ok = 1;

	wl_list_for_each(config_head, &config->heads, link) {
		struct wlr_output *wlr_output = config_head->state.output;
		Monitor *m = wlr_output->data;
		struct wlr_output_state state;

		/* Ensure displays previously disabled by wlr-output-power-management-v1
		 * are properly handled*/
		m->asleep = 0;

		wlr_output_state_init(&state);
		wlr_output_state_set_enabled(&state, config_head->state.enabled);
		if (!config_head->state.enabled)
			goto apply_or_test;

		if (config_head->state.mode)
			wlr_output_state_set_mode(&state, config_head->state.mode);
		else
			wlr_output_state_set_custom_mode(&state,
					config_head->state.custom_mode.width,
					config_head->state.custom_mode.height,
					config_head->state.custom_mode.refresh);

		wlr_output_state_set_transform(&state, config_head->state.transform);
		wlr_output_state_set_scale(&state, config_head->state.scale);
		wlr_output_state_set_adaptive_sync_enabled(&state,
				config_head->state.adaptive_sync_enabled);

apply_or_test:
		ok &= test ? wlr_output_test_state(wlr_output, &state)
				: wlr_output_commit_state(wlr_output, &state);

		/* Don't move monitors if position wouldn't change. This avoids
		 * wlroots marking the output as manually configured.
		 * wlr_output_layout_add does not like disabled outputs */
		if (!test && wlr_output->enabled && (m->m.x != config_head->state.x || m->m.y != config_head->state.y))
			wlr_output_layout_add(output_layout, wlr_output,
					config_head->state.x, config_head->state.y);

		wlr_output_state_finish(&state);
	}

	if (ok)
		wlr_output_configuration_v1_send_succeeded(config);
	else
		wlr_output_configuration_v1_send_failed(config);
	wlr_output_configuration_v1_destroy(config);

	/* https://codeberg.org/dwl/dwl/issues/577 */
	updatemons(NULL, NULL);
}

void
outputmgrtest(struct wl_listener *listener, void *data)
{
	struct wlr_output_configuration_v1 *config = data;
	outputmgrapplyortest(config, 1);
}

void
pointerfocus(Client *c, struct wlr_surface *surface, double sx, double sy,
		uint32_t time)
{
	struct timespec now;

	if (surface != seat->pointer_state.focused_surface &&
			sloppyfocus && time && c && !client_is_unmanaged(c))
		focusclient(c, 0);

	/* If surface is NULL, clear pointer focus */
	if (!surface) {
		wlr_seat_pointer_notify_clear_focus(seat);
		return;
	}

	if (!time) {
		clock_gettime(CLOCK_MONOTONIC, &now);
		time = now.tv_sec * 1000 + now.tv_nsec / 1000000;
	}

	/* Let the client know that the mouse cursor has entered one
	 * of its surfaces, and make keyboard focus follow if desired.
	 * wlroots makes this a no-op if surface is already focused */
	wlr_seat_pointer_notify_enter(seat, surface, sx, sy);
	wlr_seat_pointer_notify_motion(seat, time, sx, sy);
}

void
powermgrsetmode(struct wl_listener *listener, void *data)
{
	struct wlr_output_power_v1_set_mode_event *event = data;
	struct wlr_output_state state = {0};
	Monitor *m = event->output->data;

	if (!m)
		return;

	m->gamma_lut_changed = 1; /* Reapply gamma LUT when re-enabling the ouput */
	wlr_output_state_set_enabled(&state, event->mode);
	wlr_output_commit_state(m->wlr_output, &state);

	m->asleep = !event->mode;
	updatemons(NULL, NULL);
}

void
quit(const Arg *arg)
{
	wl_display_terminate(dpy);
}

void
rendermon(struct wl_listener *listener, void *data)
{
	/* This function is called every time an output is ready to display a frame,
	 * generally at the output's refresh rate (e.g. 60Hz). */
	Monitor *m = wl_container_of(listener, m, frame);
	Client *c;
	struct wlr_output_state pending = {0};
	struct timespec now;
	int animpending = 0, pointerrefresh = 0, skipframe = 0;

	/* Render if no XDG clients have an outstanding resize and are visible on
	 * this monitor. */
	wl_list_for_each(c, &clients, link) {
		if (c->resize && !c->isfloating && client_is_rendered_on_mon(c, m) && !client_is_stopped(c)) {
			skipframe = 1;
			break;
		}
	}

	/* Animations advance only on frames that actually render, and only
	 * schedule further frames while one is active. */
	if (animations || overview_visible) {
		uint32_t t_now = now_ms();
		float dt = m->lastanimtick && t_now > m->lastanimtick
				? (float)(t_now - m->lastanimtick) : 0.0f;
		if (dt > 34.0f)
			dt = 34.0f;
		m->lastanimtick = t_now;
		if (animations) {
			wl_list_for_each(c, &clients, link) {
				float e;
				int duration, targetx, targety;
				if (c->mon != m || !c->anim.active)
					continue;
				animpending = 1;
				if (skipframe)
					continue;
				duration = c->anim.workspace
						? MAX(180, animation_duration * 5 / 4)
						: c->anim.fadein
						? MAX(160, animation_duration * 3 / 4)
						: animation_duration;
				targetx = c->anim.workspace ? c->anim.to.x : c->geom.x;
				targety = c->anim.workspace ? c->anim.to.y : c->geom.y;
				c->anim.t += duration > 0 ? dt / (float)duration : 1.0f;
				if (c->anim.t >= 1.0f) {
					c->anim.active = 0;
					wlr_scene_node_set_position(&c->scene->node,
							c->anim.hide ? c->geom.x : targetx,
							c->anim.hide ? c->geom.y : targety);
					if (c->anim.hide) {
						wlr_scene_node_set_enabled(&c->scene->node, 0);
						pointerrefresh = 1;
					}
					c->anim.workspace = 0;
					c->anim.hide = 0;
					if (c->anim.fadein) {
						c->anim.fadein = 0;
						if (c->surfbuf)
							wlr_scene_buffer_set_opacity(c->surfbuf, clientopacity(c));
					}
					continue;
				}
				e = c->anim.workspace
						? c->anim.t * c->anim.t * (3.0f - 2.0f * c->anim.t)
						: c->anim.fadein
						? 1.0f - powf(1.0f - c->anim.t, 3.0f)
						: 1.0f - powf(2.0f, -10.0f * c->anim.t);
				wlr_scene_node_set_position(&c->scene->node,
						c->anim.from.x + (int)((float)(targetx - c->anim.from.x) * e),
						c->anim.from.y + (int)((float)(targety - c->anim.from.y) * e));
				if (c->anim.fadein && c->surfbuf) {
					float opacity = c->anim.t * (2.0f - c->anim.t);
					wlr_scene_buffer_set_opacity(c->surfbuf,
							opacity * clientopacity(c));
				}
			}
			if (!skipframe) {
				animpending |= layeranimadvance(m, dt);
				animpending |= closeanimadvance(m, dt);
			}
		}
		if (pointerrefresh)
			motionnotify(0, NULL, 0, 0, 0, 0);
		if (overviewadvance(m, dt)) {
			overviewbuild();
			animpending |= m->overview_animating;
		}
	}
	overviewfinish();

	if (skipframe)
		goto skip;

	wlr_scene_output_commit(m->scene_output, NULL);

skip:
	/* Let clients know a frame has been rendered */
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(m->scene_output, &now);
	wlr_output_state_finish(&pending);
	/* scheduling a frame without a commit fires immediately and would spin
	 * the event loop; while a resize is pending the client's next commit
	 * damages the scene and restarts the animation on its own */
	if (animpending && !skipframe)
		wlr_output_schedule_frame(m->wlr_output);
}

void
requestdecorationmode(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_decoration_mode);
	if (c->surface.xdg->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(c->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

void
requeststartdrag(struct wl_listener *listener, void *data)
{
	struct wlr_seat_request_start_drag_event *event = data;

	if (wlr_seat_validate_pointer_grab_serial(seat, event->origin,
			event->serial))
		wlr_seat_start_pointer_drag(seat, event->drag, event->serial);
	else
		wlr_data_source_destroy(event->drag->source);
}

void
requestmonstate(struct wl_listener *listener, void *data)
{
	struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(event->output, event->state);
	updatemons(NULL, NULL);
}

void
resize(Client *c, struct wlr_box geo, int interact)
{
	struct wlr_box *bbox;
	struct wlr_box clip;
	int oldx = c->geom.x, oldy = c->geom.y;

	if (!c->mon || !client_surface(c)->mapped)
		return;

	bbox = interact ? &sgeom : &c->mon->w;

	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	applybounds(c, bbox);

	/* animate position changes of visible clients */
	if (animations && !interact && !c->anim.fadein
			&& (c->geom.x != oldx || c->geom.y != oldy)
			&& VISIBLEON(c, c->mon)) {
		c->anim.from.x = c->scene->node.x;
		c->anim.from.y = c->scene->node.y;
		c->anim.workspace = 0;
		c->anim.hide = 0;
		c->anim.active = 1;
		c->anim.t = 0;
		animkick(c->mon);
	}

	/* Update scene-graph, including the border */
	if (!c->anim.active)
		wlr_scene_node_set_position(&c->scene->node, c->geom.x, c->geom.y);
	wlr_scene_node_set_position(&c->scene_surface->node, c->bw, c->bw);
	if (c->border.top) {
		int bw = (int)c->bw;
		int r = (c->isfullscreen || c->isfakefull) ? 0
				: MIN(MAX(0, corner_radius), MIN(c->geom.width, c->geom.height) / 2);
		int horizontal = MAX(0, c->geom.width - 2 * r);
		int vertical = MAX(0, c->geom.height - 2 * r);

		wlr_scene_rect_set_size(c->border.top, horizontal, bw);
		wlr_scene_node_set_position(&c->border.top->node, r, 0);
		wlr_scene_rect_set_size(c->border.bottom, horizontal, bw);
		wlr_scene_node_set_position(&c->border.bottom->node, r,
				MAX(0, c->geom.height - bw));
		wlr_scene_rect_set_size(c->border.left, bw, vertical);
		wlr_scene_node_set_position(&c->border.left->node, 0, r);
		wlr_scene_rect_set_size(c->border.right, bw, vertical);
		wlr_scene_node_set_position(&c->border.right->node,
				MAX(0, c->geom.width - bw), r);

		wlr_scene_rect_set_size(c->border.top_left, r, r);
		wlr_scene_node_set_position(&c->border.top_left->node, 0, 0);
		wlr_scene_rect_set_size(c->border.top_right, r, r);
		wlr_scene_node_set_position(&c->border.top_right->node,
				MAX(0, c->geom.width - r), 0);
		wlr_scene_rect_set_size(c->border.bottom_right, r, r);
		wlr_scene_node_set_position(&c->border.bottom_right->node,
				MAX(0, c->geom.width - r), MAX(0, c->geom.height - r));
		wlr_scene_rect_set_size(c->border.bottom_left, r, r);
		wlr_scene_node_set_position(&c->border.bottom_left->node, 0,
				MAX(0, c->geom.height - r));
	}

	/* this is a no-op if size hasn't changed */
	c->resize = client_set_size(c, c->geom.width - 2 * c->bw,
			c->geom.height - 2 * c->bw);
	client_get_clip(c, &clip);
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
}

void
run(char *startup_cmd)
{
	/* Add a Unix socket to the Wayland display. */
	const char *socket = wl_display_add_socket_auto(dpy);
	if (!socket)
		die("startup: display_add_socket_auto");
	setenv("WAYLAND_DISPLAY", socket, 1);

	/* Start the backend. This will enumerate outputs and inputs, become the DRM
	 * master, etc */
	if (!wlr_backend_start(backend))
		die("startup: backend_start");

	/* Now that the socket exists and the backend is started, run the startup command */
	if (startup_cmd) {
		int piperw[2];
		if (pipe(piperw) < 0)
			die("startup: pipe:");
		if ((child_pid = fork()) < 0)
			die("startup: fork:");
		if (child_pid == 0) {
			setsid();
			dup2(piperw[0], STDIN_FILENO);
			close(piperw[0]);
			close(piperw[1]);
			execl("/bin/sh", "/bin/sh", "-c", startup_cmd, NULL);
			die("startup: execl:");
		}
		dup2(piperw[1], STDOUT_FILENO);
		close(piperw[1]);
		close(piperw[0]);
	}

	/* Mark stdout as non-blocking to avoid the startup script
	 * causing gluewc to freeze when a user neither closes stdin
	 * nor consumes standard input in his startup script */

	if (fd_set_nonblock(STDOUT_FILENO) < 0)
		close(STDOUT_FILENO);

	/* let bus-activated services (xdg-desktop-portal) see the compositor */
	{
		Arg a = {.v = (const char *[]){ "/bin/sh", "-c",
			"command -v dbus-update-activation-environment >/dev/null 2>&1 "
			"&& exec dbus-update-activation-environment --all", NULL }};
		spawn(&a);
	}

	/* Run the autostart commands from the config file */
	{
		size_t i;
		for (i = 0; i < nautostarts; i++) {
			Arg a = {.v = (const char *[]){ "/bin/sh", "-c", autostarts[i], NULL }};
			spawn(&a);
		}
	}

	/* At this point the outputs are initialized, choose initial selmon based on
	 * cursor position, and set default cursor image */
	selmon = xytomon(cursor->x, cursor->y);

	/* TODO hack to get cursor to display in its initial location (100, 100)
	 * instead of (0, 0) and then jumping. Still may not be fully
	 * initialized, as the image/coordinates are not transformed for the
	 * monitor when displayed here */
	wlr_cursor_warp_closest(cursor, NULL, cursor->x, cursor->y);
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "default");

	/* Run the Wayland event loop. This does not return until you exit the
	 * compositor. Starting the backend rigged up all of the necessary event
	 * loop configuration to listen to libinput events, DRM events, generate
	 * frame events at the refresh rate, and so on. */
	wl_display_run(dpy);
}

void
setcursor(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client provides a cursor image */
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	/* If we're "grabbing" the cursor, don't use the client's image, we will
	 * restore it after "grabbing" sending a leave event, followed by a enter
	 * event, which will result in the client requesting set the cursor surface */
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided surface as the cursor image. It will set the
	 * hardware cursor on the output that it's currently on and continue to
	 * do so as the cursor moves between outputs. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_surface(cursor, event->surface,
				event->hotspot_x, event->hotspot_y);
}

void
setcursorshape(struct wl_listener *listener, void *data)
{
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	/* This can be sent by any client, so we check to make sure this one
	 * actually has pointer focus first. If so, we can tell the cursor to
	 * use the provided cursor shape. */
	if (event->seat_client == seat->pointer_state.focused_client)
		wlr_cursor_set_xcursor(cursor, cursor_mgr,
				wlr_cursor_shape_v1_name(event->shape));
}

void
setfloating(Client *c, int floating)
{
	Client *p = client_get_parent(c);
	c->isfloating = floating;
	if (!c->mon || !client_surface(c)->mapped)
		return;
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen ||
			(p && p->isfullscreen) ? LyrFS
			: c->isfloating ? LyrFloat : LyrTile]);
	arrange(c->mon);
}

void
setfullscreen(Client *c, int fullscreen)
{
	c->isfullscreen = fullscreen;
	if (fullscreen)
		c->isfakefull = 0;
	if (!c->mon || !client_surface(c)->mapped)
		return;
	c->bw = (fullscreen || decorhidden) ? 0 : borderpx;
	client_set_fullscreen(c, fullscreen);
	applyeffects(c);
	wlr_scene_node_reparent(&c->scene->node, layers[c->isfullscreen
			? LyrFS : c->isfloating ? LyrFloat : LyrTile]);

	if (fullscreen) {
		c->prev = c->geom;
		resize(c, c->mon->m, 0);
	} else {
		/* restore previous size instead of arrange for floating windows since
		 * client positions are set by the user and cannot be recalculated */
		resize(c, c->prev, 0);
	}
	arrange(c->mon);
}

void
setmon(Client *c, Monitor *m, int ws)
{
	Monitor *oldmon = c->mon;

	if (oldmon == m)
		return;
	if (oldmon) {
		animstop(oldmon);
		bsp_detach(c);
	}
	if (c->ftl && oldmon)
		wlr_foreign_toplevel_handle_v1_output_leave(c->ftl, oldmon->wlr_output);
	if (c->ftl && m)
		wlr_foreign_toplevel_handle_v1_output_enter(c->ftl, m->wlr_output);
	c->mon = m;
	c->prev = c->geom;

	/* Scene graph sends surface leave/enter events on move and resize */
	if (oldmon)
		arrange(oldmon);
	if (m) {
		bsp_attach(m, (ws >= 0 && ws < NUMWS) ? (unsigned int)ws : m->ws, c);
		/* Make sure window actually overlaps with the monitor */
		resize(c, c->geom, 0);
		setfullscreen(c, c->isfullscreen); /* This will call arrange(c->mon) */
		setfloating(c, c->isfloating);
	}
	focusclient(focustop(selmon), 1);
}

void
setpsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in gluewc we always honor them
	 */
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(seat, event->source, event->serial);
}

void
setsel(struct wl_listener *listener, void *data)
{
	/* This event is raised by the seat when a client wants to set the selection,
	 * usually when the user copies something. wlroots allows compositors to
	 * ignore such requests if they so choose, but in gluewc we always honor them
	 */
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(seat, event->source, event->serial);
}

static char *
cfgtrim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
		*--e = '\0';
	return s;
}

static int
cfgcolor(const char *s, float out[4])
{
	unsigned int v;
	if (sscanf(s, "%x", &v) != 1)
		return 0;
	if (strlen(s) <= 6)
		v = (v << 8) | 0xff;
	out[0] = ((v >> 24) & 0xff) / 255.0f;
	out[1] = ((v >> 16) & 0xff) / 255.0f;
	out[2] = ((v >> 8) & 0xff) / 255.0f;
	out[3] = (v & 0xff) / 255.0f;
	return 1;
}

static int
cfgbindcombo(char *combo, uint32_t *mods, xkb_keysym_t *sym)
{
	char *tok, *save = NULL, *last = NULL;
	*mods = 0;
	for (tok = strtok_r(combo, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
		if (last) {
			if (!strcmp(last, "mod") || !strcmp(last, "super") || !strcmp(last, "logo"))
				*mods |= WLR_MODIFIER_LOGO;
			else if (!strcmp(last, "shift"))
				*mods |= WLR_MODIFIER_SHIFT;
			else if (!strcmp(last, "ctrl") || !strcmp(last, "control"))
				*mods |= WLR_MODIFIER_CTRL;
			else if (!strcmp(last, "alt"))
				*mods |= WLR_MODIFIER_ALT;
			else
				return 0;
		}
		last = tok;
	}
	if (!last)
		return 0;
	*sym = xkb_keysym_from_name(last, XKB_KEYSYM_NO_FLAGS);
	if (*sym == XKB_KEY_NoSymbol)
		*sym = xkb_keysym_from_name(last, XKB_KEYSYM_CASE_INSENSITIVE);
	return *sym != XKB_KEY_NoSymbol;
}

static int
cfgaction(const char *act, void (**func)(const Arg *), Arg *arg)
{
	static const struct {
		const char *name;
		void (*func)(const Arg *);
		Arg arg;
	} acts[] = {
		{ "wm:quit",                  quit,                 {0} },
		{ "wm:reload",                reloadconfig,         {0} },
		{ "wm:restart",               reloadconfig,         {0} },
		{ "wm:overview",              overviewtoggle,       {0} },
		{ "wm:toggle_opacity",        toggleopacity,        {0} },
		{ "wm:kill",                  killclient,           {0} },
		{ "wm:mode:insert",           entermode,            {.i = ModeInsert} },
		{ "wm:mode:normal",           entermode,            {.i = ModeNormal} },
		{ "wm:focus_left",            focusdir,             {.i = DirLeft} },
		{ "wm:focus_right",           focusdir,             {.i = DirRight} },
		{ "wm:focus_up",              focusdir,             {.i = DirUp} },
		{ "wm:focus_down",            focusdir,             {.i = DirDown} },
		{ "wm:focus_next",            focusnext,            {0} },
		{ "wm:swap_left",             swapdir,              {.i = DirLeft} },
		{ "wm:swap_right",            swapdir,              {.i = DirRight} },
		{ "wm:swap_up",               swapdir,              {.i = DirUp} },
		{ "wm:swap_down",             swapdir,              {.i = DirDown} },
		{ "wm:swap_prev",             swapstack,            {.i = -1} },
		{ "wm:swap_next",             swapstack,            {.i = +1} },
		{ "wm:toggle_split",          togglesplit,          {0} },
		{ "wm:toggle_fullscreen",     togglefakefullscreen, {0} },
		{ "wm:toggle_real_fullscreen",togglefullscreen,     {0} },
		{ "wm:toggle_float",          togglefloating,       {0} },
		{ "wm:toggle_float_centered", togglefloating,       {0} },
		{ "wm:toggle_decorations",    toggledecor,          {0} },
		{ "wm:workspace_prev",        wsstep,               {.i = -1} },
		{ "wm:workspace_next",        wsstep,               {.i = +1} },
		{ "wm:move_to_workspace_prev", movewsstep,          {.i = -1} },
		{ "wm:move_to_workspace_next", movewsstep,          {.i = +1} },
	};
	size_t i;
	int n;

	if (!strncmp(act, "spawn:", 6)) {
		const char **argv = ecalloc(4, sizeof(char *));
		argv[0] = "/bin/sh";
		argv[1] = "-c";
		argv[2] = strdup(act + 6);
		argv[3] = NULL;
		*func = spawn;
		arg->v = argv;
		return 1;
	}
	for (i = 0; i < LENGTH(acts); i++) {
		if (!strcmp(act, acts[i].name)) {
			*func = acts[i].func;
			*arg = acts[i].arg;
			return 1;
		}
	}
	if (!strncmp(act, "wm:ratio:", 9)) {
		*func = setratio;
		arg->f = strtof(act + 9, NULL);
		return 1;
	}
	if (sscanf(act, "wm:workspace:%d", &n) == 1 && n >= 1 && n <= NUMWS) {
		*func = viewws;
		arg->ui = n - 1;
		return 1;
	}
	if (sscanf(act, "wm:move_to_workspace_follow:%d", &n) == 1 && n >= 1 && n <= NUMWS) {
		*func = movetowsfollow;
		arg->ui = n - 1;
		return 1;
	}
	if (sscanf(act, "wm:move_to_workspace:%d", &n) == 1 && n >= 1 && n <= NUMWS) {
		*func = movetows;
		arg->ui = n - 1;
		return 1;
	}
	return 0;
}

static void
cfgaddbind(Bind **arr, size_t *n, uint32_t mods, xkb_keysym_t sym,
		void (*func)(const Arg *), Arg arg)
{
	size_t i;
	for (i = 0; i < *n; i++)
		if ((*arr)[i].mod == mods && (*arr)[i].keysym == sym)
			break;
	if (i == *n) {
		if (!(*arr = realloc(*arr, (*n + 1) * sizeof(Bind))))
			die("config: realloc:");
		(*n)++;
	}
	(*arr)[i].mod = mods;
	(*arr)[i].keysym = sym;
	(*arr)[i].func = func;
	(*arr)[i].arg = arg;
}

void
readconfig(void)
{
	char path[512], *line = NULL, *k, *v, *v2;
	const char *env;
	size_t i, lsz = 0;
	FILE *f;

	/* start from the built-in binds; the config upserts over them */
	for (i = 0; i < LENGTH(keys); i++)
		cfgaddbind(&runkeys, &nrunkeys, keys[i].mod, keys[i].keysym,
				keys[i].func, keys[i].arg);
	for (i = 0; i < LENGTH(normalkeys); i++)
		cfgaddbind(&runnormalkeys, &nrunnormalkeys, normalkeys[i].mod,
				normalkeys[i].keysym, normalkeys[i].func, normalkeys[i].arg);

	if ((env = getenv("XDG_CONFIG_HOME")))
		snprintf(path, sizeof path, "%s/gluewc/config.conf", env);
	else if ((env = getenv("HOME")))
		snprintf(path, sizeof path, "%s/.config/gluewc/config.conf", env);
	else
		return;
	if (!(f = fopen(path, "r")))
		return;

	while (getline(&line, &lsz, f) != -1) {
		k = cfgtrim(line);
		if (!*k || *k == '#')
			continue;
		if (!(v = strchr(k, '=')))
			continue;
		*v++ = '\0';
		k = cfgtrim(k);
		v = cfgtrim(v);
		if (!strcmp(k, "bind_insert") || !strcmp(k, "bind_normal")) {
			uint32_t mods;
			xkb_keysym_t sym;
			void (*func)(const Arg *);
			Arg arg = {0};
			if (!(v2 = strchr(v, '=')))
				continue;
			*v2++ = '\0';
			v = cfgtrim(v);
			v2 = cfgtrim(v2);
			if (!cfgbindcombo(v, &mods, &sym) || !cfgaction(v2, &func, &arg)) {
				wlr_log(WLR_ERROR, "config: bad bind '%s = %s'", v, v2);
				continue;
			}
			if (!strcmp(k, "bind_insert"))
				cfgaddbind(&runkeys, &nrunkeys, mods, sym, func, arg);
			else
				cfgaddbind(&runnormalkeys, &nrunnormalkeys, mods, sym, func, arg);
		} else if (!strcmp(k, "gap")) {
			gappx = atoi(v);
		} else if (!strcmp(k, "border")) {
			borderpx = (unsigned int)atoi(v);
		} else if (!strcmp(k, "border_focus")) {
			cfgcolor(v, focuscolor);
		} else if (!strcmp(k, "border_normal")) {
			cfgcolor(v, bordercolor);
		} else if (!strcmp(k, "unfocused_borders")) {
			unfocused_borders = !strcmp(v, "true") || !strcmp(v, "1");
		} else if (!strcmp(k, "normal_mode_color")) {
			cfgcolor(v, normalmodecolor);
		} else if (!strcmp(k, "root_color")) {
			cfgcolor(v, rootcolor);
		} else if (!strcmp(k, "warp_pointer")) {
			warpcursor = !strcmp(v, "true") || !strcmp(v, "1");
		} else if (!strcmp(k, "corner_radius")) {
			corner_radius = atoi(v);
		} else if (!strcmp(k, "blur")) {
			blurenabled = !strcmp(v, "true") || !strcmp(v, "1");
		} else if (!strcmp(k, "blur_passes")) {
			blur_passes = atoi(v);
		} else if (!strcmp(k, "blur_radius")) {
			blur_radius = atoi(v);
		} else if (!strcmp(k, "opacity")) {
			win_opacity = strtof(v, NULL);
			if (win_opacity < 0.1f)
				win_opacity = 0.1f;
			if (win_opacity > 1.0f)
				win_opacity = 1.0f;
		} else if (!strcmp(k, "animations")) {
			animations = !strcmp(v, "true") || !strcmp(v, "1");
		} else if (!strcmp(k, "animation_duration")) {
			animation_duration = atoi(v);
		} else if (!strcmp(k, "repeat_rate")) {
			repeat_rate = atoi(v);
		} else if (!strcmp(k, "repeat_delay")) {
			repeat_delay = atoi(v);
		} else if (!strcmp(k, "xkb_layout")) {
			snprintf(xkb_layout_buf, sizeof xkb_layout_buf, "%s", v);
			xkb_rules.layout = *xkb_layout_buf ? xkb_layout_buf : NULL;
		} else if (!strcmp(k, "xkb_variant")) {
			snprintf(xkb_variant_buf, sizeof xkb_variant_buf, "%s", v);
			xkb_rules.variant = *xkb_variant_buf ? xkb_variant_buf : NULL;
		} else if (!strcmp(k, "xkb_options")) {
			snprintf(xkb_options_buf, sizeof xkb_options_buf, "%s", v);
			xkb_rules.options = *xkb_options_buf ? xkb_options_buf : NULL;
		} else if (!strcmp(k, "autostart")) {
			if (!(autostarts = realloc(autostarts, (nautostarts + 1) * sizeof(char *))))
				die("config: realloc:");
			autostarts[nautostarts++] = strdup(v);
		} else {
			wlr_log(WLR_ERROR, "config: unknown key '%s'", k);
		}
	}
	free(line);
	fclose(f);
}

void
setup(void)
{
	int drm_fd, i, sig[] = {SIGCHLD, SIGINT, SIGTERM, SIGPIPE};
	struct sigaction sa = {.sa_flags = SA_RESTART, .sa_handler = handlesig};
	sigemptyset(&sa.sa_mask);

	for (i = 0; i < (int)LENGTH(sig); i++)
		sigaction(sig[i], &sa, NULL);
	signal(SIGSEGV, handlecrash);
	signal(SIGABRT, handlecrash);
	signal(SIGBUS, handlecrash);

	wlr_log_init(log_level, logfilter);

	readconfig();

	/* The Wayland display is managed by libwayland. It handles accepting
	 * clients from the Unix socket, manging Wayland globals, and so on. */
	dpy = wl_display_create();
	event_loop = wl_display_get_event_loop(dpy);

	/* The backend is a wlroots feature which abstracts the underlying input and
	 * output hardware. The autocreate option will choose the most suitable
	 * backend based on the current environment, such as opening an X11 window
	 * if an X11 server is running. */
	if (!(backend = wlr_backend_autocreate(event_loop, &session)))
		die("couldn't create backend");

	/* Initialize the scene graph used to lay out windows */
	scene = wlr_scene_create();
	wlr_scene_set_blur_data(scene, blur_passes, blur_radius, 0.02f, 0.9f, 0.9f, 1.1f);
	root_bg = wlr_scene_rect_create(&scene->tree, 0, 0, rootcolor);
	wl_list_init(&close_anims);
	for (i = 0; i < NUM_LAYERS; i++)
		layers[i] = wlr_scene_tree_create(&scene->tree);
	drag_icon = wlr_scene_tree_create(&scene->tree);
	wlr_scene_node_place_below(&drag_icon->node, &layers[LyrBlock]->node);
	overview_dim_scene = wlr_scene_tree_create(layers[LyrBottom]);
	wlr_scene_node_set_enabled(&overview_dim_scene->node, 0);
	overview_scene = wlr_scene_tree_create(layers[LyrOverlay]);
	overview_panels_scene = wlr_scene_tree_create(overview_scene);
	overview_drag_scene = wlr_scene_tree_create(overview_scene);
	wlr_scene_node_set_enabled(&overview_scene->node, 0);

	/* Autocreates a renderer, either Pixman, GLES2 or Vulkan for us. The user
	 * can also specify a renderer using the WLR_RENDERER env var.
	 * The renderer is responsible for defining the various pixel formats it
	 * supports for shared memory, this configures that for clients. */
	if (!(drw = fx_renderer_create(backend)))
		die("couldn't create renderer");
	wl_signal_add(&drw->events.lost, &gpu_reset);

	/* Create shm, drm and linux_dmabuf interfaces by ourselves.
	 * The simplest way is to call:
	 *      wlr_renderer_init_wl_display(drw);
	 * but we need to create the linux_dmabuf interface manually to integrate it
	 * with wlr_scene. */
	wlr_renderer_init_wl_shm(drw, dpy);

	if (wlr_renderer_get_texture_formats(drw, WLR_BUFFER_CAP_DMABUF)) {
		wlr_drm_create(dpy, drw);
		wlr_scene_set_linux_dmabuf_v1(scene,
				wlr_linux_dmabuf_v1_create_with_renderer(dpy, 5, drw));
	}

	if ((drm_fd = wlr_renderer_get_drm_fd(drw)) >= 0 && drw->features.timeline
			&& backend->features.timeline)
		wlr_linux_drm_syncobj_manager_v1_create(dpy, 1, drm_fd);

	/* Autocreates an allocator for us.
	 * The allocator is the bridge between the renderer and the backend. It
	 * handles the buffer creation, allowing wlroots to render onto the
	 * screen */
	if (!(alloc = wlr_allocator_autocreate(backend, drw)))
		die("couldn't create allocator");

	/* This creates some hands-off wlroots interfaces. The compositor is
	 * necessary for clients to allocate surfaces and the data device manager
	 * handles the clipboard. Each of these wlroots interfaces has room for you
	 * to dig your fingers in and play with their behavior if you want. Note that
	 * the clients cannot set the selection directly without compositor approval,
	 * see the setsel() function. */
	compositor = wlr_compositor_create(dpy, 6, drw);
	wlr_subcompositor_create(dpy);
	wlr_data_device_manager_create(dpy);
	wlr_export_dmabuf_manager_v1_create(dpy);
	wlr_screencopy_manager_v1_create(dpy);
	wlr_data_control_manager_v1_create(dpy);
	wlr_primary_selection_v1_device_manager_create(dpy);
	wlr_viewporter_create(dpy);
	wlr_single_pixel_buffer_manager_v1_create(dpy);
	wlr_fractional_scale_manager_v1_create(dpy, 1);
	wlr_presentation_create(dpy, backend, 2);
	wlr_alpha_modifier_v1_create(dpy);

	wlr_scene_set_gamma_control_manager_v1(scene, wlr_gamma_control_manager_v1_create(dpy));

	power_mgr = wlr_output_power_manager_v1_create(dpy);
	wl_signal_add(&power_mgr->events.set_mode, &output_power_mgr_set_mode);

	/* Creates an output layout, which is a wlroots utility for working with an
	 * arrangement of screens in a physical layout. */
	output_layout = wlr_output_layout_create(dpy);
	wl_signal_add(&output_layout->events.change, &layout_change);

    wlr_xdg_output_manager_v1_create(dpy, output_layout);

	/* Configure a listener to be notified when new outputs are available on the
	 * backend. */
	wl_list_init(&mons);
	wl_signal_add(&backend->events.new_output, &new_output);

	/* Set up our client lists, the xdg-shell and the layer-shell. The xdg-shell is a
	 * Wayland protocol which is used for application windows. For more
	 * detail on shells, refer to the article:
	 *
	 * https://drewdevault.com/2018/07/29/Wayland-shells.html
	 */
	wl_list_init(&clients);
	wl_list_init(&fstack);

	xdg_shell = wlr_xdg_shell_create(dpy, 6);
	wl_signal_add(&xdg_shell->events.new_toplevel, &new_xdg_toplevel);
	wl_signal_add(&xdg_shell->events.new_popup, &new_xdg_popup);

	layer_shell = wlr_layer_shell_v1_create(dpy, 3);
	wl_signal_add(&layer_shell->events.new_surface, &new_layer_surface);

	idle_notifier = wlr_idle_notifier_v1_create(dpy);

	idle_inhibit_mgr = wlr_idle_inhibit_v1_create(dpy);
	wl_signal_add(&idle_inhibit_mgr->events.new_inhibitor, &new_idle_inhibitor);

	session_lock_mgr = wlr_session_lock_manager_v1_create(dpy);
	wl_signal_add(&session_lock_mgr->events.new_lock, &new_session_lock);
	locked_bg = wlr_scene_rect_create(layers[LyrBlock], sgeom.width, sgeom.height,
			(float [4]){0.1f, 0.1f, 0.1f, 1.0f});
	wlr_scene_node_set_enabled(&locked_bg->node, 0);

	/* Use decoration protocols to negotiate server-side decorations */
	wlr_server_decoration_manager_set_default_mode(
			wlr_server_decoration_manager_create(dpy),
			WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	xdg_decoration_mgr = wlr_xdg_decoration_manager_v1_create(dpy);
	wl_signal_add(&xdg_decoration_mgr->events.new_toplevel_decoration, &new_xdg_decoration);

	pointer_constraints = wlr_pointer_constraints_v1_create(dpy);
	wl_signal_add(&pointer_constraints->events.new_constraint, &new_pointer_constraint);

	relative_pointer_mgr = wlr_relative_pointer_manager_v1_create(dpy);

	/*
	 * Creates a cursor, which is a wlroots utility for tracking the cursor
	 * image shown on screen.
	 */
	cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(cursor, output_layout);

	/* Creates an xcursor manager, another wlroots utility which loads up
	 * Xcursor themes to source cursor images from and makes sure that cursor
	 * images are available at all scale factors on the screen (necessary for
	 * HiDPI support). Scaled cursors will be loaded with each output. */
	cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
	setenv("XCURSOR_SIZE", "24", 1);

	/*
	 * wlr_cursor *only* displays an image on screen. It does not move around
	 * when the pointer moves. However, we can attach input devices to it, and
	 * it will generate aggregate events for all of them. In these events, we
	 * can choose how we want to process them, forwarding them to clients and
	 * moving the cursor around. More detail on this process is described in
	 * https://drewdevault.com/2018/07/17/Input-handling-in-wlroots.html
	 *
	 * And more comments are sprinkled throughout the notify functions above.
	 */
	wl_signal_add(&cursor->events.motion, &cursor_motion);
	wl_signal_add(&cursor->events.motion_absolute, &cursor_motion_absolute);
	wl_signal_add(&cursor->events.button, &cursor_button);
	wl_signal_add(&cursor->events.axis, &cursor_axis);
	wl_signal_add(&cursor->events.frame, &cursor_frame);
	wl_signal_add(&cursor->events.swipe_begin, &cursor_swipe_begin);
	wl_signal_add(&cursor->events.swipe_update, &cursor_swipe_update);
	wl_signal_add(&cursor->events.swipe_end, &cursor_swipe_end);

	cursor_shape_mgr = wlr_cursor_shape_manager_v1_create(dpy, 1);
	wl_signal_add(&cursor_shape_mgr->events.request_set_shape, &request_set_cursor_shape);

	/*
	 * Configures a seat, which is a single "seat" at which a user sits and
	 * operates the computer. This conceptually includes up to one keyboard,
	 * pointer, touch, and drawing tablet device. We also rig up a listener to
	 * let us know when new input devices are available on the backend.
	 */
	wl_signal_add(&backend->events.new_input, &new_input_device);
	virtual_keyboard_mgr = wlr_virtual_keyboard_manager_v1_create(dpy);
	wl_signal_add(&virtual_keyboard_mgr->events.new_virtual_keyboard,
			&new_virtual_keyboard);
	virtual_pointer_mgr = wlr_virtual_pointer_manager_v1_create(dpy);
    wl_signal_add(&virtual_pointer_mgr->events.new_virtual_pointer,
            &new_virtual_pointer);

	seat = wlr_seat_create(dpy, "seat0");
	wl_signal_add(&seat->events.request_set_cursor, &request_cursor);
	wl_signal_add(&seat->events.request_set_selection, &request_set_sel);
	wl_signal_add(&seat->events.request_set_primary_selection, &request_set_psel);
	wl_signal_add(&seat->events.request_start_drag, &request_start_drag);
	wl_signal_add(&seat->events.start_drag, &start_drag);

	kb_group = createkeyboardgroup();
	wl_list_init(&kb_group->destroy.link);

	output_mgr = wlr_output_manager_v1_create(dpy);
	wl_signal_add(&output_mgr->events.apply, &output_mgr_apply);
	wl_signal_add(&output_mgr->events.test, &output_mgr_test);

	/* status bar support: dwl-ipc (workspaces/tags) + foreign-toplevel (windows) */
	wl_list_init(&ipc_outputs);
	wl_global_create(dpy, &zdwl_ipc_manager_v2_interface, 2, NULL, ipcmgrbind);
	ftl_mgr = wlr_foreign_toplevel_manager_v1_create(dpy);

	/* Make sure XWayland clients don't connect to the parent X server,
	 * e.g when running in the x11 backend or the wayland backend and the
	 * compositor has Xwayland support */
	unsetenv("DISPLAY");
#ifdef XWAYLAND
	/*
	 * Initialise the XWayland X server.
	 * It will be started when the first X client is started.
	 */
	if ((xwayland = wlr_xwayland_create(dpy, compositor, 1))) {
		wl_signal_add(&xwayland->events.ready, &xwayland_ready);
		wl_signal_add(&xwayland->events.new_surface, &new_xwayland_surface);

		setenv("DISPLAY", xwayland->display_name, 1);
	} else {
		fprintf(stderr, "failed to setup XWayland X server, continuing without it\n");
	}
#endif
}

void
spawn(const Arg *arg)
{
	if (fork() == 0) {
		dup2(STDERR_FILENO, STDOUT_FILENO);
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		die("gluewc: execvp %s failed:", ((char **)arg->v)[0]);
	}
}

void
startdrag(struct wl_listener *listener, void *data)
{
	struct wlr_drag *drag = data;
	if (!drag->icon)
		return;

	drag->icon->data = &wlr_scene_drag_icon_create(drag_icon, drag->icon)->node;
	LISTEN_STATIC(&drag->icon->events.destroy, destroydragicon);
}

void
reloadconfig(const Arg *arg)
{
	Client *c;
	Monitor *m;
	struct xkb_context *context;
	struct xkb_keymap *keymap;
	size_t i;

	for (i = 0; i < nautostarts; i++)
		free(autostarts[i]);
	nautostarts = 0;
	free(runkeys);
	free(runnormalkeys);
	runkeys = runnormalkeys = NULL;
	nrunkeys = nrunnormalkeys = 0;
	readconfig();
	wl_list_for_each(m, &mons, link)
		animstop(m);

	wlr_scene_set_blur_data(scene, blur_passes, blur_radius, 0.02f, 0.9f, 0.9f, 1.1f);
	wlr_scene_rect_set_color(root_bg, rootcolor);

	if ((context = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		if ((keymap = xkb_keymap_new_from_names(context, &xkb_rules,
				XKB_KEYMAP_COMPILE_NO_FLAGS))) {
			wlr_keyboard_set_keymap(&kb_group->wlr_group->keyboard, keymap);
			xkb_keymap_unref(keymap);
		}
		xkb_context_unref(context);
	}
	wlr_keyboard_set_repeat_info(&kb_group->wlr_group->keyboard,
			repeat_rate, repeat_delay);

	wl_list_for_each(c, &clients, link) {
		if (!c->isfullscreen)
			c->bw = decorhidden ? 0 : borderpx;
		client_set_border_color(c, bordercolor);
		applyeffects(c);
		if (c->isfloating)
			resize(c, c->geom, 0);
	}
	focusclient(focustop(selmon), 1);
	wl_list_for_each(m, &mons, link)
		arrange(m);
}

void
setratio(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || !sel->node || !sel->node->par)
		return;
	sel->node->par->ratio += arg->f;
	if (sel->node->par->ratio < 0.1f)
		sel->node->par->ratio = 0.1f;
	if (sel->node->par->ratio > 0.9f)
		sel->node->par->ratio = 0.9f;
	arrange(selmon);
}

void
swapnodes(Client *sel, Client *t)
{
	Node *a = sel->node, *b = t->node;
	a->c = t;
	b->c = sel;
	sel->node = b;
	t->node = a;
	arrange(selmon);
	warpto(sel);
}

void
swapdir(const Arg *arg)
{
	Client *sel = focustop(selmon), *t;
	if (!sel || sel->isfloating || sel->isfullscreen || sel->isfakefull || !sel->node)
		return;
	if (!(t = dirpick(selmon, sel, arg->i)) || !t->node)
		return;
	swapnodes(sel, t);
}

void
swapstack(const Arg *arg)
{
	/* swap with the next (+1) or previous (-1) leaf in tree order */
	Client *sel = focustop(selmon);
	Node *n;
	if (!sel || sel->isfullscreen || sel->isfakefull || !sel->node || !selmon)
		return;
	n = arg->i > 0 ? bsp_nextleaf(selmon->tree[sel->ws], sel->node)
			: bsp_prevleaf(selmon->tree[sel->ws], sel->node);
	if (n && n->c != sel)
		swapnodes(sel, n->c);
}

void
toggledecor(const Arg *arg)
{
	Client *c;
	Monitor *m;
	decorhidden ^= 1;
	wl_list_for_each(c, &clients, link) {
		if (!c->isfullscreen)
			c->bw = decorhidden ? 0 : borderpx;
		applyeffects(c);
		if (c->isfloating)
			resize(c, c->geom, 0);
	}
	wl_list_for_each(m, &mons, link)
		arrange(m);
}

void
togglefakefullscreen(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel)
		return;
	if (sel->isfullscreen)
		setfullscreen(sel, 0);
	sel->isfakefull ^= 1;
	applyeffects(sel);
	if (sel->isfakefull) {
		sel->prev = sel->geom;
		if (sel->isfloating)
			setfloating(sel, 0);
	} else if (sel->isfloating) {
		resize(sel, sel->prev, 0);
	}
	arrange(selmon);
}

void
togglefloating(const Arg *arg)
{
	Client *sel = focustop(selmon);
	/* return if fullscreen */
	if (!sel || sel->isfullscreen || sel->isfakefull)
		return;
	if (!sel->isfloating && selmon) {
		/* float centered at 3/5 of the window area, like nvwm */
		struct wlr_box b;
		b.width = MAX(50, selmon->w.width * 3 / 5);
		b.height = MAX(50, selmon->w.height * 3 / 5);
		b.x = selmon->w.x + (selmon->w.width - b.width) / 2;
		b.y = selmon->w.y + (selmon->w.height - b.height) / 2;
		sel->isfloating = 1;
		resize(sel, b, 0);
		setfloating(sel, 1);
	} else {
		setfloating(sel, !sel->isfloating);
	}
}

void
togglefullscreen(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (sel)
		setfullscreen(sel, !sel->isfullscreen);
}

void
togglesplit(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || !sel->node || !sel->node->par)
		return;
	sel->node->par->horiz ^= 1;
	arrange(selmon);
}

void
viewws(const Arg *arg)
{
	Client *c;
	unsigned int oldws, forward, backward;
	int animate, dir, started = 0;

	if (!selmon || selmon->ws == arg->ui)
		return;
	wlr_log(WLR_DEBUG, "viewws: %u -> %u", selmon->ws, arg->ui);
	oldws = selmon->ws;
	forward = (arg->ui + NUMWS - oldws) % NUMWS;
	backward = (oldws + NUMWS - arg->ui) % NUMWS;
	dir = forward <= backward ? 1 : -1;
	animate = animations && animation_duration > 0 && !overview_visible;
	if (animate) {
		animstop(selmon);
		arrange(selmon);
		wl_list_for_each(c, &clients, link) {
			if (c->mon != selmon || c->ws != oldws
					|| !c->scene->node.enabled || client_is_unmanaged(c))
				continue;
			c->anim.from.x = c->scene->node.x;
			c->anim.from.y = c->scene->node.y;
			c->anim.to.x = c->geom.x - dir * selmon->m.width;
			c->anim.to.y = c->geom.y;
			c->anim.fadein = 0;
			c->anim.workspace = 1;
			c->anim.hide = 1;
			c->anim.active = 1;
			c->anim.t = 0.0f;
			started = 1;
		}
	}
	selmon->ws = arg->ui;
	if (!overview_visible)
		focusclient(focustop(selmon), 1);
	arrange(selmon);
	if (animate) {
		wl_list_for_each(c, &clients, link) {
			if (c->mon != selmon || !VISIBLEON(c, selmon)
					|| !c->scene->node.enabled || client_is_unmanaged(c))
				continue;
			c->anim.from.x = c->geom.x + dir * selmon->m.width;
			c->anim.from.y = c->geom.y;
			c->anim.to.x = c->geom.x;
			c->anim.to.y = c->geom.y;
			c->anim.fadein = 0;
			c->anim.workspace = 1;
			c->anim.hide = 0;
			c->anim.active = 1;
			c->anim.t = 0.0f;
			wlr_scene_node_set_position(&c->scene->node, c->anim.from.x, c->anim.from.y);
			started = 1;
		}
		if (started)
			animkick(selmon);
	}
}

void
movetows(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || !selmon || sel->ws == arg->ui || !sel->node)
		return;
	bsp_detach(sel);
	bsp_attach(selmon, arg->ui, sel);
	focusclient(focustop(selmon), 1);
	arrange(selmon);
}

void
movetowsfollow(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || !selmon || sel->ws == arg->ui || !sel->node)
		return;
	bsp_detach(sel);
	bsp_attach(selmon, arg->ui, sel);
	viewws(arg);
}

void
wsstep(const Arg *arg)
{
	Arg a;
	if (!selmon)
		return;
	a.ui = (selmon->ws + NUMWS + arg->i) % NUMWS;
	viewws(&a);
}

void
movewsstep(const Arg *arg)
{
	Arg a;
	if (!selmon)
		return;
	a.ui = (selmon->ws + NUMWS + arg->i) % NUMWS;
	movetowsfollow(&a);
}

void
warpto(Client *c)
{
	if (!warpcursor || !c)
		return;
	wlr_cursor_warp_closest(cursor, NULL,
			c->geom.x + c->geom.width / 2.0,
			c->geom.y + c->geom.height / 2.0);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
unlocksession(struct wl_listener *listener, void *data)
{
	SessionLock *lock = wl_container_of(listener, lock, unlock);
	destroylock(lock, 1);
}

void
unmaplayersurfacenotify(struct wl_listener *listener, void *data)
{
	LayerSurface *l = wl_container_of(listener, l, unmap);

	if (!l->anim.closing && layeranimated(l))
		l->anim.closing = closeanimstart(&l->scene->node,
				layers[layermap[l->layer_surface->current.layer]], l->mon);
	l->anim.active = 0;
	l->anim.closing = 0;
	layeropacity(l, 1.0f);
	l->mapped = 0;
	wlr_scene_node_set_enabled(&l->scene->node, 0);
	if (l == exclusive_focus)
		exclusive_focus = NULL;
	if (l->layer_surface->output && (l->mon = l->layer_surface->output->data))
		arrangelayers(l->mon);
	if (l->layer_surface->surface == seat->keyboard_state.focused_surface)
		focusclient(focustop(selmon), 1);
	motionnotify(0, NULL, 0, 0, 0, 0);
}

void
unmapnotify(struct wl_listener *listener, void *data)
{
	/* Called when the surface is unmapped, and should no longer be shown. */
	Client *c = wl_container_of(listener, c, unmap);
	clientcloseanim(c);
	c->anim.closing = 0;
	if (c == grabc) {
		cursor_mode = CurNormal;
		grabc = NULL;
	}
	if (c == overview_drag_client) {
		overview_drag_client = NULL;
		overview_dragging = 0;
		overviewdragclear();
	}
	if (c == overview_focus_client)
		overview_focus_client = NULL;

	if (c->ftl) {
		wl_list_remove(&c->ftl_activate.link);
		wl_list_remove(&c->ftl_close.link);
		wlr_foreign_toplevel_handle_v1_destroy(c->ftl);
		c->ftl = NULL;
	}
	c->surfbuf = NULL;

	if (client_is_unmanaged(c)) {
		if (c == exclusive_focus) {
			exclusive_focus = NULL;
			focusclient(focustop(selmon), 1);
		}
	} else {
		wl_list_remove(&c->link);
		setmon(c, NULL, 0);
		wl_list_remove(&c->flink);
	}

	wlr_scene_node_destroy(&c->scene->node);
	c->border.top = c->border.bottom = NULL;
	c->border.left = c->border.right = NULL;
	c->border.top_left = c->border.top_right = NULL;
	c->border.bottom_right = c->border.bottom_left = NULL;
	motionnotify(0, NULL, 0, 0, 0, 0);
	ipcnotifyall();
	if (overview_visible)
		overviewbuild();
}

void
updatemons(struct wl_listener *listener, void *data)
{
	/*
	 * Called whenever the output layout changes: adding or removing a
	 * monitor, changing an output's mode or position, etc. This is where
	 * the change officially happens and we update geometry, window
	 * positions, focus, and the stored configuration in wlroots'
	 * output-manager implementation.
	 */
	struct wlr_output_configuration_v1 *config
			= wlr_output_configuration_v1_create();
	Client *c;
	struct wlr_output_configuration_head_v1 *config_head;
	Monitor *m;

	/* First remove from the layout the disabled monitors */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled || m->asleep)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);
		config_head->state.enabled = 0;
		/* Remove this output from the layout to avoid cursor enter inside it */
		wlr_output_layout_remove(output_layout, m->wlr_output);
		closemon(m);
		m->m = m->w = (struct wlr_box){0};
	}
	/* Insert outputs that need to */
	wl_list_for_each(m, &mons, link) {
		if (m->wlr_output->enabled
				&& !wlr_output_layout_get(output_layout, m->wlr_output))
			wlr_output_layout_add_auto(output_layout, m->wlr_output);
	}

	/* Now that we update the output layout we can get its box */
	wlr_output_layout_get_box(output_layout, NULL, &sgeom);

	wlr_scene_node_set_position(&root_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(root_bg, sgeom.width, sgeom.height);

	/* Make sure the clients are hidden when gluewc is locked */
	wlr_scene_node_set_position(&locked_bg->node, sgeom.x, sgeom.y);
	wlr_scene_rect_set_size(locked_bg, sgeom.width, sgeom.height);

	wl_list_for_each(m, &mons, link) {
		if (!m->wlr_output->enabled)
			continue;
		config_head = wlr_output_configuration_head_v1_create(config, m->wlr_output);

		/* Get the effective monitor geometry to use for surfaces */
		wlr_output_layout_get_box(output_layout, m->wlr_output, &m->m);
		m->w = m->m;
		wlr_scene_output_set_position(m->scene_output, m->m.x, m->m.y);

		wlr_scene_node_set_position(&m->fullscreen_bg->node, m->m.x, m->m.y);
		wlr_scene_rect_set_size(m->fullscreen_bg, m->m.width, m->m.height);

		if (m->lock_surface) {
			struct wlr_scene_tree *scene_tree = m->lock_surface->surface->data;
			wlr_scene_node_set_position(&scene_tree->node, m->m.x, m->m.y);
			wlr_session_lock_surface_v1_configure(m->lock_surface, m->m.width, m->m.height);
		}

		/* Calculate the effective monitor geometry to use for clients */
		arrangelayers(m);
		/* Don't move clients to the left output when plugging monitors */
		arrange(m);
		/* make sure fullscreen clients have the right size */
		if ((c = focustop(m)) && c->isfullscreen)
			resize(c, m->m, 0);

		/* Try to re-set the gamma LUT when updating monitors,
		 * it's only really needed when enabling a disabled output, but meh. */
		m->gamma_lut_changed = 1;

		config_head->state.x = m->m.x;
		config_head->state.y = m->m.y;

		if (!selmon) {
			selmon = m;
		}
	}

	if (selmon && selmon->wlr_output->enabled) {
		wl_list_for_each(c, &clients, link) {
			if (!c->mon && client_surface(c)->mapped)
				setmon(c, selmon, c->ws);
		}
		if (!overview_visible)
			focusclient(focustop(selmon), 1);
		if (selmon->lock_surface) {
			client_notify_enter(selmon->lock_surface->surface,
					wlr_seat_get_keyboard(seat));
			client_activate_surface(selmon->lock_surface->surface, 1);
		}
	}

	/* FIXME: figure out why the cursor image is at 0,0 after turning all
	 * the monitors on.
	 * Move the cursor image where it used to be. It does not generate a
	 * wl_pointer.motion event for the clients, it's only the image what it's
	 * at the wrong position after all. */
	wlr_cursor_move(cursor, NULL, 0, 0);

	wlr_output_manager_v1_set_configuration(output_mgr, config);
	if (overview_visible)
		overviewrelayout();
}

void
updatetitle(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, set_title);
	const char *title = client_get_title(c);
	if (c->ftl && title)
		wlr_foreign_toplevel_handle_v1_set_title(c->ftl, title);
	ipcnotifyall();
}

void
virtualkeyboard(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_keyboard_v1 *kb = data;
	/* virtual keyboards shouldn't share keyboard group */
	KeyboardGroup *group = createkeyboardgroup();
	/* Set the keymap to match the group keymap */
	wlr_keyboard_set_keymap(&kb->keyboard, group->wlr_group->keyboard.keymap);
	LISTEN(&kb->keyboard.base.events.destroy, &group->destroy, destroykeyboardgroup);

	/* Add the new keyboard to the group */
	wlr_keyboard_group_add_keyboard(group->wlr_group, &kb->keyboard);
}

void
virtualpointer(struct wl_listener *listener, void *data)
{
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;
	struct wlr_input_device *device = &event->new_pointer->pointer.base;

	wlr_cursor_attach_input_device(cursor, device);
	if (event->suggested_output)
		wlr_cursor_map_input_to_output(cursor, device, event->suggested_output);
}

Monitor *
xytomon(double x, double y)
{
	struct wlr_output *o = wlr_output_layout_output_at(output_layout, x, y);
	return o ? o->data : NULL;
}

void
xytonode(double x, double y, struct wlr_surface **psurface,
		Client **pc, LayerSurface **pl, double *nx, double *ny)
{
	struct wlr_scene_node *node, *pnode;
	struct wlr_surface *surface = NULL;
	Client *c = NULL;
	LayerSurface *l = NULL;
	int layer;

	for (layer = NUM_LAYERS - 1; !surface && layer >= 0; layer--) {
		if (!(node = wlr_scene_node_at(&layers[layer]->node, x, y, nx, ny)))
			continue;

		if (node->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_surface *scene_surface =
					wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(node));
			if (scene_surface)
				surface = scene_surface->surface;
		}
		/* Walk the tree to find a node that knows the client */
		for (pnode = node; pnode && !c;
				pnode = pnode->parent ? &pnode->parent->node : NULL)
			c = pnode->data;
		if (c && c->type == LayerShell) {
			c = NULL;
			l = pnode ? pnode->data : NULL;
		}
	}

	if (psurface) *psurface = surface;
	if (pc) *pc = c;
	if (pl) *pl = l;
}

#ifdef XWAYLAND
void
activatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, activate);

	/* Only "managed" windows can be activated */
	if (!client_is_unmanaged(c))
		wlr_xwayland_surface_activate(c->surface.xwayland, 1);
}

void
associatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, associate);

	LISTEN(&client_surface(c)->events.client_commit, &c->precommit,
			precommitnotify);
	LISTEN(&client_surface(c)->events.map, &c->map, mapnotify);
	LISTEN(&client_surface(c)->events.unmap, &c->unmap, unmapnotify);
}

void
configurex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, configure);
	struct wlr_xwayland_surface_configure_event *event = data;
	if (!client_surface(c) || !client_surface(c)->mapped) {
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if (client_is_unmanaged(c)) {
		wlr_scene_node_set_position(&c->scene->node, event->x, event->y);
		wlr_xwayland_surface_configure(c->surface.xwayland,
				event->x, event->y, event->width, event->height);
		return;
	}
	if (c->isfloating && c != grabc) {
		resize(c, (struct wlr_box){.x = event->x - c->bw,
				.y = event->y - c->bw, .width = event->width + c->bw * 2,
				.height = event->height + c->bw * 2}, 0);
	} else {
		arrange(c->mon);
	}
}

void
createnotifyx11(struct wl_listener *listener, void *data)
{
	struct wlr_xwayland_surface *xsurface = data;
	Client *c;

	/* Allocate a Client for this surface */
	c = xsurface->data = ecalloc(1, sizeof(*c));
	c->surface.xwayland = xsurface;
	c->type = X11;
	c->bw = client_is_unmanaged(c) ? 0 : borderpx;

	/* Listen to the various events it can emit */
	LISTEN(&xsurface->events.associate, &c->associate, associatex11);
	LISTEN(&xsurface->events.destroy, &c->destroy, destroynotify);
	LISTEN(&xsurface->events.dissociate, &c->dissociate, dissociatex11);
	LISTEN(&xsurface->events.request_activate, &c->activate, activatex11);
	LISTEN(&xsurface->events.request_configure, &c->configure, configurex11);
	LISTEN(&xsurface->events.request_fullscreen, &c->fullscreen, fullscreennotify);
	LISTEN(&xsurface->events.set_title, &c->set_title, updatetitle);
}

void
dissociatex11(struct wl_listener *listener, void *data)
{
	Client *c = wl_container_of(listener, c, dissociate);
	wl_list_remove(&c->precommit.link);
	wl_list_remove(&c->map.link);
	wl_list_remove(&c->unmap.link);
}

void
xwaylandready(struct wl_listener *listener, void *data)
{
	struct wlr_xcursor *xcursor;

	/* assign the one and only seat */
	wlr_xwayland_set_seat(xwayland, seat);

	/* Set the default XWayland cursor to match the rest of gluewc. */
	if ((xcursor = wlr_xcursor_manager_get_xcursor(cursor_mgr, "default", 1)))
		wlr_xwayland_set_cursor(xwayland,
				xcursor->images[0]->buffer, xcursor->images[0]->width * 4,
				xcursor->images[0]->width, xcursor->images[0]->height,
				xcursor->images[0]->hotspot_x, xcursor->images[0]->hotspot_y);
}
#endif

int
main(int argc, char *argv[])
{
	char *startup_cmd = NULL;
	int c;

	while ((c = getopt(argc, argv, "s:hdv")) != -1) {
		if (c == 's')
			startup_cmd = optarg;
		else if (c == 'd')
			log_level = WLR_DEBUG;
		else if (c == 'v')
			die("gluewc " VERSION);
		else
			goto usage;
	}
	if (optind < argc)
		goto usage;

	/* Wayland requires XDG_RUNTIME_DIR for creating its communications socket */
	if (!getenv("XDG_RUNTIME_DIR"))
		die("XDG_RUNTIME_DIR must be set");
	setup();
	run(startup_cmd);
	cleanup();
	return EXIT_SUCCESS;

usage:
	die("Usage: %s [-v] [-d] [-s startup command]", argv[0]);
}
