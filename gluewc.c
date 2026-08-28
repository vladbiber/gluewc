/*
 * See LICENSE file for copyright and license details.
 */
#include <getopt.h>
#include <sys/inotify.h>
#include <limits.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
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
#include <scenefx/types/fx/clipped_region.h>
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

#include "xdg-shell-protocol.h"
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
#define SCROLLLT(M)             ((M) && (M)->lt == LtScroll)
#define LTMIGRATE(C, M, WS)     ((C)->mon == (M) && (C)->ws == (WS) \
                                && client_surface(C)->mapped && !client_is_unmanaged(C))
#define DRIFTLT(M)              ((M) && (M)->lt == LtDrift)
#define RESIZEWAIT              300 /* ms an unacked resize may hold up a frame */

/* enums */
enum { CurNormal, CurPressed, CurMove, CurResize,
       CurDragTile, CurResizeCol, CurDriftMove, CurDriftResize,
       CurDriftPan, CurBSPMove, CurBSPResize }; /* cursor */
enum { LtBSP, LtScroll, LtDrift, LtLast }; /* per-monitor layouts */
enum { XDGShell, LayerShell, X11 }; /* client types */
/* LyrOverview sits above the windows but below LyrTop, so the bar stays
 * visible over the overview while a fullscreen client (LyrFS) still covers it */
enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrOverview, LyrTop, LyrFS, LyrOverlay, LyrBlock, NUM_LAYERS }; /* scene layers */
enum { ModeInsert, ModeNormal }; /* input modes */
enum { DirLeft, DirRight, DirUp, DirDown }; /* focus/swap directions */
enum { AnimNone, AnimFade, AnimSlide, AnimZoom }; /* open/close animation */

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
	struct wlr_box box; /* area this node was last tiled into, gaps included */
};

/* one column of the niri-style scrolling layout */
typedef struct Column Column;
struct Column {
	struct wl_list link;    /* Monitor.cols[ws] */
	struct wl_list clients; /* Client.clink, top to bottom */
	float wfrac;            /* width as a fraction of the window area */
	float prevfrac;         /* width to restore after a maximize toggle */
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
	struct wlr_scene_blur *blur; /* backdrop blur behind that buffer */
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
	Column *col;          /* scroll layout column, NULL while in a BSP tree */
	int canvassized;      /* canvas holds the window's real size, not a screen box */
	struct wl_list clink; /* position inside Column.clients */
	struct wlr_box canvas; /* drift layout: unscaled canvas rect, includes border */
	int oncanvas;          /* 1 while the client lives on a drift canvas */
	int isfloating, isfullscreen, isfakefull;
	struct {
		int active, fadein, workspace, hide, closing;
		int zoom;    /* the open animation also scales the window up */
		float scale; /* scale currently painted on the tree, 1 = untouched */
		float t; /* progress 0..1, advanced only on rendered frames */
		struct wlr_box from, to; /* only x/y are used */
	} anim;
	uint32_t resize; /* configure serial of a pending resize */
	uint32_t resizeat; /* when that serial was sent, in ms */
	double bufscale; /* scale painted on this client's buffers, 1 = untouched */
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
	unsigned int lt; /* LtBSP, LtScroll (niri-style) or LtDrift (infinite canvas) */
	struct wl_list cols[NUMWS]; /* Column.link per workspace (scroll layout) */
	int scrollx[NUMWS]; /* horizontal scroll offset per workspace */
	/* drift layout: one camera over an infinite canvas per workspace */
	double camx[NUMWS], camy[NUMWS], camz[NUMWS];
	int driftscaled; /* the visible windows are currently drawn zoomed */
	int gamma_lut_changed;
	uint32_t lastanimtick;
	struct wl_event_source *unblock; /* wakes the output when a client stalls */
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
static float animease(const float *curve, float t);
static void animkick(Monitor *m);
static void animstop(Monitor *m);
static void applybounds(Client *c, struct wlr_box *bbox);
static void applyeffects(Client *c);
static void applyfakefull(Client *c);
static void applyrules(Client *c);
static void arrange(Monitor *m);
static void arrangelayer(Monitor *m, struct wl_list *list,
		struct wlr_box *usable_area, int exclusive);
static void arrangelayers(Monitor *m);
static void attachclient(Monitor *m, unsigned int ws, Client *c);
static void axisnotify(struct wl_listener *listener, void *data);
static void bsp_attach(Monitor *m, unsigned int ws, Client *c);
static void bsp_detach(Client *c);
static void bsp_dragto(Client *c, double cx, double cy);
static Node *bsp_findsplit(Node *n, int horiz, int after);
static Node *bsp_first(Node *n);
static int bsp_has_tiled(Node *n);
static Node *bsp_nextleaf(Node *tree, Node *cur);
static Node *bsp_prevleaf(Node *tree, Node *cur);
static void bsp_resizeto(Client *c, double cx, double cy);
static void bsp_tile(Node *n, struct wlr_box box);
static void buttonpress(struct wl_listener *listener, void *data);
static void chvt(const Arg *arg);
static void checkidleinhibitor(struct wlr_surface *exclude);
static void clientborders(Client *c, int width, int height, int bw, int radius);
static float clientopacity(Client *c);
static void clientscale(Client *c, float z);
static void clientunscale(Client *c);
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
static void consumewin(const Arg *arg);
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
static void detachclient(Client *c);
static Client *dirpick(Monitor *m, Client *from, int dir);
static void driftapply(Monitor *m);
static void driftarrange(Monitor *m);
static void driftattach(Monitor *m, unsigned int ws, Client *c);
static void driftbounds(Monitor *m, unsigned int ws, int withview, struct wlr_box *out);
static int driftcluster(Client *c, Client **out, int max);
static void driftdetach(Client *c);
static void driftdragto(Client *c, double cx, double cy);
static void driftfit(const Arg *arg);
static void driftmove(Client *c, int dx, int dy, int cluster);
static void driftnudge(const Arg *arg);
static void driftpan(const Arg *arg);
static void driftpanto(double cx, double cy);
static void driftpankey(const Arg *arg);
static Client *driftpick(Monitor *m, Client *from, int dir);
static void driftresizeto(Client *c, double cx, double cy);
static void driftreveal(Monitor *m, Client *c);
static void driftscaleclient(Client *c, double z);
static void driftscreen(Monitor *m, Client *c, struct wlr_box *out);
static void driftsnap(Monitor *m, Client *c, struct wlr_box *box);
static void drifttile(Monitor *m);
static int drifttiled(Client *c, Monitor *m, unsigned int ws);
static double driftz(Monitor *m, unsigned int ws);
static void driftzoomkey(const Arg *arg);
static void driftzoomto(Monitor *m, double z, double ax, double ay);
static void entermode(const Arg *arg);
static void expelwin(const Arg *arg);
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
static void movewsdir(const Arg *arg);
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
static void overviewfinish(void);
static void overviewset(int active);
static int overviewpassthrough(void);
static int overviewkeypassthrough(void);
static void saveoverviewstate(int active);
static void overviewtoggle(const Arg *arg);
static int overviewvalid(Monitor *m);
static void workspacestep(int dir);
static void gesturebegin(uint32_t fingers);
static void gestureupdate(uint32_t fingers, double dx, double dy);
static void swipebeginnotify(struct wl_listener *listener, void *data);
static void swipeendnotify(struct wl_listener *listener, void *data);
static void swipeupdatenotify(struct wl_listener *listener, void *data);
static void pointerfocus(Client *c, struct wlr_surface *surface,
		double sx, double sy, uint32_t time);
static void powermgrsetmode(struct wl_listener *listener, void *data);
static void quit(const Arg *arg);
static void readconfig(void);
static void cfgerror(const char *fmt, ...);
static int cfgerrorhide(void *data);
static void cfgerrorshow(void);
static void cfgwatch(void);
static int cfgwatchevent(int fd, uint32_t mask, void *data);
static int cfgwatchfire(void *data);
static void reloadconfig(const Arg *arg);
static void rendermon(struct wl_listener *listener, void *data);
static void requestdecorationmode(struct wl_listener *listener, void *data);
static void requeststartdrag(struct wl_listener *listener, void *data);
static void requestmonstate(struct wl_listener *listener, void *data);
static void resize(Client *c, struct wlr_box geo, int interact);
static void run(char *startup_cmd);
static Column *scroll_addcol(Monitor *m, unsigned int ws, struct wl_list *pos);
static void scroll_addclient(Column *col, Client *c, unsigned int ws);
static void scroll_attach(Monitor *m, unsigned int ws, Client *c);
static int scroll_coltiled(Column *col);
static int scroll_colwidth(Monitor *m, Column *col);
static void scroll_detach(Client *c);
static void scroll_dragto(Client *c, double cx);
static void scroll_maximize(Client *sel);
static Client *scroll_next(Monitor *m, Client *sel);
static void scroll_swap(Client *sel, int dir);
static void scroll_tile(Monitor *m);
static void swipefocus(int dir);
static void overviewfocuscol(int dir);
static void setcursor(struct wl_listener *listener, void *data);
static void setcursorshape(struct wl_listener *listener, void *data);
static void setfloating(Client *c, int floating);
static void setfullscreen(Client *c, int fullscreen);
static void setlayout(Monitor *m, unsigned int lt);
static int layoutstatepath(char *buf, size_t size);
static void loadlayoutstate(void);
static void savelayoutstate(unsigned int lt);
static void setlayoutarg(const Arg *arg);
static void pinchbeginnotify(struct wl_listener *listener, void *data);
static void pinchendnotify(struct wl_listener *listener, void *data);
static void pinchupdatenotify(struct wl_listener *listener, void *data);
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
static void togglelayout(const Arg *arg);
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
static Client *wsfocused(Monitor *m, unsigned int ws);
static Client *wsfocusedany(Monitor *m, unsigned int ws);
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
static struct wl_event_source *overview_watchdog;
/* config file watch: the file is reloaded as soon as an editor closes it, and
 * anything the parser rejected is reported instead of only reaching the log */
static int cfg_wd = -1;
static struct wl_event_source *cfg_watch_src, *cfg_watch_delay;
static struct wlr_scene_rect *cfg_errbar;
static struct wl_event_source *cfg_errbar_timer;
static int cfg_nerrors;
static int cfg_errline, cfg_curline;
static char cfg_errmsg[192];
static char cfg_path[512];
static int overview_button_swallow;
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
static double grab_startx; /* cursor x at the start of a resize or canvas pan */
static double grab_starty; /* cursor y at the start of a resize or canvas pan */
static float grab_wfrac;   /* column width when the resize started */
static Node *bsp_grabh, *bsp_grabv; /* splits a bsp drag-resize is moving */
static float bsp_grabhr, bsp_grabvr; /* their ratios when the drag started */
static Monitor *grabm;     /* monitor whose camera a canvas pan is dragging */
static double grab_camx, grab_camy; /* camera position when the pan started */
static int lt_migrating;   /* set while windows move between layouts */
static int drift_dragcluster; /* the running drag moves the whole snapped cluster */
static double drift_pinch_zoom, drift_pinch_x, drift_pinch_y;
static int drift_pinch_done;

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
static struct wl_listener cursor_pinch_begin = {.notify = pinchbeginnotify};
static struct wl_listener cursor_pinch_end = {.notify = pinchendnotify};
static struct wl_listener cursor_pinch_update = {.notify = pinchupdatenotify};
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
static int
monunblock(void *data)
{
	/* a client sat on its configure long enough to freeze the output: come
	 * back and draw the frame without it */
	Monitor *m = data;

	if (m->wlr_output->enabled)
		wlr_output_schedule_frame(m->wlr_output);
	return 0;
}

float
animease(const float *curve, float t)
{
	/* CSS cubic-bezier(x1, y1, x2, y2): the curve runs from (0,0) to (1,1),
	 * so x(u) is solved for the wanted time and y(u) is the eased progress.
	 * Newton converges in a couple of steps for the usual control points. */
	float u = t, x, dx, mu;
	int i;

	if (t <= 0.0f)
		return 0.0f;
	if (t >= 1.0f)
		return 1.0f;
	for (i = 0; i < 8; i++) {
		mu = 1.0f - u;
		x = 3.0f * mu * mu * u * curve[0] + 3.0f * mu * u * u * curve[2]
				+ u * u * u;
		dx = 3.0f * mu * mu * curve[0]
				+ 6.0f * mu * u * (curve[2] - curve[0])
				+ 3.0f * u * u * (1.0f - curve[2]);
		if (fabsf(x - t) < 0.0005f || fabsf(dx) < 1e-5f)
			break;
		u -= (x - t) / dx;
		u = u < 0.0f ? 0.0f : u > 1.0f ? 1.0f : u;
	}
	mu = 1.0f - u;
	return 3.0f * mu * mu * u * curve[1] + 3.0f * mu * u * u * curve[3]
			+ u * u * u;
}

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
		c->anim.zoom = 0;
		clientunscale(c);
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
clientborders(Client *c, int width, int height, int bw, int radius)
{
	/* the four edges are square and the corners carry the radius, so a
	 * transparent client keeps a real rounded outline */
	int horizontal, vertical;

	if (!c->border.top)
		return;
	horizontal = MAX(0, width - 2 * radius);
	vertical = MAX(0, height - 2 * radius);

	wlr_scene_rect_set_size(c->border.top, horizontal, bw);
	wlr_scene_node_set_position(&c->border.top->node, radius, 0);
	wlr_scene_rect_set_size(c->border.bottom, horizontal, bw);
	wlr_scene_node_set_position(&c->border.bottom->node, radius,
			MAX(0, height - bw));
	wlr_scene_rect_set_size(c->border.left, bw, vertical);
	wlr_scene_node_set_position(&c->border.left->node, 0, radius);
	wlr_scene_rect_set_size(c->border.right, bw, vertical);
	wlr_scene_node_set_position(&c->border.right->node,
			MAX(0, width - bw), radius);

	wlr_scene_rect_set_size(c->border.top_left, radius, radius);
	wlr_scene_node_set_position(&c->border.top_left->node, 0, 0);
	wlr_scene_rect_set_size(c->border.top_right, radius, radius);
	wlr_scene_node_set_position(&c->border.top_right->node,
			MAX(0, width - radius), 0);
	wlr_scene_rect_set_size(c->border.bottom_right, radius, radius);
	wlr_scene_node_set_position(&c->border.bottom_right->node,
			MAX(0, width - radius), MAX(0, height - radius));
	wlr_scene_rect_set_size(c->border.bottom_left, radius, radius);
	wlr_scene_node_set_position(&c->border.bottom_left->node, 0,
			MAX(0, height - radius));

	if (c->blur) {
		wlr_scene_blur_set_size(c->blur, MAX(0, width - 2 * bw),
				MAX(0, height - 2 * bw));
		wlr_scene_blur_set_corner_radius(c->blur, MAX(0, radius - bw));
		wlr_scene_node_set_position(&c->blur->node, bw, bw);
	}
}

/* SceneFX 0.5 keeps the backdrop blur in a node of its own that is masked by
 * the buffer it belongs to, so a cloned buffer needs a cloned blur node. */
static void
cloneblur(struct wlr_scene_buffer *src, struct wlr_scene_buffer *clone,
		struct wlr_scene_tree *tree, int width, int height,
		struct fx_corner_radii corners)
{
	struct wlr_scene_blur *blur;

