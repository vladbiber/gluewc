/* Taken from https://github.com/djpohly/dwl/issues/466 */
#define COLOR(hex)    { ((hex >> 24) & 0xFF) / 255.0f, \
                        ((hex >> 16) & 0xFF) / 255.0f, \
                        ((hex >> 8) & 0xFF) / 255.0f, \
                        (hex & 0xFF) / 255.0f }
/* appearance — the non-const values can be overridden at runtime from
 * ~/.config/gluewc/config.conf */
static const int sloppyfocus               = 1;  /* focus follows mouse */
static int warpcursor                      = 1;  /* warp cursor to focused window on keyboard focus, like nvwm */
static const int bypass_surface_visibility = 0;  /* 1 means idle inhibitors will disable idle tracking even if it's surface isn't visible  */
static unsigned int borderpx               = 2;  /* border pixel of windows */
static int gappx                           = 8;  /* gap around tiled windows */
static float rootcolor[]                   = COLOR(0x000000ff);
static float bordercolor[]                 = COLOR(0x2f3549ff);
static float focuscolor[]                  = COLOR(0x7aa2f7ff);
static float normalmodecolor[]             = COLOR(0xe0af68ff); /* focused border while in normal mode */
static int unfocused_borders                = 0;  /* 1 keeps the dim border on unfocused windows */
static int corner_radius                   = 8;  /* rounded corner radius, 0 disables */
static int blurenabled                     = 0;  /* backdrop blur behind transparent windows */
static int blur_passes                     = 3;
static int blur_radius                     = 5;
static float win_opacity                   = 1.0f; /* window opacity when transparency is on */
static int opacityenabled                  = 1;  /* toggled at runtime with wm:toggle_opacity */
static int animations                      = 1;  /* position animations (open/retile/workspace) */
static int animation_duration              = 200; /* ms, retile and workspace moves */
/* how a window appears and disappears: AnimZoom pops it out of its own centre,
 * AnimSlide moves it in from below, AnimFade only fades, AnimNone is instant */
static int animation_type_open             = AnimZoom;
static int animation_type_close            = AnimZoom;
static int animation_duration_open         = 300; /* ms */
static int animation_duration_close        = 250; /* ms */
static float zoom_initial_ratio            = 0.72f; /* size an opening window starts at */
static float zoom_end_ratio                = 0.76f; /* size a closing window ends at */
/* cubic-bezier control points, as in CSS: x1, y1, x2, y2 */
static float animation_curve_open[4]       = {0.16f, 1.0f, 0.3f, 1.0f};  /* ease out, snappy */
static float animation_curve_close[4]      = {0.42f, 0.0f, 0.6f, 1.0f};  /* ease in out */
static float scroll_colfrac                = 0.5f; /* default column width in the niri-style scroll layout */
static int default_layout                  = LtBSP; /* layout new monitors start in: LtBSP, LtScroll or LtDrift */
static int remember_layout                 = 1;     /* reopen in the layout the last session ended in */
/* drift layout — driftwm-style infinite canvas */
static int drift_snap                      = 24;    /* edge snapping distance, in canvas pixels */
static int drift_nudge                     = 20;    /* pixels a window moves per keyboard nudge */
static float drift_zoom_min                = 0.2f;  /* how far the camera can zoom out */
static float drift_zoom_max                = 3.0f;  /* how far the camera can zoom in */
static float drift_zoom_step               = 1.12f; /* zoom factor per key press or wheel notch */
static float drift_pan_speed               = 1.0f;  /* multiplier for touchpad and scroll panning */
/* This conforms to the xdg-protocol. Set the alpha to zero to restore the old behavior */
static const float fullscreen_bg[]         = {0.0f, 0.0f, 0.0f, 1.0f}; /* You can also use glsl colors */

/* logging */
static int log_level = WLR_ERROR;

static const Rule rules[] = {
	/* app_id     title       ws (1-9, 0 = current)   isfloating   monitor */
	{ NULL,       NULL,       0,                       0,           -1 },
};

/* monitors */
/* (x=-1, y=-1) is reserved as an "autoconfigure" monitor position indicator
 * WARNING: negative values other than (-1, -1) cause problems with Xwayland clients
 * https://gitlab.freedesktop.org/xorg/xserver/-/issues/899 */