	if (!linked_nodes_get_sibling(&src->blur)
			|| !(blur = wlr_scene_blur_create(tree, width, height)))
		return;
	wlr_scene_blur_set_corner_radii(blur, corners);
	wlr_scene_blur_set_transparency_mask_source(blur, clone);
	wlr_scene_node_set_position(&blur->node, clone->node.x, clone->node.y);
	wlr_scene_node_place_below(&blur->node, &clone->node);
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
		wlr_scene_rect_set_corner_radii(c->border.top, corner_radii_none());
		wlr_scene_rect_set_corner_radii(c->border.bottom, corner_radii_none());
		wlr_scene_rect_set_corner_radii(c->border.left, corner_radii_none());
		wlr_scene_rect_set_corner_radii(c->border.right, corner_radii_none());
		wlr_scene_rect_set_corner_radii(c->border.top_left, corner_radii_new(r, 0, 0, 0));
		wlr_scene_rect_set_corner_radii(c->border.top_right, corner_radii_new(0, r, 0, 0));
		wlr_scene_rect_set_corner_radii(c->border.bottom_right, corner_radii_new(0, 0, r, 0));
		wlr_scene_rect_set_corner_radii(c->border.bottom_left, corner_radii_new(0, 0, 0, r));
	}
	if (c->surfbuf) {
		wlr_scene_buffer_set_corner_radius(c->surfbuf, MAX(0, r - (int)c->bw));
		if (!c->anim.fadein)
			wlr_scene_buffer_set_opacity(c->surfbuf, clientopacity(c));
	}
	if (c->blur) {
		/* masking the blur with the surface keeps it inside the parts the
		 * client actually paints, the way ignore_transparent used to */
		wlr_scene_blur_set_transparency_mask_source(c->blur, c->surfbuf);
		wlr_scene_node_set_enabled(&c->blur->node, blurenabled && !fs);
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
	wlr_scene_buffer_set_corner_radii(b->scene, buffer->corners);
	wlr_scene_buffer_set_opaque_region(b->scene, &buffer->opaque_region);
	b->opacity = buffer->opacity;
	b->x = sx - a->x;
	b->y = sy - a->y;
	b->width = width;
	b->height = height;
	wlr_scene_node_set_position(&b->scene->node, b->x, b->y);
	cloneblur(buffer, b->scene, a->tree, width, height, buffer->corners);
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

	if (!animations || animation_type_close == AnimNone
			|| animation_duration_close <= 0 || overview_visible
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
	int cx, cy, duration, slide, pending = 0;

	duration = MAX(60, animation_duration_close);
	/* zoom collapses the window towards its own centre, slide drops it out
	 * of the way; both fade, and a plain fade holds the window still */
	slide = animation_type_close == AnimSlide
			? MAX(16, MIN(64, (m->w.height ? m->w.height : 720) / 24)) : 0;
	wl_list_for_each_safe(a, tmp, &close_anims, link) {
		if (a->mon != m)
			continue;
		a->t += duration > 0 ? dt / (float)duration : 1.0f;
		if (a->t >= 1.0f) {
			closeanimdestroy(a);
			continue;
		}
		pending = 1;
		e = animease(animation_curve_close, a->t);
		scale = animation_type_close == AnimZoom
				? 1.0f - (1.0f - zoom_end_ratio) * e : 1.0f;
		cx = (a->minx + a->maxx) / 2;
		cy = (a->miny + a->maxy) / 2;
		wlr_scene_node_set_position(&a->tree->node, a->x,
				a->y + (int)roundf(slide * e));
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
	if (!animations || animation_type_open == AnimNone
			|| animation_duration_open <= 0 || overview_visible
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

	duration = MAX(60, animation_duration_open);
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
			e = animease(animation_curve_open, l->anim.t);
			opacity = e;
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

	/* a fullscreen client (real or fake) is shown alone on its workspace;
	 * in the scroll layout fake fullscreen leaves the strip mapped beneath
	 * so it just grows over it and shrinks back, like niri */
	wl_list_for_each(c, &clients, link) {
		if (!fs && VISIBLEON(c, m) && (c->isfullscreen || c->isfakefull))
			fs = c;
	}

	wl_list_for_each(c, &clients, link) {
		if (c->mon == m) {
			int vis = VISIBLEON(c, m) && (!fs || c == fs
					|| (SCROLLLT(m) && fs->isfakefull));
			int render = vis || (c->anim.workspace && c->anim.hide);
			wlr_scene_node_set_enabled(&c->scene->node, render);
			client_set_suspended(c, !vis);
		}
	}

	wlr_scene_node_set_enabled(&m->fullscreen_bg->node, fs && fs->isfullscreen);

	if (fs && fs->isfakefull && SCROLLLT(m) && !fs->isfloating) {
		scroll_tile(m);
		resize(fs, m->m, 0); /* cover the whole output */
	} else if (fs) {
		if (fs->isfakefull)
			resize(fs, m->w, 0);
	} else if (SCROLLLT(m)) {
		scroll_tile(m);
	} else if (DRIFTLT(m)) {
		drifttile(m);
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

	/* Nothing above the shell wants the keyboard any more. Without this the
	 * grab would stick after e.g. the overview dock closes its search box,
	 * and every later key would go to a surface that stopped asking for it. */
	if (exclusive_focus) {
		exclusive_focus = NULL;
		if (overview_visible)
			wlr_seat_keyboard_notify_clear_focus(seat);
		else
			focusclient(focustop(m), 1);
	}
}

void
axisnotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_axis_event *event = data;
	Monitor *m;
	double delta;
	int dir;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	delta = event->delta != 0.0 ? event->delta : event->delta_discrete;
	dir = delta > 0.0 ? 1 : delta < 0.0 ? -1 : 0;
	if (!locked && !overview_visible && dir
			&& DRIFTLT((m = xytomon(cursor->x, cursor->y)))) {
		struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
		struct wlr_surface *surface = NULL;
		double amount = event->relative_direction
				== WL_POINTER_AXIS_RELATIVE_DIRECTION_INVERTED ? -delta : delta;
		double z = driftz(m, m->ws);
		int super = super_group && super_group->super_down;

		if (super && kb && (wlr_keyboard_get_modifiers(kb) & WLR_MODIFIER_SHIFT)) {
			/* Mod+Shift+wheel zooms at the pointer; plain Mod+wheel keeps
			 * changing workspace, like in the other layouts */
			super_group->super_alone = 0;
			driftzoomto(m, z * (dir > 0 ? 1.0 / drift_zoom_step : drift_zoom_step),
					cursor->x, cursor->y);
			return;
		}
		if (!super) {
			xytonode(cursor->x, cursor->y, &surface, NULL, NULL, NULL, NULL);
			if (!surface) {
				/* over bare canvas, scrolling pans it; over a window the
				 * event belongs to the client */
				if (event->source != WL_POINTER_AXIS_SOURCE_FINGER)
					amount *= 4.0;
				if (event->orientation == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
					m->camx[m->ws] += amount * drift_pan_speed / z;
				else
					m->camy[m->ws] += amount * drift_pan_speed / z;
				driftarrange(m);
				return;
			}
		}
	}
	if (super_group && super_group->super_down
			&& (event->source == WL_POINTER_AXIS_SOURCE_WHEEL
				|| event->source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT)) {
		super_group->super_alone = 0;
		if (dir)
			workspacestep(dir);
		return;
	}
	/* over the shell's own surfaces the wheel belongs to them - the overview
	 * dock scrolls its app grid with it - so only steal it elsewhere */
	if (overview_visible && !overviewpassthrough()) {
		if (!overview_active)
			return;
		if (event->source == WL_POINTER_AXIS_SOURCE_WHEEL
				|| event->source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT) {
			if (dir)
				workspacestep(dir);
			return;
		}
		/* two-finger scrolling stays out of workspace switching; the
		 * overview is walked with three fingers, the wheel or the keys */
		return;
	}
	wlr_seat_pointer_notify_axis(seat,
			event->time_msec, event->orientation, event->delta,
			event->delta_discrete, event->source, event->relative_direction);
}

void
swipefocus(int dir)
{
	/* directional focus from a touchpad gesture: no cursor warp */
	Arg a = {.i = dir};
	int oldwarp = warpcursor;

	warpcursor = 0;
	focusdir(&a);
	warpcursor = oldwarp;
}

void
gesturebegin(uint32_t fingers)
{
	Monitor *m;

	wlr_idle_notifier_v1_notify_activity(idle_notifier, seat);
	/* three and four fingers only: two-finger scrolling belongs to the
	 * clients, and in the drift layout to the canvas */
	overview_swipe_active = !locked && (fingers == 3 || fingers == 4);
	overview_swipe_triggered = 0;
	overview_swipe_dx = overview_swipe_dy = 0.0;
	if (!overview_swipe_active)
		return;
	m = xytomon(cursor->x, cursor->y);
	if (overviewvalid(m))
		selmon = m;
}

void
swipebeginnotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_begin_event *event = data;

	gesturebegin(event->fingers);
}

void
gestureupdate(uint32_t fingers, double dx, double dy)
{
	if (!overview_swipe_active || fingers < 3 || fingers > 4
			|| !overviewvalid(selmon))
		return;
	overview_swipe_dx += dx;
	overview_swipe_dy += dy;

	if (fingers == 4) {
		/* four fingers walk the workspaces and open the overview in every
		 * layout, which is where drift keeps them since three fingers pan */
		const double step = 120.0;

		if (fabs(overview_swipe_dx) > fabs(overview_swipe_dy) * 1.2) {
			for (; overview_swipe_dx <= -step; overview_swipe_dx += step) {
				overview_swipe_dy = 0.0;
				workspacestep(1);
			}
			for (; overview_swipe_dx >= step; overview_swipe_dx -= step) {
				overview_swipe_dy = 0.0;
				workspacestep(-1);
			}
		} else if (!overview_swipe_triggered && fabs(overview_swipe_dy) >= 64.0) {
			overview_swipe_triggered = 1;
			overviewset(overview_swipe_dy < 0.0);
		}
		return;
	}

	if (fingers == 3 && DRIFTLT(selmon) && !overview_visible) {
		/* three fingers pan the canvas, the content following the fingers */
		double z = driftz(selmon, selmon->ws);

		selmon->camx[selmon->ws] -= dx * drift_pan_speed / z;
		selmon->camy[selmon->ws] -= dy * drift_pan_speed / z;
		driftarrange(selmon);
		return;
	}

	if (SCROLLLT(selmon)) {
		/* niri-style gestures: horizontal swipes walk the windows on the
		 * strip, vertical swipes change workspace; every swipe_step
		 * pixels repeats the action so a long swipe keeps going */
		const double step = 140.0;

		if (fabs(overview_swipe_dx) > fabs(overview_swipe_dy) * 1.2) {
			for (; overview_swipe_dx <= -step; overview_swipe_dx += step) {
				overview_swipe_dy = 0.0;
				if (overview_active)
					overviewfocuscol(1);
				else if (!overview_visible)
					swipefocus(DirRight);
			}
			for (; overview_swipe_dx >= step; overview_swipe_dx -= step) {
				overview_swipe_dy = 0.0;
				if (overview_active)
					overviewfocuscol(-1);
				else if (!overview_visible)
					swipefocus(DirLeft);
			}
		} else if (fabs(overview_swipe_dy) > fabs(overview_swipe_dx) * 1.2) {
			for (; overview_swipe_dy <= -step; overview_swipe_dy += step) {
				overview_swipe_dx = 0.0;
				workspacestep(1);
			}
			for (; overview_swipe_dy >= step; overview_swipe_dy -= step) {
				overview_swipe_dx = 0.0;
				workspacestep(-1);
			}
		}
		return;
	}

	if (overview_swipe_triggered)
		return;
	if (fabs(overview_swipe_dx) >= 48.0
			&& fabs(overview_swipe_dx) > fabs(overview_swipe_dy) * 1.2) {
		overview_swipe_triggered = 1;
		workspacestep(overview_swipe_dx < 0.0 ? 1 : -1);
	} else if (fingers == 3 && fabs(overview_swipe_dy) >= 48.0
			&& fabs(overview_swipe_dy) > fabs(overview_swipe_dx) * 1.2) {
		overview_swipe_triggered = 1;
		overviewset(overview_swipe_dy < 0.0);
	}
}

void
swipeupdatenotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_swipe_update_event *event = data;

	gestureupdate(event->fingers, event->dx, event->dy);
}

void
swipeendnotify(struct wl_listener *listener, void *data)
{
	overview_swipe_active = 0;
	overview_swipe_triggered = 0;
	overview_swipe_dx = overview_swipe_dy = 0.0;
}

void
pinchbeginnotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_pinch_begin_event *event = data;
	Monitor *m;

	drift_pinch_done = 0;
	drift_pinch_x = cursor->x;
	drift_pinch_y = cursor->y;
	gesturebegin(event->fingers);
	m = xytomon(cursor->x, cursor->y);
	if (!locked && overviewvalid(m))
		selmon = m;
	drift_pinch_zoom = DRIFTLT(selmon) ? driftz(selmon, selmon->ws) : 1.0;
}

void
pinchupdatenotify(struct wl_listener *listener, void *data)
{
	struct wlr_pointer_pinch_update_event *event = data;

	if (locked || !overviewvalid(selmon) || event->scale <= 0.0)
		return;
	if (event->fingers >= 3) {
		/* libinput reports a three-finger swipe as a pinch as soon as the
		 * fingers drift apart a little, so the movement is fed to the swipe
		 * handler instead of the zoom — otherwise the gesture would either
		 * do nothing or zoom by accident */
		if (event->fingers >= 4 && !drift_pinch_done) {
			if (event->scale <= 0.75) {
				drift_pinch_done = 1;
				overviewset(1);
			} else if (event->scale >= 1.3) {
				drift_pinch_done = 1;
				overviewset(0);
			}
		}
		gestureupdate(event->fingers, event->dx, event->dy);
		return;
	}
	if (overview_visible || !DRIFTLT(selmon))
		return;
	/* a two-finger pinch zooms the camera around the pinch centre */
	driftzoomto(selmon, drift_pinch_zoom * event->scale,
			drift_pinch_x, drift_pinch_y);
}

void
pinchendnotify(struct wl_listener *listener, void *data)
{
	drift_pinch_done = 0;
	overview_swipe_active = 0;
	overview_swipe_triggered = 0;
	overview_swipe_dx = overview_swipe_dy = 0.0;
}

void
bsp_attach(Monitor *m, unsigned int ws, Client *c)
{
	Node *leaf, *t = NULL, *sp;
	Client *f;

	c->canvas.width = c->canvas.height = 0;
	c->canvassized = 0; /* the window is no longer sized by a canvas */
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
	/* a running drag-resize must not keep pointing at a freed split */
	if (bsp_grabh == n)
		bsp_grabh = NULL;
	if (bsp_grabv == n)
		bsp_grabv = NULL;
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

void
bsp_dragto(Client *c, double cx, double cy)
{
	/* bsp drag: hand the window the tile it is dragged onto instead of
	 * floating it.  arrange() can bounce back into motionnotify(), so this
	 * has to survive re-entry. */
	static int busy;
	Monitor *m = c->mon;
	Client *t, *target = NULL;
	Node *a, *b;

	if (busy || !m || !c->node || c->isfloating)
		return;
	wl_list_for_each(t, &clients, link) {
		if (t == c || !t->node || t->isfloating || t->isfullscreen
				|| !VISIBLEON(t, m))
			continue;
		if (cx >= t->geom.x && cx < t->geom.x + t->geom.width
				&& cy >= t->geom.y && cy < t->geom.y + t->geom.height) {
			target = t;
			break;
		}
	}
	if (!target)
		return;
	a = c->node;
	b = target->node;
	a->c = target;
	b->c = c;
	c->node = b;
	target->node = a;
	busy = 1;
	arrange(m);
	busy = 0;
}

Node *
bsp_findsplit(Node *n, int horiz, int after)
{
	/* Nearest ancestor split of the requested orientation whose dividing
	 * line lies after (right of / below) n, or before it when after is 0.
	 * Splits with nothing tiled on one side are skipped: bsp_tile() hands
	 * the whole box to the other side there, so their ratio does nothing.
	 * A split on the wrong side is kept as a fallback for windows that sit
	 * against the screen edge. */
	Node *p, *fallback = NULL;

	for (; n && n->par; n = n->par) {
		p = n->par;
		if (!p->horiz != !horiz || !bsp_has_tiled(p->a) || !bsp_has_tiled(p->b))
			continue;
		if ((p->a == n) == !!after)
			return p;
		if (!fallback)
			fallback = p;
	}
	return fallback;
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
bsp_resizeto(Client *c, double cx, double cy)
{
	/* bsp right-drag: move the split lines the grab started on, so the
	 * window resizes inside the tree and its neighbours give up the room */
	static int busy;
	float r;
	int changed = 0;

	if (busy || !c->mon || !c->node || c->isfloating)
		return;
	if (bsp_grabh && bsp_grabh->box.width > 0) {
		r = bsp_grabhr + (float)((cx - grab_startx) / bsp_grabh->box.width);
		r = r < 0.05f ? 0.05f : r > 0.95f ? 0.95f : r;
		changed |= fabsf(r - bsp_grabh->ratio) >= 0.001f;
		bsp_grabh->ratio = r;
	}
	if (bsp_grabv && bsp_grabv->box.height > 0) {
		r = bsp_grabvr + (float)((cy - grab_starty) / bsp_grabv->box.height);
		r = r < 0.05f ? 0.05f : r > 0.95f ? 0.95f : r;
		changed |= fabsf(r - bsp_grabv->ratio) >= 0.001f;
		bsp_grabv->ratio = r;
	}
	if (!changed)
		return;
	busy = 1;
	arrange(c->mon);
	busy = 0;
}

void
bsp_tile(Node *n, struct wlr_box box)
{
	/* Hiding borders is a visual choice; keep the tile spacing intact. */
	int gap = gappx;

	if (!n)
		return;
	n->box = box; /* remembered so a drag-resize can scale its own split */
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

Column *
scroll_addcol(Monitor *m, unsigned int ws, struct wl_list *pos)
{
	Column *col = ecalloc(1, sizeof(Column));

	wl_list_init(&col->clients);
	col->wfrac = scroll_colfrac;
	wl_list_insert(pos, &col->link);
	return col;
}

void
scroll_addclient(Column *col, Client *c, unsigned int ws)
{
	wl_list_insert(col->clients.prev, &c->clink);
	c->col = col;
	c->ws = ws;
}

void
scroll_attach(Monitor *m, unsigned int ws, Client *c)
{
	/* a new window opens in its own column right of the focused one, like niri */
	Column *after = NULL;
	Client *f;

	c->canvas.width = c->canvas.height = 0;
	c->canvassized = 0; /* the window is no longer sized by a canvas */

	if (lt_migrating) {
		/* rebuilding the strip from another layout: keep tiling order */
		scroll_addclient(scroll_addcol(m, ws, m->cols[ws].prev), c, ws);
		return;
	}
	wl_list_for_each(f, &fstack, flink) {
		if (f != c && f->mon == m && f->ws == ws && f->col) {
			after = f->col;
			break;
		}
	}
	scroll_addclient(scroll_addcol(m, ws,
			after ? &after->link : m->cols[ws].prev), c, ws);
}

void
scroll_detach(Client *c)
{
	Column *col = c->col;

	if (!col)
		return;
	wl_list_remove(&c->clink);
	c->col = NULL;
	if (wl_list_empty(&col->clients)) {
		wl_list_remove(&col->link);
		free(col);
	}
}

int
scroll_coltiled(Column *col)
{
	Client *c;
	int n = 0;

	wl_list_for_each(c, &col->clients, clink) {
		if (!c->isfloating)
			n++;
	}
	return n;
}

int
scroll_colwidth(Monitor *m, Column *col)
{
	int w = (int)(m->w.width * col->wfrac);

	return MAX(2 * gappx + 50, MIN(w, m->w.width));
}

Client *
wsfocused(Monitor *m, unsigned int ws)
{
	Client *c;

	wl_list_for_each(c, &fstack, flink) {
		if (c->mon == m && c->ws == ws && c->col && !c->isfloating)
			return c;
	}
	return NULL;
}

Client *
wsfocusedany(Monitor *m, unsigned int ws)
{
	Client *c;

	wl_list_for_each(c, &fstack, flink) {
		if (c->mon == m && c->ws == ws)
			return c;
	}
	return NULL;
}

void
scroll_tile(Monitor *m)
{
	Column *col, *fcol;
	Client *c, *focus = wsfocused(m, m->ws);
	int gap = gappx;
	int total = 0, x, colw, n, i, h;
	int *sx = &m->scrollx[m->ws];

	fcol = focus ? focus->col : NULL;

	/* total strip width, then scroll just enough to reveal the focused
	 * column — the columns themselves never move, only the viewport */
	wl_list_for_each(col, &m->cols[m->ws], link) {
		if (scroll_coltiled(col))
			total += scroll_colwidth(m, col);
	}
	if (total <= m->w.width) {
		*sx = 0;
	} else {
		x = 0;
		wl_list_for_each(col, &m->cols[m->ws], link) {
			if (!scroll_coltiled(col))
				continue;
			colw = scroll_colwidth(m, col);
			if (col == fcol) {
				if (x < *sx)
					*sx = x;
				else if (x + colw > *sx + m->w.width)
					*sx = x + colw - m->w.width;
				break;
			}
			x += colw;
		}
		*sx = MAX(0, MIN(*sx, total - m->w.width));
	}

	x = 0;
	wl_list_for_each(col, &m->cols[m->ws], link) {
		if (!(n = scroll_coltiled(col)))
			continue;
		colw = scroll_colwidth(m, col);
		h = m->w.height / n;
		i = 0;
		wl_list_for_each(c, &col->clients, clink) {
			if (c->isfloating)
				continue;
			resize(c, (struct wlr_box){
				.x = m->w.x + x - *sx + gap,
				.y = m->w.y + i * h + gap,
				.width = MAX(1, colw - 2 * gap),
				.height = MAX(1, (i == n - 1 ? m->w.height - i * h : h)
					- 2 * gap)}, 0);
			wlr_log(WLR_DEBUG, "scroll: ws=%u %dx%d%+d%+d sx=%d", c->ws,
					c->geom.width, c->geom.height,
					c->geom.x, c->geom.y, *sx);
			i++;
		}
		x += colw;
	}
}

Client *
scroll_next(Monitor *m, Client *sel)
{
	/* next client in strip order, wrapping like bsp_nextleaf */
	Column *col = sel->col;
	struct wl_list *cl = sel->clink.next;
	Client *c;

	for (;;) {
		if (cl != &col->clients) {
			c = wl_container_of(cl, c, clink);
			return c;
		}
		/* end of the column: hop to the next one (empty columns are freed
		 * on detach, so any wrapped-to column has at least one client) */
		col = col->link.next == &m->cols[sel->ws]
				? wl_container_of(m->cols[sel->ws].next, col, link)
				: wl_container_of(col->link.next, col, link);
		cl = col->clients.next;
	}
}

void
scroll_dragto(Client *c, double cx)
{
	/* niri-style drag: reorder the strip live under the cursor instead of
	 * floating the window */
	Monitor *m = c->mon;
	Column *col, *own, *target = NULL;
	int x = 0, colw = 0, tx = 0;
	double lx;

	if (!m || !c->col || c->isfloating)
		return;
	lx = cx - m->w.x + m->scrollx[c->ws];
	wl_list_for_each(col, &m->cols[c->ws], link) {
		if (!scroll_coltiled(col))
			continue;
		colw = scroll_colwidth(m, col);
		if (lx < x + colw) {
			target = col;
			tx = x;
			break;
		}
		x += colw;
	}
	if (target == c->col)
		return;
	/* pull the window out of a shared column first */
	if (wl_list_length(&c->col->clients) > 1) {
		own = scroll_addcol(m, c->ws, &c->col->link);
		wl_list_remove(&c->clink);
		wl_list_insert(own->clients.prev, &c->clink);
		c->col = own;
	}
	if (!target) {
		if (c->col->link.next == &m->cols[c->ws])
			return;
		wl_list_remove(&c->col->link);
		wl_list_insert(m->cols[c->ws].prev, &c->col->link);
	} else if (lx < tx + colw / 2) {
		if (c->col->link.next == &target->link)
			return;
		wl_list_remove(&c->col->link);
		wl_list_insert(target->link.prev, &c->col->link);
	} else {
		if (c->col->link.prev == &target->link)
			return;
		wl_list_remove(&c->col->link);
		wl_list_insert(&target->link, &c->col->link);
	}
	arrange(m);
}

void
scroll_maximize(Client *sel)
{
	/* niri maximize-column: make the column screen-wide, and back to the
	 * width it had before */
	Column *col = sel->col;

	if (col->wfrac < 0.995f) {
		col->prevfrac = col->wfrac;
		col->wfrac = 1.0f;
	} else {
		col->wfrac = col->prevfrac > 0.0f ? col->prevfrac : scroll_colfrac;
	}
	arrange(sel->mon);
}

void
scroll_swap(Client *sel, int dir)
{
	Column *col = sel->col;
	struct wl_list *pos;

	if (dir == DirLeft || dir == DirRight) {
		/* move the whole column across, like niri */
		pos = dir == DirLeft ? col->link.prev : col->link.next;
		if (pos == &sel->mon->cols[sel->ws])
			return;
		wl_list_remove(&col->link);
		wl_list_insert(dir == DirLeft ? pos->prev : pos, &col->link);
	} else {
		pos = dir == DirUp ? sel->clink.prev : sel->clink.next;
		if (pos == &col->clients)
			return;
		wl_list_remove(&sel->clink);
		wl_list_insert(dir == DirUp ? pos->prev : pos, &sel->clink);
	}
	arrange(sel->mon);
	warpto(sel);
}

/* ---- drift: a driftwm-style infinite canvas, one per workspace ----
 * Windows keep the size they asked for and sit at fixed canvas coordinates.
 * Every workspace owns a camera (pan + zoom) over its canvas; zooming scales
 * the rendered surfaces, so clients never learn about it and keep their
 * native size. */

double
driftz(Monitor *m, unsigned int ws)
{
	double z = m->camz[ws] > 0.0 ? m->camz[ws] : 1.0;

	return MIN((double)drift_zoom_max, MAX((double)drift_zoom_min, z));
}

int
drifttiled(Client *c, Monitor *m, unsigned int ws)
{
	return c->mon == m && c->ws == ws && c->oncanvas && !c->isfloating;
}

void
driftscreen(Monitor *m, Client *c, struct wlr_box *out)
{
	/* canvas rect -> screen rect; borders keep their pixel width at any zoom */
	double z = driftz(m, c->ws);
	int bw = 2 * (int)c->bw;

	out->x = m->w.x + (int)round((c->canvas.x - m->camx[c->ws]) * z);
	out->y = m->w.y + (int)round((c->canvas.y - m->camy[c->ws]) * z);
	out->width = MAX(1 + bw, (int)round((c->canvas.width - bw) * z) + bw);
	out->height = MAX(1 + bw, (int)round((c->canvas.height - bw) * z) + bw);
}

static int
driftoverlap(const struct wlr_box *a, const struct wlr_box *b, int margin)
{
	return a->x - margin < b->x + b->width && b->x - margin < a->x + a->width
			&& a->y - margin < b->y + b->height && b->y - margin < a->y + a->height;
}

void
driftattach(Monitor *m, unsigned int ws, Client *c)
{
	struct wlr_box box, t;
	Client *o;
	double z = driftz(m, ws);
	int bw = 2 * (int)c->bw, ring, dx, dy, stepx, stepy, taken;

	c->ws = ws;
	c->oncanvas = 1;
	if (lt_migrating && c->geom.width > 0 && c->geom.height > 0) {
		/* coming from another layout: stay exactly where the window is now */
		c->canvas.width = MAX(1 + bw, (int)round((c->geom.width - bw) / z) + bw);
		c->canvas.height = MAX(1 + bw, (int)round((c->geom.height - bw) / z) + bw);
		c->canvas.x = (int)round((c->geom.x - m->w.x) / z + m->camx[ws]);
		c->canvas.y = (int)round((c->geom.y - m->w.y) / z + m->camy[ws]);
		c->canvassized = client_surface(c)->mapped;
		return;
	}

	if (!c->canvassized || c->canvas.width <= 0 || c->canvas.height <= 0) {
		/* arriving from another layout or freshly mapped: there the
		 * geometry is the real size.  A window that already lives on a
		 * canvas keeps its own size instead — c->geom would be the
		 * zoomed screen box, which would shrink it on every move. */
		c->canvas.width = MAX(1 + bw, c->geom.width);
		c->canvas.height = MAX(1 + bw, c->geom.height);
	}
	c->canvassized = client_surface(c)->mapped;
	/* start centred in the viewport, then spiral outwards over a grid of
	 * the window's own size until the spot is free */
	stepx = c->canvas.width + MAX(1, gappx);
	stepy = c->canvas.height + MAX(1, gappx);
	box = c->canvas;
	box.x = (int)round(m->camx[ws] + (m->w.width / z - box.width) / 2.0);
	box.y = (int)round(m->camy[ws] + (m->w.height / z - box.height) / 2.0);
	for (ring = 0; ring < 8; ring++) {
		int bestscore = 0, found = 0;
		struct wlr_box best = box;

		for (dy = -ring; dy <= ring; dy++) {
			for (dx = -ring; dx <= ring; dx++) {
				int score;
				if (ring && MAX(dx < 0 ? -dx : dx, dy < 0 ? -dy : dy) != ring)
					continue;
				/* nearest to the camera wins, and right or below beats
				 * left or above, so windows grow away from the corner */
				score = 4 * ((dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy))
						+ (dx < 0) + (dy < 0);
				if (found && score >= bestscore)
					continue;
				t = box;
				t.x += dx * stepx;
				t.y += dy * stepy;
				taken = 0;
				wl_list_for_each(o, &clients, link) {
					if (o != c && drifttiled(o, m, ws)
							&& driftoverlap(&t, &o->canvas, gappx)) {
						taken = 1;
						break;
					}
				}
				if (!taken) {
					best = t;
					bestscore = score;
					found = 1;
				}
			}
		}
		if (found) {
			c->canvas.x = best.x;
			c->canvas.y = best.y;
			driftreveal(m, c); /* a new window is always brought into view */
			return;
		}
	}
	/* the canvas around the camera is packed solid: cascade instead */
	c->canvas.x = box.x + (wl_list_length(&clients) % 8) * MAX(24, gappx * 3);
	c->canvas.y = box.y + (wl_list_length(&clients) % 8) * MAX(24, gappx * 3);
	driftreveal(m, c);
}

void
driftdetach(Client *c)
{
	c->oncanvas = 0;
}

void
drifttile(Monitor *m)
{
	struct wlr_box box;
	Client *c;

	wl_list_for_each(c, &clients, link) {
		if (!drifttiled(c, m, m->ws))
			continue;
		driftscreen(m, c, &box);
		resize(c, box, 0);
		wlr_log(WLR_DEBUG, "drift: ws=%u %dx%d%+d%+d cam=%.0f,%.0f z=%.2f",
				c->ws, c->canvas.width, c->canvas.height,
				c->canvas.x, c->canvas.y, m->camx[c->ws], m->camy[c->ws],
				driftz(m, c->ws));
	}
}

void
driftbounds(Monitor *m, unsigned int ws, int withview, struct wlr_box *out)
{
	/* the canvas area in use, optionally joined with the current viewport */
	double z = driftz(m, ws);
	Client *c;
	int x1 = 0, y1 = 0, x2 = 0, y2 = 0, first = 1;

	if (withview) {
		x1 = (int)round(m->camx[ws]);
		y1 = (int)round(m->camy[ws]);
		x2 = x1 + MAX(1, (int)round(m->w.width / z));
		y2 = y1 + MAX(1, (int)round(m->w.height / z));
		first = 0;
	}
	wl_list_for_each(c, &clients, link) {
		if (!drifttiled(c, m, ws))
			continue;
		if (first) {
			x1 = c->canvas.x;
			y1 = c->canvas.y;
			x2 = c->canvas.x + c->canvas.width;
			y2 = c->canvas.y + c->canvas.height;
			first = 0;
			continue;
		}
		x1 = MIN(x1, c->canvas.x);
		y1 = MIN(y1, c->canvas.y);
		x2 = MAX(x2, c->canvas.x + c->canvas.width);
		y2 = MAX(y2, c->canvas.y + c->canvas.height);
	}
	if (first) {
		x1 = (int)round(m->camx[ws]);
		y1 = (int)round(m->camy[ws]);
		x2 = x1 + MAX(1, (int)round(m->w.width / z));
		y2 = y1 + MAX(1, (int)round(m->w.height / z));
	}
	*out = (struct wlr_box){.x = x1, .y = y1,
		.width = MAX(1, x2 - x1), .height = MAX(1, y2 - y1)};
}

static void
driftsnapedge(int lo, int size, int tlo, int tsize, int *best, int *out)
{
	/* the four ways our edges can meet theirs: aligned or butted together */
	int cand[4], i, d;

	cand[0] = tlo;
	cand[1] = tlo + tsize;
	cand[2] = tlo - size;
	cand[3] = tlo + tsize - size;
	for (i = 0; i < 4; i++) {
		d = cand[i] > lo ? cand[i] - lo : lo - cand[i];
		if (d <= drift_snap && d < *best) {
			*best = d;
			*out = cand[i];
		}
	}
}

void
driftsnap(Monitor *m, Client *c, struct wlr_box *box)
{
	/* snapping kicks in as edges approach each other, like driftwm */
	double z = driftz(m, c->ws);
	struct wlr_box view;
	Client *o;
	int x = box->x, y = box->y;
	int bestx = drift_snap + 1, besty = drift_snap + 1;

	if (drift_snap <= 0)
		return;
	view.x = (int)round(m->camx[c->ws]);
	view.y = (int)round(m->camy[c->ws]);
	view.width = MAX(1, (int)round(m->w.width / z));
	view.height = MAX(1, (int)round(m->w.height / z));
	driftsnapedge(box->x, box->width, view.x, view.width, &bestx, &x);
	driftsnapedge(box->y, box->height, view.y, view.height, &besty, &y);
	wl_list_for_each(o, &clients, link) {
		if (o == c || !drifttiled(o, m, c->ws))
			continue;
		/* only snap along an axis where the windows actually face each other */
		if (o->canvas.y - drift_snap < box->y + box->height
				&& box->y - drift_snap < o->canvas.y + o->canvas.height)
			driftsnapedge(box->x, box->width, o->canvas.x, o->canvas.width,
					&bestx, &x);
		if (o->canvas.x - drift_snap < box->x + box->width
				&& box->x - drift_snap < o->canvas.x + o->canvas.width)
			driftsnapedge(box->y, box->height, o->canvas.y, o->canvas.height,
					&besty, &y);
	}
	box->x = x;
	box->y = y;
}

int
driftcluster(Client *c, Client **out, int max)
{
	/* windows that touch, directly or through another window, form an
	 * implicit group that moves together */
	Client *o;
	int n = 1, i, added = 1;

	out[0] = c;
	while (added && n < max) {
		added = 0;
		wl_list_for_each(o, &clients, link) {
			if (o == c || !drifttiled(o, c->mon, c->ws))
				continue;
			for (i = 0; i < n; i++)
				if (out[i] == o)
					break;
			if (i < n)
				continue;
			for (i = 0; i < n; i++)
				if (driftoverlap(&o->canvas, &out[i]->canvas, MAX(1, drift_snap)))
					break;
			if (i == n)
				continue;
			out[n++] = o;
			added = 1;
			if (n == max)
				break;
		}
	}
	return n;
}

void
driftmove(Client *c, int dx, int dy, int cluster)
{
	Client *group[32];
	int i, n = 1;

	if (!dx && !dy)
		return;
	group[0] = c;
	if (cluster)
		n = driftcluster(c, group, (int)LENGTH(group));
	for (i = 0; i < n; i++) {
		group[i]->canvas.x += dx;
		group[i]->canvas.y += dy;
	}
}

void
driftreveal(Monitor *m, Client *c)
{
	/* pan just enough to bring the window into the viewport */
	double z, vw, vh, x, y;
	int margin;

	if (!DRIFTLT(m) || !c || !c->oncanvas || c->isfloating)
		return;
	z = driftz(m, c->ws);
	vw = m->w.width / z;
	vh = m->w.height / z;
	x = m->camx[c->ws];
	y = m->camy[c->ws];
	margin = MAX(8, gappx);
	if (c->canvas.x - margin < x)
		x = c->canvas.x - margin;
	else if (c->canvas.x + c->canvas.width + margin > x + vw)
		x = c->canvas.x + c->canvas.width + margin - vw;
	if (c->canvas.y - margin < y)
		y = c->canvas.y - margin;
	else if (c->canvas.y + c->canvas.height + margin > y + vh)
		y = c->canvas.y + c->canvas.height + margin - vh;
	m->camx[c->ws] = x;
	m->camy[c->ws] = y;
}

Client *
driftpick(Monitor *m, Client *from, int dir)
{
	/* nearest window in that direction on the canvas, offscreen ones
	 * included; unlike tiled layouts nothing has to line up */
	Client *c, *best = NULL;
	long bestscore = 0;
	int fx, fy;

	if (!from)
		return NULL;
	fx = from->canvas.x + from->canvas.width / 2;
	fy = from->canvas.y + from->canvas.height / 2;
	wl_list_for_each(c, &clients, link) {
		long primary, perp, score;
		int dx, dy;
		if (c == from || !drifttiled(c, m, m->ws))
			continue;
		dx = c->canvas.x + c->canvas.width / 2 - fx;
		dy = c->canvas.y + c->canvas.height / 2 - fy;
		if (dir == DirLeft || dir == DirRight) {
			primary = dir == DirLeft ? -dx : dx;
			perp = dy < 0 ? -dy : dy;
		} else {
			primary = dir == DirUp ? -dy : dy;
			perp = dx < 0 ? -dx : dx;
		}
		if (primary <= 0 || perp > primary * 2)
			continue;
		score = primary + perp / 2;
		if (!best || score < bestscore) {
			best = c;
			bestscore = score;
		}
	}
	return best;
}

void
driftzoomto(Monitor *m, double z, double ax, double ay)
{
	/* zoom around a screen anchor: the canvas point under it stays put */
	unsigned int ws;
	double old, cx, cy;

	if (!DRIFTLT(m) || m->w.width <= 0 || m->w.height <= 0)
		return;
	ws = m->ws;
	old = driftz(m, ws);
	z = MIN((double)drift_zoom_max, MAX((double)drift_zoom_min, z));
	if (fabs(z - old) < 0.0005)
		return;
	cx = m->camx[ws] + (ax - m->w.x) / old;
	cy = m->camy[ws] + (ay - m->w.y) / old;
	m->camz[ws] = z;
	m->camx[ws] = cx - (ax - m->w.x) / z;
	m->camy[ws] = cy - (ay - m->w.y) / z;
	driftarrange(m);
}

void
driftzoomkey(const Arg *arg)
{
	Monitor *m = selmon;

	double ax, ay;

	if (!DRIFTLT(m))
		return;
	/* zoom around the middle of the viewport: positive zooms in, negative
	 * out, zero goes back to 1:1 */
	ax = m->w.x + m->w.width / 2.0;
	ay = m->w.y + m->w.height / 2.0;
	if (arg->f == 0.0f)
		driftzoomto(m, 1.0, ax, ay);
	else
		driftzoomto(m, driftz(m, m->ws) * (arg->f > 0.0f
				? drift_zoom_step : 1.0f / drift_zoom_step), ax, ay);
}

void
driftfit(const Arg *arg)
{
	/* zoom to fit every window on the workspace, like driftwm's overview */
	Monitor *m = selmon;
	struct wlr_box b;
	double z;
	int margin;

	if (!DRIFTLT(m) || m->w.width <= 0 || m->w.height <= 0)
		return;
	driftbounds(m, m->ws, 0, &b);
	margin = MAX(16, gappx * 2);
	b.x -= margin;
	b.y -= margin;
	b.width += 2 * margin;
	b.height += 2 * margin;
	z = MIN((double)m->w.width / b.width, (double)m->w.height / b.height);
	z = MIN((double)drift_zoom_max, MAX((double)drift_zoom_min, z));
	m->camz[m->ws] = z;
	m->camx[m->ws] = b.x + b.width / 2.0 - m->w.width / (2.0 * z);
	m->camy[m->ws] = b.y + b.height / 2.0 - m->w.height / (2.0 * z);
	driftarrange(m);
}

void
driftpankey(const Arg *arg)
{
	/* Mod+Alt+arrow pans the camera; the other layouts keep swapping windows */
	Monitor *m = selmon;
	double z, stepx, stepy;

	if (!DRIFTLT(m)) {
		swapdir(arg);
		return;
	}
	z = driftz(m, m->ws);
	stepx = m->w.width / z / 4.0;
	stepy = m->w.height / z / 4.0;
	m->camx[m->ws] += arg->i == DirLeft ? -stepx : arg->i == DirRight ? stepx : 0;
	m->camy[m->ws] += arg->i == DirUp ? -stepy : arg->i == DirDown ? stepy : 0;
	driftarrange(m);
}

void
driftpan(const Arg *arg)
{
	/* Mod+Shift+drag grabs the canvas itself and pulls it under the cursor.
	 * The wheel only pans over bare canvas and the three-finger swipe needs a
	 * touchpad, so this is how a plain mouse gets around a covered canvas. */
	Monitor *m = xytomon(cursor->x, cursor->y);

	if (cursor_mode != CurNormal && cursor_mode != CurPressed)
		return;
	if (!DRIFTLT(m))
		return;
	grabc = NULL;
	grabm = m;
	grab_startx = cursor->x;
	grab_starty = cursor->y;
	grab_camx = m->camx[m->ws];
	grab_camy = m->camy[m->ws];
	cursor_mode = CurDriftPan;
	wlr_cursor_set_xcursor(cursor, cursor_mgr, "grabbing");
}

void
driftpanto(double cx, double cy)
{
	/* driftarrange() calls motionnotify() again, so keep this re-entrant
	 * safe and quiet when the camera did not actually move */
	static int busy;
	Monitor *m = grabm;
	double z, camx, camy;

	if (busy || !DRIFTLT(m) || m->w.width <= 0 || m->w.height <= 0)
		return;
	z = driftz(m, m->ws);
	camx = grab_camx - (cx - grab_startx) / z;
	camy = grab_camy - (cy - grab_starty) / z;
	if (camx == m->camx[m->ws] && camy == m->camy[m->ws])
		return;
	m->camx[m->ws] = camx;
	m->camy[m->ws] = camy;
	busy = 1;
	driftarrange(m);
	busy = 0;
}

void
driftnudge(const Arg *arg)
{
	/* Mod+Shift+arrow nudges the window; snapping still applies */
	Client *sel = focustop(selmon);
	struct wlr_box box;

	if (!sel || !drifttiled(sel, selmon, selmon->ws))
		return;
	/* no snapping here: the keyboard is how you place a window precisely,
	 * and a snap wider than the step would swallow every nudge */
	box = sel->canvas;
	box.x += arg->i == DirLeft ? -drift_nudge : arg->i == DirRight ? drift_nudge : 0;
	box.y += arg->i == DirUp ? -drift_nudge : arg->i == DirDown ? drift_nudge : 0;
	driftmove(sel, box.x - sel->canvas.x, box.y - sel->canvas.y, 0);
	driftreveal(selmon, sel);
	arrange(selmon);
}

void
driftdragto(Client *c, double cx, double cy)
{
	/* drag on the canvas: snap to the neighbours and auto-pan at the edges.
	 * arrange() calls motionnotify() again, so this has to be re-entrant
	 * safe and quiet when nothing actually moved */
	static int busy;
	Monitor *m = c->mon;
	struct wlr_box box;
	double z, edge, oldcamx, oldcamy;

	if (busy || !m || !c->oncanvas || c->isfloating)
		return;
	z = driftz(m, c->ws);
	oldcamx = m->camx[c->ws];
	oldcamy = m->camy[c->ws];
	edge = MAX(16.0, MIN(48.0, m->w.width / 24.0));
	if (cx < m->w.x + edge)
		m->camx[c->ws] -= (m->w.x + edge - cx) / z;
	else if (cx > m->w.x + m->w.width - edge)
		m->camx[c->ws] += (cx - (m->w.x + m->w.width - edge)) / z;
	if (cy < m->w.y + edge)
		m->camy[c->ws] -= (m->w.y + edge - cy) / z;
	else if (cy > m->w.y + m->w.height - edge)
		m->camy[c->ws] += (cy - (m->w.y + m->w.height - edge)) / z;

	box = c->canvas;
	box.x = (int)round((cx - grabcx - m->w.x) / z + m->camx[c->ws]);
	box.y = (int)round((cy - grabcy - m->w.y) / z + m->camy[c->ws]);
	driftsnap(m, c, &box);
	if (box.x == c->canvas.x && box.y == c->canvas.y
			&& m->camx[c->ws] == oldcamx && m->camy[c->ws] == oldcamy)
		return;
	driftmove(c, box.x - c->canvas.x, box.y - c->canvas.y, drift_dragcluster);
	busy = 1;
	driftarrange(m);
	busy = 0;
}

void
driftresizeto(Client *c, double cx, double cy)
{
	static int busy;
	Monitor *m = c->mon;
	double z;
	int bw, w, h;

	if (busy || !m || !c->oncanvas || c->isfloating)
		return;
	z = driftz(m, c->ws);
	bw = 2 * (int)c->bw;
	w = MAX(1 + bw, (int)round((cx - c->geom.x - bw) / z) + bw);
	h = MAX(1 + bw, (int)round((cy - c->geom.y - bw) / z) + bw);
	if (w == c->canvas.width && h == c->canvas.height)
		return;
	c->canvas.width = w;
	c->canvas.height = h;
	busy = 1;
	driftarrange(m);
	busy = 0;
}

/* --- rendering the camera zoom ---
 * wlroots has no scale on a scene tree, so the zoom is applied to the client's
 * buffers: the destination size is scaled while the source box wlroots
 * computed is left alone.  Every value below is derived from protocol state,
 * so re-running this over an already scaled tree is a no-op — which is what
 * lets it run again after each commit and on every frame. */
typedef struct {
	struct wlr_surface *surface;
	int x, y;
} DriftSurface;

typedef struct {
	DriftSurface list[24];
	int n;
	double z;
	int ox, oy;          /* where the clip origin lands on screen */
	struct wlr_box clip; /* unscaled, in surface-local coordinates */
} DriftScale;

static void
driftcollect(struct wlr_surface *surface, int sx, int sy, void *data)
{
	DriftScale *ds = data;

	if (ds->n < (int)LENGTH(ds->list)) {
		ds->list[ds->n].surface = surface;
		ds->list[ds->n].x = sx;
		ds->list[ds->n].y = sy;
		ds->n++;
	}
}

/* the hit test wlroots installs on a scene surface, kept so the clip it
 * applies keeps being applied after the scale correction below */
static wlr_scene_buffer_point_accepts_input_func_t wlr_point_accepts_input;

static bool
driftpointinput(struct wlr_scene_buffer *buffer, double *sx, double *sy)
{
	/* wlroots hit-tests a buffer in destination pixels and hands the result
	 * to the client as surface coordinates, which only agree while nothing
	 * is scaled.  A scaled buffer therefore needs the scale divided back
	 * out first, otherwise every click lands zoom times too far into the
	 * window and the input region covers the wrong part of it. */
	struct wlr_scene_surface *s = wlr_scene_surface_try_from_buffer(buffer);
	struct wlr_scene_node *n;
	Client *c = NULL;
	double z = 1.0;

	if (!s)
		return false;
	for (n = &buffer->node; n && !c; n = n->parent ? &n->parent->node : NULL)
		c = n->data;
	if (c && c->type != LayerShell && c->bufscale > 0.0)
		z = c->bufscale;
	*sx /= z;
	*sy /= z;
	return wlr_point_accepts_input ? wlr_point_accepts_input(buffer, sx, sy)
			: wlr_surface_point_accepts_input(s->surface, *sx, *sy);
}

static void
driftscalebuf(struct wlr_scene_buffer *buffer, int sx, int sy, void *data)
{
	DriftScale *ds = data;
	struct wlr_scene_surface *s = wlr_scene_surface_try_from_buffer(buffer);
	int i, x1, y1, x2, y2, tx, ty;

	if (!s)
		return;
	/* every buffer this pass touches gets the scale-aware hit test, and
	 * keeps it: at 1:1 it behaves exactly like the one wlroots installs */
	if (buffer->point_accepts_input
			&& buffer->point_accepts_input != driftpointinput)
		wlr_point_accepts_input = buffer->point_accepts_input;
	buffer->point_accepts_input = driftpointinput;
	for (i = 0; i < ds->n && ds->list[i].surface != s->surface; i++);
	if (i == ds->n)
		return;
	/* the part of this surface that survives the clip, unscaled */
	x1 = MAX(ds->list[i].x, ds->clip.x);
	y1 = MAX(ds->list[i].y, ds->clip.y);
	x2 = MIN(ds->list[i].x + s->surface->current.width,
			ds->clip.x + ds->clip.width);
	y2 = MIN(ds->list[i].y + s->surface->current.height,
			ds->clip.y + ds->clip.height);
	if (x2 <= x1 || y2 <= y1)
		return;
	tx = ds->ox + (int)round((x1 - ds->clip.x) * ds->z);
	ty = ds->oy + (int)round((y1 - ds->clip.y) * ds->z);
	wlr_scene_buffer_set_dest_size(buffer,
			MAX(1, (int)round((x2 - x1) * ds->z)),
			MAX(1, (int)round((y2 - y1) * ds->z)));
	wlr_scene_buffer_set_filter_mode(buffer, WLR_SCALE_FILTER_BILINEAR);
	wlr_scene_node_set_position(&buffer->node,
			buffer->node.x + tx - sx, buffer->node.y + ty - sy);
}

static void
driftscalesurface(struct wlr_scene_tree *tree, struct wlr_surface *surface,
		double z, int ox, int oy, const struct wlr_box *clip)
{
	DriftScale ds = {.n = 0, .z = z, .ox = ox, .oy = oy, .clip = *clip};

	wlr_surface_for_each_surface(surface, driftcollect, &ds);
	wlr_scene_node_for_each_buffer(&tree->node, driftscalebuf, &ds);
}

static void
driftscalepopups(struct wl_list *popups, double z, int ox, int oy)
{
	/* popups hang off the client's tree, so they are scaled separately;
	 * their position comes from the protocol, never from the scene.
	 * ox/oy is the parent's geometry origin inside the tree the popup is
	 * parented to, which is (border, border) for a toplevel's popups and
	 * (0, 0) for a popup of a popup. */
	struct wlr_xdg_popup *p;
	struct wlr_scene_tree *tree;
	struct wlr_box clip;
	int tx, ty;

	wl_list_for_each(p, popups, link) {
		if (!(tree = p->base->surface->data))
			continue;
		clip = p->base->geometry;
		if (clip.width <= 0 || clip.height <= 0)
			continue;
		tx = ox + (int)round(p->current.geometry.x * z);
		ty = oy + (int)round(p->current.geometry.y * z);
		wlr_scene_node_set_position(&tree->node, tx, ty);
		driftscalesurface(tree, p->base->surface, z, tx, ty, &clip);
		driftscalepopups(&p->base->popups, z, 0, 0);
	}
}

void
driftscaleclient(Client *c, double z)
{
	/* wlr_scene_node_for_each_buffer() counts from the parent of the node it
	 * is given, so everything here is relative to the client's scene tree —
	 * which also keeps the open and retile animations working */
	struct wlr_box clip;
	int bw = (int)c->bw;

	if (!c->scene || !c->scene_surface || !client_surface(c)->mapped)
		return;
	client_get_clip(c, &clip);
	/* the clip is the size the client was configured with, borders excluded */
	clip.width = MAX(1, c->canvas.width - 2 * bw);
	clip.height = MAX(1, c->canvas.height - 2 * bw);
	c->bufscale = z;
	driftscalesurface(c->scene_surface, client_surface(c), z, bw, bw, &clip);
#ifdef XWAYLAND
	if (c->type != XDGShell)
		return;
#endif
	driftscalepopups(&c->surface.xdg->popups, z, bw, bw);
}

/* --- scaling a live window for the open animation ---
 * The same trick as the camera zoom: the frame is drawn at the animated size
 * and the buffers are stretched to match, so the client never sees a resize
 * and keeps painting real content while it grows. */
void
clientscale(Client *c, float z)
{
	/* popups are left alone: a window that just mapped has none, and one
	 * that shows up mid-animation is better placed than half scaled */
	struct wlr_box clip;
	int bw, w, h, r;

	if (!c->scene || !c->scene_surface || !client_surface(c)->mapped)
		return;
	if (z >= 1.0f) {
		clientunscale(c);
		return;
	}
	bw = (int)roundf(c->bw * z);
	w = MAX(1 + 2 * bw, (int)roundf(c->geom.width * z));
	h = MAX(1 + 2 * bw, (int)roundf(c->geom.height * z));
	r = (c->isfullscreen || c->isfakefull) ? 0
			: MIN(MAX(0, (int)roundf(corner_radius * z)), MIN(w, h) / 2);
	clientborders(c, w, h, bw, r);
	client_get_clip(c, &clip);
	driftscalesurface(c->scene_surface, client_surface(c), z, bw, bw, &clip);
	c->anim.scale = z;
	c->bufscale = z;
}

void
clientunscale(Client *c)
{
	/* one pass at 1:1 puts every buffer back on the size and position
	 * wlroots computed for it, the same way the camera zoom returns */
	struct wlr_box clip;
	int bw = (int)c->bw, r;

	if ((c->anim.scale == 0.0f || c->anim.scale >= 1.0f) && c->bufscale == 1.0)
		return;
	c->anim.scale = 1.0f;
	c->bufscale = 1.0;
	if (!c->scene || !c->scene_surface || !client_surface(c)->mapped)
		return;
	r = (c->isfullscreen || c->isfakefull) ? 0
			: MIN(MAX(0, corner_radius),
					MIN(c->geom.width, c->geom.height) / 2);
	clientborders(c, c->geom.width, c->geom.height, bw, r);
	client_get_clip(c, &clip);
	driftscalesurface(c->scene_surface, client_surface(c), 1.0, bw, bw, &clip);
}

void
driftapply(Monitor *m)
{
	/* wlroots resets buffer sizes whenever a client commits, so the camera
	 * zoom is re-applied once per frame, before anything reads the buffers */
	Client *c;
	double z;

	if (!DRIFTLT(m))
		return;
	z = driftz(m, m->ws);
	/* at 1:1 the scene graph is already right, except for the one pass that
	 * puts the buffers back after a zoom */
	if (z == 1.0 && !m->driftscaled)
		return;
	wl_list_for_each(c, &clients, link) {
		if (!drifttiled(c, m, m->ws) || !VISIBLEON(c, m)
				|| c->isfullscreen || c->isfakefull)
			continue;
		driftscaleclient(c, z);
	}
	m->driftscaled = z != 1.0;
}

static void
driftarrange(Monitor *m)
{
	/* camera moves are direct manipulation: retile without easing */
	int oldanim = animations;

	animations = 0;
	arrange(m);
	animations = oldanim;
}

void
attachclient(Monitor *m, unsigned int ws, Client *c)
{
	if (SCROLLLT(m))
		scroll_attach(m, ws, c);
	else if (DRIFTLT(m))
		driftattach(m, ws, c);
	else
		bsp_attach(m, ws, c);
}

void
detachclient(Client *c)
{
	/* detach from whichever layout currently holds the client */
	if (c->oncanvas)
		driftdetach(c);
	else if (c->col)
		scroll_detach(c);
	else
		bsp_detach(c);
}

void
setlayout(Monitor *m, unsigned int lt)
{
	/* move every window of every workspace into the new layout, keeping the
	 * order they are in now */
	Client *c, *sel;
	unsigned int ws;

	if (!m || lt >= LtLast || m->lt == lt)
		return;
	sel = focustop(m);
	for (ws = 0; ws < NUMWS; ws++) {
		wl_list_for_each(c, &clients, link) {
			if (LTMIGRATE(c, m, ws) && (c->node || c->col || c->oncanvas))
				detachclient(c);
		}
		m->scrollx[ws] = 0;
		m->camx[ws] = m->camy[ws] = 0.0;
		m->camz[ws] = 1.0;
	}
	m->lt = lt;
	savelayoutstate(lt);
	/* windows are re-attached in tiling order, and the canvas starts out as
	 * a copy of the screen, so nothing jumps around */
	lt_migrating = 1;
	for (ws = 0; ws < NUMWS; ws++) {
		wl_list_for_each(c, &clients, link) {
			if (LTMIGRATE(c, m, ws))
				attachclient(m, ws, c);
		}
	}
	lt_migrating = 0;
	arrange(m);
	ipcnotifyall();
	if (sel) {
		focusclient(sel, 1);
		warpto(sel);
	}
}

void
togglelayout(const Arg *arg)
{
	if (selmon)
		setlayout(selmon, (selmon->lt + 1) % LtLast);
}

void
setlayoutarg(const Arg *arg)
{
	if (selmon)
		setlayout(selmon, arg->ui);
}

void
consumewin(const Arg *arg)
{
	/* pull the first window of the next column into this one (niri consume) */
	Client *sel = focustop(selmon), *t;
	Column *next;

	if (!SCROLLLT(selmon) || !sel || !sel->col)
		return;
	if (sel->col->link.next == &selmon->cols[sel->ws])
		return;
	next = wl_container_of(sel->col->link.next, next, link);
	t = wl_container_of(next->clients.next, t, clink);
	scroll_detach(t);
	scroll_addclient(sel->col, t, sel->ws);
	arrange(selmon);
	warpto(sel);
}

void
expelwin(const Arg *arg)
{
	/* push the focused window out into its own column to the right (niri expel) */
	Client *sel = focustop(selmon);
	Column *col;

	if (!SCROLLLT(selmon) || !sel || !sel->col
			|| wl_list_length(&sel->col->clients) < 2)
		return;
	col = sel->col;
	wl_list_remove(&sel->clink);
	sel->col = NULL;
	scroll_addclient(scroll_addcol(selmon, sel->ws, &col->link), sel, sel->ws);
	arrange(selmon);
	warpto(sel);
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
	/* clicking while Super is held is not a bare Super tap */
	if (super_group && super_group->super_down
			&& event->state == WL_POINTER_BUTTON_STATE_PRESSED)
		super_group->super_alone = 0;
	/* a fresh press over the shell's dock belongs to the dock, not the
	 * overview; once a gesture is under way the overview keeps it */
	if ((overview_visible || overview_button_swallow)
			&& !(!overview_button_swallow && overviewpassthrough())) {
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
			/* Alt bindings exist for driftwm muscle memory; outside the
			 * drift layout Alt+click belongs to the application */
			if ((b->mod & WLR_MODIFIER_ALT) && !DRIFTLT(selmon))
				continue;
			/* there is no camera to pan in the other layouts, so let the
			 * click through instead of swallowing it */
			if (b->func == driftpan && !DRIFTLT(selmon))
				continue;
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
			if (cursor_mode == CurMove || cursor_mode == CurResize) {
				/* Drop the window off on its new monitor */
				selmon = xytomon(cursor->x, cursor->y);
				setmon(grabc, selmon, -1);
			} else if ((cursor_mode == CurDragTile
					|| cursor_mode == CurBSPMove
					|| cursor_mode == CurDriftMove
					|| cursor_mode == CurDriftResize) && grabc) {
				Monitor *dropmon = xytomon(cursor->x, cursor->y);
				if (dropmon && dropmon != grabc->mon) {
					/* a canvas window keeps the spot it was dropped on,
					 * translated onto the other monitor's canvas */
					lt_migrating = DRIFTLT(dropmon) && grabc->oncanvas;
					setmon(grabc, dropmon, -1);
					lt_migrating = 0;
				}
			}
			cursor_mode = CurNormal;
			grabc = NULL;
			grabm = NULL;
			bsp_grabh = bsp_grabv = NULL;
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

	if (grabm == m) {
		grabm = NULL;
		if (cursor_mode == CurDriftPan)
			cursor_mode = CurNormal;
	}

	if (m->unblock) {
		wl_event_source_remove(m->unblock);
		m->unblock = NULL;
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
	wl_list_remove(&cursor_pinch_begin.link);
	wl_list_remove(&cursor_pinch_update.link);
	wl_list_remove(&cursor_pinch_end.link);
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

	if (DRIFTLT(c->mon) && c->oncanvas && !c->isfloating && !c->isfullscreen
			&& !c->isfakefull) {
		/* on the canvas the client owns its size: follow whatever it asked
		 * for, then place that box under the camera again */
		struct wlr_box geo, box;
		int w, h;
		client_get_geometry(c, &geo);
		/* a client that never acks a configure would otherwise hold the
		 * canvas at a size it is not painting at, for as long as it lives */
		if (c->resize && now_ms() - c->resizeat > RESIZEWAIT)
			c->resize = 0;
		if (!c->resize && geo.width > 0 && geo.height > 0) {
			w = geo.width + 2 * (int)c->bw;
			h = geo.height + 2 * (int)c->bw;
			/* Growing to its real size is the first thing a client like a
			 * browser does after mapping, and it would walk out of the
			 * centre it was placed in.  Resizing around the middle keeps
			 * it where it was put, wherever that is. */
			if (c->canvassized) {
				c->canvas.x -= (w - c->canvas.width) / 2;
				c->canvas.y -= (h - c->canvas.height) / 2;
			}
			c->canvas.width = w;
			c->canvas.height = h;
			c->canvassized = 1;
		}
		driftscreen(c->mon, c, &box);
		resize(c, box, 0);
	} else {
		resize(c, c->geom, (c->isfloating && !c->isfullscreen));
	}

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
	for (i = 0; i < NUMWS; i++) {
		wl_list_init(&m->cols[i]);
		m->camz[i] = 1.0;
	}
	m->lt = default_layout >= 0 && default_layout < LtLast
			? (unsigned int)default_layout : LtBSP;

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

	m->unblock = wl_event_loop_add_timer(event_loop, monunblock, m);

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
	c->bufscale = 1.0;

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
	if (DRIFTLT(m) && from->oncanvas && !from->isfloating)
		return driftpick(m, from, dir);
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
	if (SCROLLLT(selmon) && sel
			&& (sel->isfullscreen || sel->isfakefull)) {
		/* niri: moving focus out of fullscreen just resizes the window
		 * back into its column, then the move proceeds */
		if (sel->isfullscreen)
			setfullscreen(sel, 0);
		if (sel->isfakefull) {
			sel->isfakefull = 0;
			applyeffects(sel);
			applyfakefull(sel);
			arrange(selmon);
		}
	}
	if (selmon && (!sel || sel->isfullscreen || sel->isfakefull
			|| !(c = dirpick(selmon, sel, arg->i)))) {
		/* nothing in that direction: step to the neighbouring workspace.
		 * The scroll layout stacks workspaces vertically like niri, so
		 * there the vertical axis changes workspace instead. */
		if (SCROLLLT(selmon)
				? (arg->i == DirUp || arg->i == DirDown)
				: (arg->i == DirLeft || arg->i == DirRight)) {
			Arg a = {.i = (arg->i == DirLeft || arg->i == DirUp) ? -1 : +1};
			wsstep(&a);
		}
		return;
	}
	if (!selmon)
		return;
	focusclient(c, 1);
	if (SCROLLLT(selmon)) {
		arrange(selmon); /* scroll the focused column into view */
	} else if (DRIFTLT(selmon)) {
		driftreveal(selmon, c); /* pan the camera onto the new window */
		driftarrange(selmon);
	}
	warpto(c);
}

void
focusnext(const Arg *arg)
{
	Client *sel = focustop(selmon), *c;
	Node *n;
	if (!sel || sel->isfullscreen || sel->isfakefull || !selmon
			|| (!sel->node && !sel->col && !sel->oncanvas))
		return;
	if (sel->oncanvas) {
		/* walk the windows of this canvas in tiling order */
		struct wl_list *pos = sel->link.next;
		for (;;) {
			if (pos == &clients) {
				pos = clients.next;
				continue;
			}
			c = wl_container_of(pos, c, link);
			if (c == sel)
				return;
			if (drifttiled(c, selmon, selmon->ws))
				break;
			pos = pos->next;
		}
		focusclient(c, 1);
		driftreveal(selmon, c);
		driftarrange(selmon);
		warpto(c);
		return;
	}
	if (sel->col) {
		c = scroll_next(selmon, sel);
		if (c != sel) {
			focusclient(c, 1);
			arrange(selmon);
			warpto(c);
		}
		return;
	}
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
	/* the overview hides every window, so a dock click there would focus
	 * something the user cannot see: step out of it first */
	overviewset(0);
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
	IpcOutput *io = wl_resource_get_user_data(resource);

	if (io && io->mon && index < LtLast)
		setlayout(io->mon, index);
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
	zdwl_ipc_output_v2_send_layout(io->resource, m->lt);
	title = sel ? client_get_title(sel) : NULL;
	appid = sel ? client_get_appid(sel) : NULL;
	zdwl_ipc_output_v2_send_title(io->resource, title ? title : "");
	if (version >= ZDWL_IPC_OUTPUT_V2_APPID_SINCE_VERSION)
		zdwl_ipc_output_v2_send_appid(io->resource, appid ? appid : "");
	if (version >= ZDWL_IPC_OUTPUT_V2_LAYOUT_SYMBOL_SINCE_VERSION)
		zdwl_ipc_output_v2_send_layout_symbol(io->resource,
				m->lt == LtScroll ? "scroll" : m->lt == LtDrift ? "drift" : "bsp");
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
	zdwl_ipc_manager_v2_send_layout(res, "scroll");
	zdwl_ipc_manager_v2_send_layout(res, "drift");
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
	struct fx_corner_radii corners;
	float scale;
	int width, height, dstw, dsth;

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
	dstw = MAX(1, (int)roundf(width * oc->scalex));
	dsth = MAX(1, (int)roundf(height * oc->scaley));
	wlr_scene_buffer_set_dest_size(clone, dstw, dsth);
	wlr_scene_buffer_set_opacity(clone, buffer->opacity * oc->opacity);
	wlr_scene_buffer_set_filter_mode(clone, WLR_SCALE_FILTER_BILINEAR);
	scale = MIN(oc->scalex, oc->scaley);
	corners = oc->radius >= 0 ? corner_radii_all(oc->radius)
			: corner_radii_new((int)roundf(buffer->corners.top_left * scale),
				(int)roundf(buffer->corners.top_right * scale),
				(int)roundf(buffer->corners.bottom_right * scale),
				(int)roundf(buffer->corners.bottom_left * scale));
	wlr_scene_buffer_set_corner_radii(clone, corners);
	wlr_scene_buffer_set_opaque_region(clone, &buffer->opaque_region);
	wlr_scene_node_set_position(&clone->node,
			oc->x + (int)roundf((sx - oc->srcx) * oc->scalex),
			oc->y + (int)roundf((sy - oc->srcy) * oc->scaley));
	cloneblur(buffer, clone, oc->tree, dstw, dsth, corners);
	oc->count++;
}

static struct wlr_box
overviewwindowbox(Monitor *m, Client *c, const struct wlr_box *panel)
{
	struct wlr_box geo = (c->isfullscreen || c->isfakefull) ? c->prev : c->geom;
	float scalex = (float)panel->width / m->w.width;
	float scaley = (float)panel->height / m->w.height;

	if (DRIFTLT(m) && c->oncanvas && !c->isfloating
			&& !c->isfullscreen && !c->isfakefull) {
		/* the card shows the whole canvas, not just the viewport */
		struct wlr_box b;
		float scale;

		driftbounds(m, c->ws, 1, &b);
		scale = MIN((float)panel->width / MAX(1, b.width),
				(float)panel->height / MAX(1, b.height));
		return (struct wlr_box){
			.x = panel->x + (panel->width - (int)roundf(b.width * scale)) / 2
					+ (int)roundf((c->canvas.x - b.x) * scale),
			.y = panel->y + (panel->height - (int)roundf(b.height * scale)) / 2
					+ (int)roundf((c->canvas.y - b.y) * scale),
			.width = MAX(4, (int)roundf(c->canvas.width * scale)),
			.height = MAX(4, (int)roundf(c->canvas.height * scale)),
		};
	}
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
		const struct wlr_box *box, float opacity, int focused)
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
	if (focused && !decorhidden && borderpx > 0) {
		/* focus ring behind the clone, like the live focus border */
		const float *fc = focuscolorfor();
		float bcolor[] = {fc[0] * opacity, fc[1] * opacity,
			fc[2] * opacity, fc[3] * opacity};
		int fbw = MAX(2, (int)roundf(borderpx * scale) + 1);
		struct wlr_scene_rect *ring = wlr_scene_rect_create(tree,
				box->width + 2 * fbw, box->height + 2 * fbw, bcolor);
		wlr_scene_rect_set_corner_radius(ring, radius + fbw);
		wlr_scene_node_set_position(&ring->node, box->x - fbw, box->y - fbw);
	}
	content = wlr_scene_tree_create(tree);
	back = wlr_scene_rect_create(content, box->width, box->height, windowcolor);
	wlr_scene_rect_set_corner_radius(back, radius);
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
		const struct wlr_box *panel, float opacity, int focused)
{
	struct wlr_box box = overviewwindowbox(m, c, panel);

	overviewwindowdraw(tree, c, &box, opacity, focused);
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
	Client *c, *focus = NULL;
	OverviewClone oc;

	if (opacity <= 0.0f)
		return;
	/* the focus ring marks the window the strip is parked on, which only
	 * means something in the scroll layout */
	if (ws == m->ws && SCROLLLT(m))
		focus = overview_focus_client && overview_focus_client->mon == m
				&& overview_focus_client->ws == ws
				? overview_focus_client : wsfocusedany(m, ws);
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
	wlr_scene_rect_set_corner_radius(panel, oc.radius);
	wlr_scene_node_set_position(&panel->node, box->x, box->y);

	wl_list_for_each(l, &m->layers[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND], link) {
		if (l->mapped)
			wlr_scene_node_for_each_buffer(&l->scene->node, overviewclone, &oc);
	}
	shade = wlr_scene_rect_create(tree, box->width, box->height, shadecolor);
	wlr_scene_rect_set_corner_radius(shade, oc.radius);
	wlr_scene_node_set_position(&shade->node, box->x, box->y);

	wl_list_for_each_reverse(c, &clients, link) {
		if (c->mon == m && c->ws == ws
				&& (!overview_dragging || c != overview_drag_client))
			overviewwindow(tree, m, c, box, opacity, c == focus);
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
	if (SCROLLLT(m)) {
		/* niri stacks workspaces vertically: prev above, next below */
		scale = MIN((m->w.width - 2.0f * margin) / m->m.width,
				(m->w.height * 0.72f) / m->m.height);
	} else {
		scale = MIN((m->w.height - 2.0f * margin) / m->m.height,
				(m->w.width * 0.72f) / m->m.width);
	}
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
	if (SCROLLLT(m)) {
		*left = *right = (struct wlr_box){
			.x = m->w.x + (m->w.width - sidewidth) / 2,
			.width = sidewidth, .height = sideheight,
		};
		left->y = center->y - sideheight - gap;
		right->y = center->y + height + gap;
	} else {
		*left = *right = (struct wlr_box){
			.y = m->w.y + (m->w.height - sideheight) / 2,
			.width = sidewidth, .height = sideheight,
		};
		left->x = center->x - sidewidth - gap;
		right->x = center->x + width + gap;
	}
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
overviewwatchdog(void *data)
{
	/* While the overview is up the tiling layers are switched off and only
	 * overviewfinish() turns them back on, so a closing transition that
	 * never reaches its last frame — an output that stopped sending them,
	 * a failed commit — would leave the screen without a single window on
	 * it, nothing to click and nothing to type into.  Never wait forever. */
	Monitor *m;

	if (overview_active || !overview_visible)
		return 0;
	wlr_log(WLR_ERROR, "overview close stalled; closing it by hand");
	wl_list_for_each(m, &mons, link)
		m->overview_animating = 0;
	overviewfinish();
	wl_list_for_each(m, &mons, link)
		if (m->wlr_output->enabled)
			wlr_output_schedule_frame(m->wlr_output);
	return 0;
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
	if (overview_watchdog)
		wl_event_source_timer_update(overview_watchdog, 0);
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
		/* light dim only, so the wallpaper stays visible as the backdrop */
		float dimcolor[] = {0.0f, 0.0f, 0.0f, 0.18f * m->overview_dim};
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
		detachclient(c);
		attachclient(m, ws, c);
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
overviewpassthrough(void)
{
	/* The shell puts its overview dock on a layer surface. While the pointer
	 * is over one, the overview stops swallowing input so the dock can be
	 * hovered and clicked; everywhere else the overview keeps the pointer. */
	struct wlr_surface *surface = NULL;
	LayerSurface *l = NULL;
	Client *c = NULL;

	if (!overview_visible || overview_drag_client)
		return 0;
	/* xytonode's own pl output walks one node too far up and comes back
	 * empty, so resolve the toplevel from the surface instead */
	xytonode(cursor->x, cursor->y, &surface, NULL, NULL, NULL, NULL);
	return surface && toplevel_from_wlr_surface(surface, &c, &l) == LayerShell;
}

static int
overviewkeypassthrough(void)
{
	/* A layer surface holds the keyboard - the overview dock's search box
	 * asking for it - so the keys are its to consume, not the overview's. */
	struct wlr_surface *focus;
	LayerSurface *l = NULL;
	Client *c = NULL;

	if (!overview_visible)
		return 0;
	focus = seat->keyboard_state.focused_surface;
	return focus && toplevel_from_wlr_surface(focus, &c, &l) == LayerShell;
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
					&overview_drag_box, 0.96f, 0);
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
	saveoverviewstate(active);
	overview_swipe_active = overview_swipe_triggered = 0;
	overview_swipe_dx = overview_swipe_dy = 0.0;
	if (active) {
		overview_focus_client = NULL;
		if (!overview_visible) {
			overview_visible = 1;
			/* the previews clone the live buffers, so nothing may be
			 * caught halfway through an open animation */
			wl_list_for_each(m, &mons, link)
				animstop(m);
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
				m->overview[1] = m->m;
				m->overview[2] = target[2];
				if (SCROLLLT(m)) {
					m->overview[0].y -= m->m.height;
					m->overview[2].y += m->m.height;
				} else {
					m->overview[0].x -= m->m.width;
					m->overview[2].x += m->m.width;
				}
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
		/* the windows stay hidden until the closing transition ends, so
		 * put a deadline on it */
		if (overview_watchdog)
			wl_event_source_timer_update(overview_watchdog,
					MAX(500, animation_duration * 4));
		wl_list_for_each(m, &mons, link) {
			if (!overviewvalid(m))
				continue;
			for (i = 0; i < 3; i++)
				end[i] = m->overview[i];
			if (SCROLLLT(m)) {
				end[0].y -= m->m.height;
				end[2].y += m->m.height;
			} else {
				end[0].x -= m->m.width;
				end[2].x += m->m.width;
			}
			end[1] = m->w;
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
		if (SCROLLLT(m)) {
			distance = old[2].y - old[1].y;
			m->overview[2].y = old[2].y + distance;
		} else {
			distance = old[2].x - old[1].x;
			m->overview[2].x = old[2].x + distance;
		}
		m->overview_opacity[0] = oldopacity[1];
		m->overview_opacity[1] = oldopacity[2];
		m->overview_opacity[2] = 0.0f;
	} else {
		m->overview[2] = old[1];
		m->overview[1] = old[0];
		m->overview[0] = target[0];
		if (SCROLLLT(m)) {
			distance = old[1].y - old[0].y;
			m->overview[0].y = old[0].y - distance;
		} else {
			distance = old[1].x - old[0].x;
			m->overview[0].x = old[0].x - distance;
		}
		m->overview_opacity[2] = oldopacity[1];
		m->overview_opacity[1] = oldopacity[0];
		m->overview_opacity[0] = 0.0f;
	}
	overviewtransition(m, target, shown, 1.0f);
	overviewbuild();
}

void
overviewfocuscol(int dir)
{
	/* walk the focus along the strip inside the overview, like niri: the
	 * center panel pans to follow the newly focused column */
	static const float shown[] = {1.0f, 1.0f, 1.0f};
	struct wlr_box target[3];
	Monitor *m = selmon;
	Client *sel, *t = NULL, *cc;
	Column *cand = NULL, *it;
	struct wl_list *pos;

	if (!overview_active || !overviewvalid(m) || !SCROLLLT(m)
			|| m->overview_animating)
		return;
	if (!(sel = wsfocused(m, m->ws)))
		return;
	for (pos = dir > 0 ? sel->col->link.next : sel->col->link.prev;
			pos != &m->cols[m->ws];
			pos = dir > 0 ? pos->next : pos->prev) {
		it = wl_container_of(pos, it, link);
		if (scroll_coltiled(it)) {
			cand = it;
			break;
		}
	}
	if (!cand)
		return;
	wl_list_for_each(cc, &cand->clients, clink) {
		if (!cc->isfloating) {
			t = cc;
			break;
		}
	}
	if (!t)
		return;
	wl_list_remove(&t->flink);
	wl_list_insert(&fstack, &t->flink);
	overview_focus_client = t;
	arrange(m); /* pans the strip to the new column */
	overviewtargets(m, target);
	/* a (no-move) transition forces the clones to rebuild every frame, so
	 * the pan animation shows inside the panel */
	overviewtransition(m, target, shown, 1.0f);
	overviewbuild();
}

int
overviewkey(xkb_keysym_t sym)
{
	int scroll = SCROLLLT(selmon);

	sym = xkb_keysym_to_lower(sym);
	if (sym == XKB_KEY_Escape || sym == XKB_KEY_Return) {
		overviewset(0);
	} else if (sym == XKB_KEY_Left || sym == XKB_KEY_h) {
		if (scroll)
			overviewfocuscol(-1);
		else
			overviewnavigate((selmon->ws + NUMWS - 1) % NUMWS, -1);
	} else if (sym == XKB_KEY_Right || sym == XKB_KEY_l) {
		if (scroll)
			overviewfocuscol(1);
		else
			overviewnavigate((selmon->ws + 1) % NUMWS, 1);
	} else if (sym == XKB_KEY_Up || sym == XKB_KEY_k) {
		overviewnavigate((selmon->ws + NUMWS - 1) % NUMWS, -1);
	} else if (sym == XKB_KEY_Down || sym == XKB_KEY_j) {
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
	if (!locked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED && !super
			&& !overviewkeypassthrough()) {
		if (overview_visible)
			group->overview_keycode = event->keycode;
		for (i = 0; i < nsyms; i++)
			handled = (overview_visible ? (overview_active ? overviewkey(syms[i]) : 1)
					: keybinding(mods, syms[i])) || handled;
	}
	if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && !overviewkeypassthrough()
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

	/* The blur goes under the frame and the surface, so it paints what is
	 * behind the whole window. */
	if ((c->blur = wlr_scene_blur_create(c->scene, 0, 0))) {
		c->blur->node.data = c;
		wlr_scene_node_lower_to_bottom(&c->blur->node);
	}

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

	/* Pop the new window in */
	if (animations && animation_type_open != AnimNone && animation_duration_open > 0
			&& c->mon && VISIBLEON(c, c->mon)) {
		/* the drift camera owns the buffer scale of the windows it holds,
		 * so a window opening on that canvas only slides and fades */
		c->anim.zoom = animation_type_open == AnimZoom && !DRIFTLT(c->mon);
		c->anim.from.x = c->geom.x;
		c->anim.from.y = c->geom.y + (animation_type_open == AnimSlide
				? MAX(14, MIN(32, c->geom.height / 18)) : 0);
		c->anim.fadein = 1;
		c->anim.workspace = 0;
		c->anim.hide = 0;
		c->anim.active = 1;
		c->anim.t = 0;
		if (c->anim.zoom) {
			clientscale(c, zoom_initial_ratio);
			wlr_scene_node_set_position(&c->scene->node,
					c->geom.x + (int)roundf(c->geom.width
							* (1.0f - zoom_initial_ratio) / 2.0f),
					c->geom.y + (int)roundf(c->geom.height
							* (1.0f - zoom_initial_ratio) / 2.0f));
		} else {
			wlr_scene_node_set_position(&c->scene->node,
					c->anim.from.x, c->anim.from.y);
		}
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
				applyfakefull(w);
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

		if (active_constraint && cursor_mode != CurResize && cursor_mode != CurMove
				&& cursor_mode != CurDragTile && cursor_mode != CurResizeCol
				&& cursor_mode != CurBSPMove && cursor_mode != CurBSPResize
				&& cursor_mode != CurDriftMove && cursor_mode != CurDriftResize
				&& cursor_mode != CurDriftPan) {
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
	if (!overviewpassthrough() && overviewmotion())
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
	} else if (cursor_mode == CurDragTile) {
		if (grabc)
			scroll_dragto(grabc, cursor->x);
		return;
	} else if (cursor_mode == CurBSPMove) {
		if (grabc)
			bsp_dragto(grabc, cursor->x, cursor->y);
		return;
	} else if (cursor_mode == CurBSPResize) {
		if (grabc)
			bsp_resizeto(grabc, cursor->x, cursor->y);
		return;
	} else if (cursor_mode == CurDriftMove) {
		if (grabc)
			driftdragto(grabc, cursor->x, cursor->y);
		return;
	} else if (cursor_mode == CurDriftResize) {
		if (grabc)
			driftresizeto(grabc, cursor->x, cursor->y);
		return;
	} else if (cursor_mode == CurDriftPan) {
		driftpanto(cursor->x, cursor->y);
		return;
	} else if (cursor_mode == CurResizeCol) {
		if (grabc && grabc->col && grabc->mon && grabc->mon->w.width > 0) {
			float f = grab_wfrac + (float)((cursor->x - grab_startx)
					/ grabc->mon->w.width);
			f = f < 0.15f ? 0.15f : f > 1.0f ? 1.0f : f;
			if (fabsf(f - grabc->col->wfrac) >= 0.003f) {
				/* direct manipulation: retile without easing */
				int oldanim = animations;
				grabc->col->wfrac = f;
				animations = 0;
				arrange(grabc->mon);
				animations = oldanim;
			}
		}
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

	if (DRIFTLT(grabc->mon) && grabc->oncanvas && !grabc->isfloating
			&& !grabc->isfakefull) {
		/* driftwm-style direct manipulation: drag moves the window across
		 * the canvas, right-drag resizes it for real */
		grabcx = (int)round(cursor->x) - grabc->geom.x;
		grabcy = (int)round(cursor->y) - grabc->geom.y;
		drift_dragcluster = arg->ui == CurDriftMove;
		if (arg->ui == CurResize) {
			cursor_mode = CurDriftResize;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
		} else {
			cursor_mode = CurDriftMove;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "grabbing");
		}
		return;
	}

	if (SCROLLLT(grabc->mon) && grabc->col && !grabc->isfloating
			&& !grabc->isfakefull) {
		/* niri-style direct manipulation: drag reorders the strip and
		 * right-drag resizes the column — the window never floats */
		if (arg->ui == CurMove) {
			cursor_mode = CurDragTile;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "grabbing");
		} else {
			cursor_mode = CurResizeCol;
			grab_startx = cursor->x;
			grab_wfrac = grabc->col->wfrac;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "ew-resize");
		}
		return;
	}

	if (grabc->node && !grabc->isfloating && !grabc->isfakefull) {
		/* bsp direct manipulation: drag swaps the window with the tile
		 * under the cursor and right-drag moves the splits it sits
		 * between — neither makes the window float */
		if (arg->ui == CurResize) {
			/* the grab picks the edges the click is nearest, so
			 * dragging away from the window always grows it */
			bsp_grabh = bsp_findsplit(grabc->node, 1,
					cursor->x >= grabc->geom.x + grabc->geom.width / 2.0);
			bsp_grabv = bsp_findsplit(grabc->node, 0,
					cursor->y >= grabc->geom.y + grabc->geom.height / 2.0);
			if (!bsp_grabh && !bsp_grabv)
				return; /* the only window on the workspace */
			bsp_grabhr = bsp_grabh ? bsp_grabh->ratio : 0.5f;
			bsp_grabvr = bsp_grabv ? bsp_grabv->ratio : 0.5f;
			grab_startx = cursor->x;
			grab_starty = cursor->y;
			cursor_mode = CurBSPResize;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "se-resize");
		} else {
			cursor_mode = CurBSPMove;
			wlr_cursor_set_xcursor(cursor, cursor_mgr, "grabbing");
		}
		return;
	}

	/* Float the window and tell motionnotify to grab it */
	setfloating(grabc, 1);
	switch (cursor_mode = (arg->ui == CurDriftMove ? CurMove : arg->ui)) {
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

	/* Following the pointer only on a change of pointer focus leaves the
	 * keyboard stranded whenever focus was dropped without the cursor
	 * moving off the window (a workspace switch onto an empty workspace, a
	 * client that closed elsewhere, a launcher that took the keyboard and
	 * went away): the window under the cursor is already the pointer focus,
	 * so nothing hands it the keyboard back and typing goes nowhere.  When
	 * nothing holds the keyboard at all, moving the mouse gives it to the
	 * window under it.  Focus set from the keyboard is left alone. */
	if (sloppyfocus && time && c && !client_is_unmanaged(c)
			&& (surface != seat->pointer_state.focused_surface
				|| (!exclusive_focus && !seat->drag
					&& !seat->keyboard_state.focused_surface)))
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
	int animpending = 0, pointerrefresh = 0, skipframe = 0, relayout = 0;
	int blockwait = 0;

	/* Render if no XDG clients have an outstanding resize and are visible on
	 * this monitor.  A client that never acks must not hold the output
	 * hostage: past RESIZEWAIT it is drawn at whatever size it has, or the
	 * whole screen would stay frozen on the last frame — with an animation
	 * caught halfway, windows parked off screen and nothing focusable. */
	wl_list_for_each(c, &clients, link) {
		if (c->resize && !c->isfloating && client_is_rendered_on_mon(c, m)
				&& !client_is_stopped(c)) {
			uint32_t waited = now_ms() - c->resizeat;
			if (waited >= RESIZEWAIT)
				continue;
			skipframe = 1;
			blockwait = MAX(blockwait, (int)(RESIZEWAIT - waited));
		}
	}

	/* Animations advance only on frames that actually render, and only
	 * schedule further frames while one is active. */
	/* The overview clones the live buffers, so the camera zoom has to be on
	 * them before anything is built from them this frame. */
	driftapply(m);

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
						? MAX(60, animation_duration_open)
						: animation_duration;
				targetx = c->anim.workspace ? c->anim.to.x : c->geom.x;
				targety = c->anim.workspace ? c->anim.to.y : c->geom.y;
				c->anim.t += duration > 0 ? dt / (float)duration : 1.0f;
				if (c->anim.t >= 1.0f) {
					c->anim.active = 0;
					if (c->anim.zoom) {
						c->anim.zoom = 0;
						clientunscale(c);
					}
					wlr_scene_node_set_position(&c->scene->node,
							c->anim.hide ? c->geom.x : targetx,
							c->anim.hide ? c->geom.y : targety);
					if (c->anim.hide) {
						/* the workspace can have come back while this
						 * ran: hiding it then would leave a window of
						 * the current workspace invisible and unfocusable */
						if (VISIBLEON(c, m)) {
							relayout = 1;
						} else {
							wlr_scene_node_set_enabled(&c->scene->node, 0);
							pointerrefresh = 1;
						}
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
						? animease(animation_curve_open, c->anim.t)
						: 1.0f - powf(2.0f, -10.0f * c->anim.t);
				if (c->anim.zoom) {
					/* grow the whole frame out of its own centre */
					float z = zoom_initial_ratio
							+ (1.0f - zoom_initial_ratio) * e;
					clientscale(c, z);
					wlr_scene_node_set_position(&c->scene->node,
							targetx + (int)roundf(c->geom.width
									* (1.0f - z) / 2.0f),
							targety + (int)roundf(c->geom.height
									* (1.0f - z) / 2.0f));
				} else {
					wlr_scene_node_set_position(&c->scene->node,
							c->anim.from.x + (int)((float)(targetx - c->anim.from.x) * e),
							c->anim.from.y + (int)((float)(targety - c->anim.from.y) * e));
				}
				if (c->anim.fadein && c->surfbuf)
					wlr_scene_buffer_set_opacity(c->surfbuf,
							e * clientopacity(c));
			}
			if (!skipframe) {
				animpending |= layeranimadvance(m, dt);
				animpending |= closeanimadvance(m, dt);
			}
			/* a hide that finished on a window the current workspace owns
			 * again: let arrange() settle visibility from the real state */
			if (relayout)
				arrange(m);
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
	 * damages the scene and restarts the animation on its own.  If it never
	 * commits, the timer below brings the output back once the wait is up. */
	if (animpending && !skipframe)
		wlr_output_schedule_frame(m->wlr_output);
	else if (skipframe && m->unblock)
		wl_event_source_timer_update(m->unblock, MAX(1, blockwait));
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
	int drifted;
	uint32_t serial;

	if (!c->mon || !client_surface(c)->mapped)
		return;

	bbox = interact ? &sgeom : &c->mon->w;
	drifted = DRIFTLT(c->mon) && c->oncanvas && !c->isfloating && !interact
			&& !c->isfullscreen && !c->isfakefull;

	client_set_bounds(c, geo.width, geo.height);
	c->geom = geo;
	if ((SCROLLLT(c->mon) || drifted) && !c->isfloating && !interact) {
		/* scroll columns and canvas windows legitimately extend past the
		 * viewport, so only enforce the minimum size */
		c->geom.width = MAX(1 + 2 * (int)c->bw, c->geom.width);
		c->geom.height = MAX(1 + 2 * (int)c->bw, c->geom.height);
	} else {
		applybounds(c, bbox);
	}

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
	{
		/* the open animation redraws these at its own size every frame */
		int r = (c->isfullscreen || c->isfakefull) ? 0
				: MIN(MAX(0, corner_radius), MIN(c->geom.width, c->geom.height) / 2);
		clientborders(c, c->geom.width, c->geom.height, (int)c->bw, r);
	}

	/* a window that left the canvas keeps its scaled buffers until it next
	 * commits, which is long enough to click on the wrong place */
	if (!drifted && c->bufscale != 1.0 && !c->anim.active)
		clientunscale(c);

	/* this is a no-op if size hasn't changed */
	if (drifted) {
		/* the client is sized in canvas pixels; the camera zoom is applied
		 * to its buffers instead, so it never sees a resize */
		serial = client_set_size(c,
				MAX(1, c->canvas.width - 2 * (int)c->bw),
				MAX(1, c->canvas.height - 2 * (int)c->bw));
	} else {
		serial = client_set_size(c, c->geom.width - 2 * c->bw,
				c->geom.height - 2 * c->bw);
	}
	if (serial && serial != c->resize)
		c->resizeat = now_ms();
	c->resize = serial;
	client_get_clip(c, &clip);
	if (drifted) {
		clip.width = MAX(1, c->canvas.width - 2 * (int)c->bw);
		clip.height = MAX(1, c->canvas.height - 2 * (int)c->bw);
	}
	wlr_scene_subsurface_tree_set_clip(&c->scene_surface->node, &clip);
	if (drifted && (driftz(c->mon, c->ws) != 1.0 || c->mon->driftscaled))
		driftscaleclient(c, driftz(c->mon, c->ws));
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

	/* a config that was already broken at login is worth saying out loud */
	cfgerrorshow();

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
	if (DRIFTLT(c->mon) && c->oncanvas && !floating && c->geom.width > 0) {
		/* dropped back onto the canvas: keep it where the user left it */
		double z = driftz(c->mon, c->ws);
		int bw = 2 * (int)c->bw;
		c->canvas.width = MAX(1 + bw, (int)round((c->geom.width - bw) / z) + bw);
		c->canvas.height = MAX(1 + bw, (int)round((c->geom.height - bw) / z) + bw);
		c->canvas.x = (int)round((c->geom.x - c->mon->w.x) / z + c->mon->camx[c->ws]);
		c->canvas.y = (int)round((c->geom.y - c->mon->w.y) / z + c->mon->camy[c->ws]);
		c->canvassized = 1;
	}
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
		detachclient(c);
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
		attachclient(m, (ws >= 0 && ws < NUMWS) ? (unsigned int)ws : m->ws, c);
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
cfganimtype(const char *s)
{
	if (!strcmp(s, "zoom") || !strcmp(s, "pop"))
		return AnimZoom;
	if (!strcmp(s, "slide"))
		return AnimSlide;
	if (!strcmp(s, "fade"))
		return AnimFade;
	if (!strcmp(s, "none") || !strcmp(s, "false"))
		return AnimNone;
	cfgerror("unknown animation type '%s'", s);
	return AnimZoom;
}

static int
cfgcurve(const char *s, float out[4])
{
	/* x1,y1,x2,y2 — the x values are clamped, as CSS does, so the curve
	 * stays a function of time; the y values may overshoot on purpose */
	float c[4];

	if (sscanf(s, "%f , %f , %f , %f", &c[0], &c[1], &c[2], &c[3]) != 4) {
		cfgerror("bad curve '%s', want x1,y1,x2,y2", s);
		return 0;
	}
	out[0] = MIN(1.0f, MAX(0.0f, c[0]));
	out[1] = c[1];
	out[2] = MIN(1.0f, MAX(0.0f, c[2]));
	out[3] = c[3];
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
		{ "wm:toggle_layout",         togglelayout,         {0} },
		{ "wm:layout:bsp",            setlayoutarg,         {.ui = LtBSP} },
		{ "wm:layout:scroll",         setlayoutarg,         {.ui = LtScroll} },
		{ "wm:layout:drift",          setlayoutarg,         {.ui = LtDrift} },
		{ "wm:pan_left",              driftpankey,          {.i = DirLeft} },
		{ "wm:pan_right",             driftpankey,          {.i = DirRight} },
		{ "wm:pan_up",                driftpankey,          {.i = DirUp} },
		{ "wm:pan_down",              driftpankey,          {.i = DirDown} },
		{ "wm:zoom_in",               driftzoomkey,         {.f = 1.0f} },
		{ "wm:zoom_out",              driftzoomkey,         {.f = -1.0f} },
		{ "wm:zoom_reset",            driftzoomkey,         {.f = 0.0f} },
		{ "wm:zoom_fit",              driftfit,             {0} },
		{ "wm:consume",               consumewin,           {0} },
		{ "wm:expel",                 expelwin,             {0} },
		{ "wm:toggle_fullscreen",     togglefakefullscreen, {0} },
		{ "wm:toggle_real_fullscreen",togglefullscreen,     {0} },
		{ "wm:toggle_float",          togglefloating,       {0} },
		{ "wm:toggle_float_centered", togglefloating,       {0} },
		{ "wm:toggle_decorations",    toggledecor,          {0} },
		{ "wm:workspace_prev",        wsstep,               {.i = -1} },
		{ "wm:workspace_next",        wsstep,               {.i = +1} },
		{ "wm:move_to_workspace_prev", movewsstep,          {.i = -1} },
		{ "wm:move_to_workspace_next", movewsstep,          {.i = +1} },
		{ "wm:move_to_workspace_left", movewsdir,           {.i = DirLeft} },
		{ "wm:move_to_workspace_right", movewsdir,          {.i = DirRight} },
		{ "wm:move_to_workspace_up",  movewsdir,            {.i = DirUp} },
		{ "wm:move_to_workspace_down", movewsdir,           {.i = DirDown} },
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
cfgerror(const char *fmt, ...)
{
	/* the first complaint is the one worth showing: after a typo the rest
	 * is usually the same mistake repeated */
	va_list ap;
	char msg[160];
	size_t i;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	wlr_log(WLR_ERROR, "config:%d: %s", cfg_curline, msg);
	if (cfg_nerrors++)
		return;
	/* the message ends up inside a shell command below */
	for (i = 0; msg[i]; i++)
		if (msg[i] == '\'' || msg[i] == '\\' || msg[i] == '\n')
			msg[i] = ' ';
	cfg_errline = cfg_curline;
	snprintf(cfg_errmsg, sizeof cfg_errmsg, "%s", msg);
}

int
cfgerrorhide(void *data)
{
	if (cfg_errbar) {
		wlr_scene_node_destroy(&cfg_errbar->node);
		cfg_errbar = NULL;
	}
	return 0;
}

void
cfgerrorshow(void)
{
	/* There is no text renderer here, so the compositor cannot spell the
	 * problem out on screen itself: it raises a red strip, which needs
	 * nothing installed and is impossible to miss, and passes the message
	 * to notify-send when a notification daemon is running.  The log line
	 * above has the details either way. */
	const float red[] = {0.85f, 0.15f, 0.18f, 0.9f};
	Monitor *m = selmon;
	char cmd[512];
	const char *argv[] = {"/bin/sh", "-c", cmd, NULL};
	Arg a = {.v = argv};

	if (!cfg_nerrors)
		return;
	if (!m && !wl_list_empty(&mons))
		m = wl_container_of(mons.next, m, link);
	if (m && m->w.width > 0) {
		cfgerrorhide(NULL);
		if ((cfg_errbar = wlr_scene_rect_create(layers[LyrOverlay],
				m->w.width, MAX(3, m->w.height / 96), red))) {
			wlr_scene_node_set_position(&cfg_errbar->node, m->w.x, m->w.y);
			if (!cfg_errbar_timer)
				cfg_errbar_timer = wl_event_loop_add_timer(event_loop,
						cfgerrorhide, NULL);
			if (cfg_errbar_timer)
				wl_event_source_timer_update(cfg_errbar_timer, 5000);
		}
	}
	snprintf(cmd, sizeof cmd, "command -v notify-send >/dev/null 2>&1 && "
			"notify-send -u critical 'gluewc: %d config error%s' "
			"'line %d: %s'", cfg_nerrors, cfg_nerrors > 1 ? "s" : "",
			cfg_errline, cfg_errmsg);
	spawn(&a);
}

int
cfgwatchfire(void *data)
{
	reloadconfig(NULL);
	return 0;
}

int
cfgwatchevent(int fd, uint32_t mask, void *data)
{
	union {
		struct inotify_event ev;
		char buf[4096];
	} u;
	const struct inotify_event *ev;
	const char *name = strrchr(cfg_path, '/');
	ssize_t len;
	char *pos;
	int hit = 0, gone = 0;

	name = name ? name + 1 : cfg_path;
	while ((len = read(fd, u.buf, sizeof u.buf)) > 0) {
		for (pos = u.buf; pos < u.buf + len; pos += sizeof(*ev) + ev->len) {
			ev = (const struct inotify_event *)pos;
			if (ev->len && !strcmp(ev->name, name)) {
				hit = 1;
			} else if (!ev->len && (ev->mask & (IN_MOVE_SELF | IN_DELETE_SELF))) {
				/* the directory was replaced under us: follow it.
				 * Nameless events are otherwise the watch's own
				 * bookkeeping — IN_IGNORED after a rearm above all —
				 * and reloading on those never stops. */
				gone = 1;
			}
		}
	}
	if (gone) {
		cfg_wd = -1;
		cfgwatch();
	}
	if (!hit)
		return 0;
	/* editors touch the file several times in a row; one reload once the
	 * dust settles is enough, and it never runs from inside the read */
	if (!cfg_watch_delay)
		cfg_watch_delay = wl_event_loop_add_timer(event_loop, cfgwatchfire, NULL);
	if (cfg_watch_delay)
		wl_event_source_timer_update(cfg_watch_delay, 150);
	return 0;
}

void
cfgwatch(void)
{
	/* Watching the directory rather than the file is what makes this work
	 * with editors at all: most of them write a temporary file and rename
	 * it over the config, which drops any watch on the file itself. */
	static int fd = -1;
	static char watched[512];
	char dir[512], *slash;

	if (!event_loop || !*cfg_path)
		return;
	snprintf(dir, sizeof dir, "%s", cfg_path);
	if (!(slash = strrchr(dir, '/')))
		return;
	*slash = '\0';
	/* rearming an intact watch only produces another event to react to */
	if (cfg_wd >= 0 && !strcmp(watched, dir))
		return;
	if (fd < 0 && (fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) < 0) {
		wlr_log(WLR_ERROR, "config: inotify_init1:");
		return;
	}
	if (cfg_wd >= 0)
		inotify_rm_watch(fd, cfg_wd);
	if ((cfg_wd = inotify_add_watch(fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO
			| IN_CREATE | IN_MOVE_SELF | IN_DELETE_SELF)) < 0) {
		wlr_log(WLR_ERROR, "config: cannot watch %s", dir);
		return;
	}
	snprintf(watched, sizeof watched, "%s", dir);
	if (!cfg_watch_src)
		cfg_watch_src = wl_event_loop_add_fd(event_loop, fd,
				WL_EVENT_READABLE, cfgwatchevent, NULL);
}

static const char *
layoutname(unsigned int lt)
{
	return lt == LtScroll ? "scroll" : lt == LtDrift ? "drift" : "bsp";
}

int
layoutstatepath(char *buf, size_t size)
{
	/* the layout the session was left in, kept out of the config file */
	const char *env;

	if ((env = getenv("XDG_STATE_HOME")) && *env)
		snprintf(buf, size, "%s/gluewc", env);
	else if ((env = getenv("HOME")) && *env)
		snprintf(buf, size, "%s/.local/state/gluewc", env);
	else
		return 0;
	return 1;
}

void
loadlayoutstate(void)
{
	char path[512], name[32] = {0};
	FILE *f;

	if (!remember_layout || !layoutstatepath(path, sizeof path - 8))
		return;
	strncat(path, "/layout", sizeof path - strlen(path) - 1);
	if (!(f = fopen(path, "r")))
		return;
	if (fscanf(f, "%31s", name) == 1) {
		if (!strcmp(name, "scroll"))
			default_layout = LtScroll;
		else if (!strcmp(name, "drift"))
			default_layout = LtDrift;
		else if (!strcmp(name, "bsp"))
			default_layout = LtBSP;
	}
	fclose(f);
}

void
savelayoutstate(unsigned int lt)
{
	char path[600], dir[512];
	FILE *f;

	if (!remember_layout || !layoutstatepath(dir, sizeof dir))
		return;
	/* the parent of the state dir usually exists already; create both anyway */
	{
		char *slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			mkdir(dir, 0700);
			*slash = '/';
		}
	}
	mkdir(dir, 0700);
	snprintf(path, sizeof path, "%s/layout", dir);
	if (!(f = fopen(path, "w")))
		return;
	fprintf(f, "%s\n", layoutname(lt));
	fclose(f);
}

void
saveoverviewstate(int active)
{
	/* published so the shell can put a dock on screen while the overview is
	 * up; no other compositor writes this, so the dock stays gluewc-only */
	char path[600], dir[512];
	FILE *f;

	if (!layoutstatepath(dir, sizeof dir))
		return;
	{
		char *slash = strrchr(dir, '/');
		if (slash) {
			*slash = '\0';
			mkdir(dir, 0700);
			*slash = '/';
		}
	}
	mkdir(dir, 0700);
	snprintf(path, sizeof path, "%s/overview", dir);
	if (!(f = fopen(path, "w")))
		return;
	fprintf(f, "%d\n", active ? 1 : 0);
	fclose(f);
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

	cfg_nerrors = 0;
	cfg_curline = 0;
	cfg_errline = 0;
	cfg_errmsg[0] = '\0';

	if ((env = getenv("XDG_CONFIG_HOME")))
		snprintf(path, sizeof path, "%s/gluewc/config.conf", env);
	else if ((env = getenv("HOME")))
		snprintf(path, sizeof path, "%s/.config/gluewc/config.conf", env);
	else
		return;
	snprintf(cfg_path, sizeof cfg_path, "%s", path);
	/* watch even when there is no config yet: writing one applies it */
	cfgwatch();
	if (!(f = fopen(path, "r"))) {
		loadlayoutstate();
		return;
	}

	while (getline(&line, &lsz, f) != -1) {
		cfg_curline++;
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
			char combo[96];
			if (!(v2 = strchr(v, '=')))
				continue;
			*v2++ = '\0';
			v = cfgtrim(v);
			v2 = cfgtrim(v2);
			/* cfgbindcombo() tokenises in place, so the error below
			 * would otherwise report only up to the first plus */
			snprintf(combo, sizeof combo, "%s", v);
			if (!cfgbindcombo(v, &mods, &sym) || !cfgaction(v2, &func, &arg)) {
				cfgerror("bad bind '%s = %s'", combo, v2);
				continue;
			}
			if (!strcmp(k, "bind_insert"))
				cfgaddbind(&runkeys, &nrunkeys, mods, sym, func, arg);
			else
				cfgaddbind(&runnormalkeys, &nrunnormalkeys, mods, sym, func, arg);
		} else if (!strcmp(k, "remember_layout")) {
			remember_layout = !strcmp(v, "true") || !strcmp(v, "1");
		} else if (!strcmp(k, "layout")) {
			default_layout = !strcmp(v, "scroll") ? LtScroll
					: !strcmp(v, "drift") ? LtDrift : LtBSP;
		} else if (!strcmp(k, "drift_snap")) {
			drift_snap = MAX(0, atoi(v));
		} else if (!strcmp(k, "drift_nudge")) {
			drift_nudge = MAX(1, atoi(v));
		} else if (!strcmp(k, "drift_zoom_min")) {
			drift_zoom_min = MAX(0.05f, strtof(v, NULL));
		} else if (!strcmp(k, "drift_zoom_max")) {
			drift_zoom_max = MAX(1.0f, strtof(v, NULL));
		} else if (!strcmp(k, "drift_zoom_step")) {
			drift_zoom_step = MAX(1.01f, strtof(v, NULL));
		} else if (!strcmp(k, "drift_pan_speed")) {
			drift_pan_speed = MAX(0.1f, strtof(v, NULL));
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
		} else if (!strcmp(k, "animation_type_open")) {
			animation_type_open = cfganimtype(v);
		} else if (!strcmp(k, "animation_type_close")) {
			animation_type_close = cfganimtype(v);
		} else if (!strcmp(k, "animation_duration_open")) {
			animation_duration_open = atoi(v);
		} else if (!strcmp(k, "animation_duration_close")) {
			animation_duration_close = atoi(v);
		} else if (!strcmp(k, "animation_curve_open")) {
			cfgcurve(v, animation_curve_open);
		} else if (!strcmp(k, "animation_curve_close")) {
			cfgcurve(v, animation_curve_close);
		} else if (!strcmp(k, "zoom_initial_ratio")) {
			zoom_initial_ratio = MIN(1.0f, MAX(0.05f, strtof(v, NULL)));
		} else if (!strcmp(k, "zoom_end_ratio")) {
			zoom_end_ratio = MIN(1.0f, MAX(0.05f, strtof(v, NULL)));
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
			cfgerror("unknown key '%s'", k);
		}
	}
	free(line);
	fclose(f);
	/* the layout the last session ended in wins over the configured one */
	loadlayoutstate();
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
	/* the first readconfig() ran before there was an event loop to watch on */
	cfgwatch();
	overview_watchdog = wl_event_loop_add_timer(event_loop, overviewwatchdog, NULL);

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
	/* clear any state left behind by a session that died with the overview
	 * open, so the shell does not start up with its dock on screen */
	saveoverviewstate(0);
	overview_scene = wlr_scene_tree_create(layers[LyrOverview]);
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
	wl_signal_add(&cursor->events.pinch_begin, &cursor_pinch_begin);
	wl_signal_add(&cursor->events.pinch_update, &cursor_pinch_update);
	wl_signal_add(&cursor->events.pinch_end, &cursor_pinch_end);

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
	cfgerrorshow();
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
	if (!sel)
		return;
	if (sel->col) {
		/* resize the column, like niri's set-column-width +/- */
		sel->col->wfrac += arg->f;
		if (sel->col->wfrac < 0.15f)
			sel->col->wfrac = 0.15f;
		if (sel->col->wfrac > 1.0f)
			sel->col->wfrac = 1.0f;
		arrange(selmon);
		return;
	}
	if (!sel->node || !sel->node->par)
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
	if (!sel || sel->isfloating || sel->isfullscreen || sel->isfakefull)
		return;
	if (sel->oncanvas) {
		/* nothing to swap on a canvas: move the window instead */
		driftnudge(arg);
		return;
	}
	if (sel->col) {
		scroll_swap(sel, arg->i);
		return;
	}
	if (!sel->node)
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
	if (!sel || sel->isfullscreen || sel->isfakefull || !selmon)
		return;
	if (sel->col) {
		scroll_swap(sel, arg->i > 0 ? DirRight : DirLeft);
		return;
	}
	if (!sel->node)
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
		if (!c->isfullscreen && !c->isfakefull)
			c->bw = decorhidden ? 0 : borderpx;
		applyeffects(c);
		if (c->isfloating)
			resize(c, c->geom, 0);
	}
	wl_list_for_each(m, &mons, link)
		arrange(m);
}

void
applyfakefull(Client *c)
{
	/* scroll layout: fake fullscreen covers the whole output (above bars),
	 * so its scene node has to move between the tile and fullscreen layers */
	if (!c->mon || c->isfullscreen)
		return;
	c->bw = (c->isfakefull || decorhidden) ? 0 : borderpx;
	if (c->isfloating || !client_surface(c)->mapped)
		return;
	wlr_scene_node_reparent(&c->scene->node,
			layers[c->isfakefull && SCROLLLT(c->mon) ? LyrFS : LyrTile]);
}

void
togglefakefullscreen(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel)
		return;
	if (sel->isfullscreen)
		setfullscreen(sel, 0);
	if (SCROLLLT(selmon) && sel->col && !sel->isfloating
			&& !sel->isfakefull) {
		/* scroll layout: no fullscreen — just make the column as wide as
		 * the screen and back, like a right-drag resize to full width */
		scroll_maximize(sel);
		return;
	}
	sel->isfakefull ^= 1;
	applyeffects(sel);
	if (sel->isfakefull) {
		sel->prev = sel->geom;
		if (sel->isfloating)
			setfloating(sel, 0);
	} else if (sel->isfloating) {
		resize(sel, sel->prev, 0);
	}
	applyfakefull(sel);
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
	if (!sel)
		return;
	if (sel->col) {
		scroll_maximize(sel);
		return;
	}
	if (!sel->node || !sel->node->par)
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
	/* always settle whatever was still moving: an animation left running
	 * across a switch finishes against the new workspace and takes windows
	 * that belong to it off screen */
	animstop(selmon);
	arrange(selmon);
	if (animate) {
		wl_list_for_each(c, &clients, link) {
			if (c->mon != selmon || c->ws != oldws
					|| !c->scene->node.enabled || client_is_unmanaged(c))
				continue;
			c->anim.from.x = c->scene->node.x;
			c->anim.from.y = c->scene->node.y;
			/* workspaces slide horizontally in BSP, vertically (like
			 * niri, one after another) in the scroll layout */
			c->anim.to.x = c->geom.x
					- (SCROLLLT(selmon) ? 0 : dir * selmon->m.width);
			c->anim.to.y = c->geom.y
					- (SCROLLLT(selmon) ? dir * selmon->m.height : 0);
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
			c->anim.from.x = c->geom.x
					+ (SCROLLLT(selmon) ? 0 : dir * selmon->m.width);
			c->anim.from.y = c->geom.y
					+ (SCROLLLT(selmon) ? dir * selmon->m.height : 0);
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
	if (!sel || !selmon || sel->ws == arg->ui
			|| (!sel->node && !sel->col && !sel->oncanvas))
		return;
	detachclient(sel);
	attachclient(selmon, arg->ui, sel);
	focusclient(focustop(selmon), 1);
	arrange(selmon);
}

void
movetowsfollow(const Arg *arg)
{
	Client *sel = focustop(selmon);
	if (!sel || !selmon || sel->ws == arg->ui
			|| (!sel->node && !sel->col && !sel->oncanvas))
		return;
	detachclient(sel);
	attachclient(selmon, arg->ui, sel);
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
movewsdir(const Arg *arg)
{
	/* Take the window along to the workspace in that direction, on the
	 * same axis focusdir() steps along when there is nothing left to focus:
	 * the scroll layout stacks workspaces vertically like niri, the others
	 * lay them out left to right. A press across the axis does nothing. */
	Arg a;

	if (!selmon)
		return;
	if (SCROLLLT(selmon) ? (arg->i == DirLeft || arg->i == DirRight)
			: (arg->i == DirUp || arg->i == DirDown))
		return;
	a.i = (arg->i == DirLeft || arg->i == DirUp) ? -1 : +1;
	movewsstep(&a);
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
		bsp_grabh = bsp_grabv = NULL;
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
	c->blur = NULL;
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
	c->bufscale = 1.0;
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
				wlr_xcursor_image_get_buffer(xcursor->images[0]),
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