static const MonitorRule monrules[] = {
	/* name       scale rotate/reflect                x    y */
	/* example of a HiDPI laptop monitor:
	{ "eDP-1",    2,    WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 }, */
	/* defaults */
	{ NULL,       1,    WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
};

/* keyboard */
static struct xkb_rule_names xkb_rules = {
	/* can specify fields: rules, model, layout, variant, options */
	/* example:
	.options = "ctrl:nocaps",
	*/
	.options = NULL,
};

static int repeat_rate = 25;
static int repeat_delay = 600;

/* Trackpad */
static const int tap_to_click = 1;
static const int tap_and_drag = 1;
static const int drag_lock = 1;
static const int natural_scrolling = 0;
static const int disable_while_typing = 1;
static const int left_handed = 0;
static const int middle_button_emulation = 0;
/* You can choose between:
LIBINPUT_CONFIG_SCROLL_NO_SCROLL
LIBINPUT_CONFIG_SCROLL_2FG
LIBINPUT_CONFIG_SCROLL_EDGE
LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN
*/
static const enum libinput_config_scroll_method scroll_method = LIBINPUT_CONFIG_SCROLL_2FG;

/* You can choose between:
LIBINPUT_CONFIG_CLICK_METHOD_NONE
LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS
LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER
*/
static const enum libinput_config_click_method click_method = LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS;

/* You can choose between:
LIBINPUT_CONFIG_SEND_EVENTS_ENABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED
LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
*/
static const uint32_t send_events_mode = LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;

/* You can choose between:
LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT
LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE
*/
static const enum libinput_config_accel_profile accel_profile = LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;
static const double accel_speed = 0.0;

/* You can choose between:
LIBINPUT_CONFIG_TAP_MAP_LRM -- 1/2/3 finger tap maps to left/right/middle
LIBINPUT_CONFIG_TAP_MAP_LMR -- 1/2/3 finger tap maps to left/middle/right
*/
static const enum libinput_config_tap_button_map button_map = LIBINPUT_CONFIG_TAP_MAP_LRM;

/* mod = Super, like nvwm */
#define MODKEY WLR_MODIFIER_LOGO

#define WSKEYS(KEY,WS) \
	{ MODKEY,                    KEY, viewws,         {.ui = (WS)} }, \
	{ MODKEY|WLR_MODIFIER_CTRL,  KEY, movetowsfollow, {.ui = (WS)} }

/* helper for spawning shell commands in the pre dwm-5.0 fashion */
#define SHCMD(cmd) { .v = (const char*[]){ "/bin/sh", "-c", cmd, NULL } }

/* Volume and backlight are driven by whichever helper is installed: wpctl
 * comes with WirePlumber and is there on any PipeWire system, pactl with the
 * PulseAudio tools, and the backlight is either brightnessctl or light. */
#define VOLCMD(wp, pa) SHCMD("if command -v wpctl >/dev/null 2>&1; then " \
	"wpctl " wp "; else pactl " pa "; fi")
#define BRTCMD(bc, li) SHCMD("if command -v brightnessctl >/dev/null 2>&1; then " \
	"brightnessctl " bc "; else light " li "; fi")
#define VOLUP   VOLCMD("set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+", "set-sink-volume @DEFAULT_SINK@ +5%")
#define VOLDOWN VOLCMD("set-volume @DEFAULT_AUDIO_SINK@ 5%-", "set-sink-volume @DEFAULT_SINK@ -5%")
#define VOLMUTE VOLCMD("set-mute @DEFAULT_AUDIO_SINK@ toggle", "set-sink-mute @DEFAULT_SINK@ toggle")
#define MICMUTE VOLCMD("set-mute @DEFAULT_AUDIO_SOURCE@ toggle", "set-source-mute @DEFAULT_SOURCE@ toggle")
#define BRTUP   BRTCMD("set +10%", "-A 10")
#define BRTDOWN BRTCMD("set 10%-", "-U 10")

/* commands */
static const char *termcmd[] = { "alacritty", NULL };
static const char *menucmd[] = { "rofi", "-show", "drun", NULL };

static const Key keys[] = {
	/* modifier                  key                 function        argument */
	{ MODKEY,                    XKB_KEY_q,          spawn,          {.v = termcmd} },
	{ MODKEY,                    XKB_KEY_Return,     spawn,          {.v = termcmd} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Q,          quit,           {0} },
	{ MODKEY,                    XKB_KEY_space,      spawn,          {.v = menucmd} },
	{ 0,                         XKB_KEY_Print,      spawn,          SHCMD("grim") },
	{ MODKEY,                    XKB_KEY_c,          killclient,     {0} },
	{ MODKEY,                    XKB_KEY_Escape,     entermode,      {.i = ModeNormal} },
	{ MODKEY,                    XKB_KEY_Tab,        focusnext,      {0} },
	{ MODKEY,                    XKB_KEY_f,          togglefakefullscreen, {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_f,          togglefullscreen, {0} },
	{ MODKEY,                    XKB_KEY_v,          togglefloating, {0} },
	{ MODKEY,                    XKB_KEY_m,          quit,           {0} },
	{ MODKEY,                    XKB_KEY_n,          togglelayout,   {0} },
	{ MODKEY,                    XKB_KEY_comma,      consumewin,     {0} },
	{ MODKEY,                    XKB_KEY_period,     expelwin,       {0} },
	{ MODKEY,                    XKB_KEY_b,          toggledecor,    {0} },
	{ MODKEY,                    XKB_KEY_o,          toggleopacity,  {0} },
	{ MODKEY,                    XKB_KEY_Left,       focusdir,       {.i = DirLeft} },
	{ MODKEY,                    XKB_KEY_Right,      focusdir,       {.i = DirRight} },
	{ MODKEY,                    XKB_KEY_Up,         focusdir,       {.i = DirUp} },
	{ MODKEY,                    XKB_KEY_Down,       focusdir,       {.i = DirDown} },
	{ MODKEY,                    XKB_KEY_h,          focusdir,       {.i = DirLeft} },
	{ MODKEY,                    XKB_KEY_l,          focusdir,       {.i = DirRight} },
	{ MODKEY,                    XKB_KEY_k,          focusdir,       {.i = DirUp} },
	{ MODKEY,                    XKB_KEY_j,          focusdir,       {.i = DirDown} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Left,       swapdir,        {.i = DirLeft} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Right,      swapdir,        {.i = DirRight} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Up,         swapdir,        {.i = DirUp} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_Down,       swapdir,        {.i = DirDown} },
	/* pans the camera in the drift layout, swaps windows everywhere else */
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Left,       driftpankey,    {.i = DirLeft} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Right,      driftpankey,    {.i = DirRight} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Up,         driftpankey,    {.i = DirUp} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Down,       driftpankey,    {.i = DirDown} },
	{ MODKEY,                    XKB_KEY_w,          driftfit,       {0} },
	{ MODKEY,                    XKB_KEY_plus,       driftzoomkey,   {.f = +1.0f} },
	{ MODKEY,                    XKB_KEY_equal,      driftzoomkey,   {.f = +1.0f} },
	{ MODKEY,                    XKB_KEY_minus,      driftzoomkey,   {.f = -1.0f} },
	{ MODKEY,                    XKB_KEY_0,          driftzoomkey,   {.f = 0.0f} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_h,          swapdir,        {.i = DirLeft} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_l,          swapdir,        {.i = DirRight} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_k,          swapdir,        {.i = DirUp} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_j,          swapdir,        {.i = DirDown} },
	{ MODKEY,                    XKB_KEY_Page_Up,    wsstep,         {.i = -1} },
	{ MODKEY,                    XKB_KEY_Page_Down,  wsstep,         {.i = +1} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Page_Up,    movewsstep,     {.i = -1} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Page_Down,  movewsstep,     {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_r,          reloadconfig,   {0} },
	/* Media keys, and the same actions on the function row for keyboards
	 * that have no media keys or hide them behind Fn. */
	{ 0, XKB_KEY_XF86AudioRaiseVolume,  spawn, VOLUP },
	{ 0, XKB_KEY_XF86AudioLowerVolume,  spawn, VOLDOWN },
	{ 0, XKB_KEY_XF86AudioMute,         spawn, VOLMUTE },
	{ 0, XKB_KEY_XF86AudioMicMute,      spawn, MICMUTE },
	{ 0, XKB_KEY_XF86AudioPlay,         spawn, SHCMD("playerctl play-pause") },
	{ 0, XKB_KEY_XF86AudioPause,        spawn, SHCMD("playerctl play-pause") },
	{ 0, XKB_KEY_XF86AudioStop,         spawn, SHCMD("playerctl stop") },
	{ 0, XKB_KEY_XF86AudioNext,         spawn, SHCMD("playerctl next") },
	{ 0, XKB_KEY_XF86AudioPrev,         spawn, SHCMD("playerctl previous") },
	{ 0, XKB_KEY_XF86MonBrightnessUp,   spawn, BRTUP },
	{ 0, XKB_KEY_XF86MonBrightnessDown, spawn, BRTDOWN },
	{ MODKEY,                    XKB_KEY_F1, spawn, SHCMD("playerctl play-pause") },
	{ MODKEY,                    XKB_KEY_F2, spawn, SHCMD("playerctl previous") },
	{ MODKEY,                    XKB_KEY_F3, spawn, SHCMD("playerctl next") },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_F2, spawn, SHCMD("playerctl position 10-") },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_F3, spawn, SHCMD("playerctl position 10+") },
	{ MODKEY,                    XKB_KEY_F4, spawn, VOLMUTE },
	{ MODKEY,                    XKB_KEY_F5, spawn, VOLDOWN },
	{ MODKEY,                    XKB_KEY_F6, spawn, VOLUP },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_F4, spawn, MICMUTE },
	{ MODKEY,                    XKB_KEY_F7, spawn, BRTDOWN },
	{ MODKEY,                    XKB_KEY_F8, spawn, BRTUP },
	WSKEYS(XKB_KEY_1, 0),
	WSKEYS(XKB_KEY_2, 1),
	WSKEYS(XKB_KEY_3, 2),
	WSKEYS(XKB_KEY_4, 3),
	WSKEYS(XKB_KEY_5, 4),
	WSKEYS(XKB_KEY_6, 5),
	WSKEYS(XKB_KEY_7, 6),
	WSKEYS(XKB_KEY_8, 7),
	WSKEYS(XKB_KEY_9, 8),

	/* Ctrl-Alt-Backspace and Ctrl-Alt-Fx used to be handled by X server */
	{ WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_Terminate_Server, quit, {0} },
	/* Ctrl-Alt-Fx is used to switch to another VT, if you don't know what a VT is
	 * do not remove them.
	 */
#define CHVT(n) { WLR_MODIFIER_CTRL|WLR_MODIFIER_ALT,XKB_KEY_XF86Switch_VT_##n, chvt, {.ui = (n)} }
	CHVT(1), CHVT(2), CHVT(3), CHVT(4), CHVT(5), CHVT(6),
	CHVT(7), CHVT(8), CHVT(9), CHVT(10), CHVT(11), CHVT(12),
};

/* normal-mode keys, matched by keysym alone (like nvwm's grabbed keyboard) */
static const Key normalkeys[] = {
	{ 0, XKB_KEY_i,     entermode,   {.i = ModeInsert} },
	{ 0, XKB_KEY_Tab,   focusnext,   {0} },
	{ 0, XKB_KEY_Left,  focusdir,    {.i = DirLeft} },
	{ 0, XKB_KEY_Right, focusdir,    {.i = DirRight} },
	{ 0, XKB_KEY_Up,    focusdir,    {.i = DirUp} },
	{ 0, XKB_KEY_Down,  focusdir,    {.i = DirDown} },
	{ 0, XKB_KEY_k,     focusdir,    {.i = DirUp} },
	{ 0, XKB_KEY_j,     focusdir,    {.i = DirDown} },
	{ 0, XKB_KEY_h,     setratio,    {.f = -0.05f} },
	{ 0, XKB_KEY_l,     setratio,    {.f = +0.05f} },
	{ 0, XKB_KEY_H,     swapstack,   {.i = -1} },
	{ 0, XKB_KEY_L,     swapstack,   {.i = +1} },
	{ 0, XKB_KEY_s,     togglesplit, {0} },
	/* the function row without Mod, since normal mode already owns the
	 * keyboard; the same order as the Mod bindings in insert mode */
	{ 0, XKB_KEY_F1,    spawn, SHCMD("playerctl play-pause") },
	{ 0, XKB_KEY_F2,    spawn, SHCMD("playerctl previous") },
	{ 0, XKB_KEY_F3,    spawn, SHCMD("playerctl next") },
	{ 0, XKB_KEY_F4,    spawn, VOLMUTE },
	{ 0, XKB_KEY_F5,    spawn, VOLDOWN },
	{ 0, XKB_KEY_F6,    spawn, VOLUP },
	{ 0, XKB_KEY_F7,    spawn, BRTDOWN },
	{ 0, XKB_KEY_F8,    spawn, BRTUP },
};

static const Button buttons[] = {
	{ MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
	/* drag the canvas itself around in the drift layout; in the other
	 * layouts there is no camera, so the click goes to the application */
	{ MODKEY|WLR_MODIFIER_SHIFT, BTN_LEFT, driftpan, {0} },
	/* driftwm keeps window dragging on Alt; these only bind in the drift
	 * layout, so Alt+click keeps working inside applications elsewhere.
	 * Shift takes the whole snapped cluster along. */
	{ WLR_MODIFIER_ALT, BTN_LEFT,  moveresize, {.ui = CurMove} },
	{ WLR_MODIFIER_ALT, BTN_RIGHT, moveresize, {.ui = CurResize} },
	{ WLR_MODIFIER_ALT|WLR_MODIFIER_SHIFT, BTN_LEFT, moveresize, {.ui = CurDriftMove} },
};
