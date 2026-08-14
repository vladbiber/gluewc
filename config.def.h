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
static int animation_duration              = 200; /* ms */
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
	{ 0,                         XKB_KEY_F1,         spawn,          SHCMD("playerctl play-pause") },
	{ 0,                         XKB_KEY_F2,         spawn,          SHCMD("playerctl previous") },
	{ 0,                         XKB_KEY_F3,         spawn,          SHCMD("playerctl next") },
	{ MODKEY,                    XKB_KEY_c,          killclient,     {0} },
	{ MODKEY,                    XKB_KEY_Escape,     entermode,      {.i = ModeNormal} },
	{ MODKEY,                    XKB_KEY_Tab,        focusnext,      {0} },
	{ MODKEY,                    XKB_KEY_f,          togglefakefullscreen, {0} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_f,          togglefullscreen, {0} },
	{ MODKEY,                    XKB_KEY_v,          togglefloating, {0} },
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
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Left,       swapdir,        {.i = DirLeft} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Right,      swapdir,        {.i = DirRight} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Up,         swapdir,        {.i = DirUp} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Down,       swapdir,        {.i = DirDown} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_h,          swapdir,        {.i = DirLeft} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_l,          swapdir,        {.i = DirRight} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_k,          swapdir,        {.i = DirUp} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_j,          swapdir,        {.i = DirDown} },
	{ MODKEY,                    XKB_KEY_Page_Up,    wsstep,         {.i = -1} },
	{ MODKEY,                    XKB_KEY_Page_Down,  wsstep,         {.i = +1} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Page_Up,    movewsstep,     {.i = -1} },
	{ MODKEY|WLR_MODIFIER_CTRL,  XKB_KEY_Page_Down,  movewsstep,     {.i = +1} },
	{ MODKEY|WLR_MODIFIER_SHIFT, XKB_KEY_r,          reloadconfig,   {0} },
	{ 0, XKB_KEY_XF86AudioRaiseVolume,  spawn, SHCMD("pactl set-sink-volume @DEFAULT_SINK@ +5%") },
	{ 0, XKB_KEY_XF86AudioLowerVolume,  spawn, SHCMD("pactl set-sink-volume @DEFAULT_SINK@ -5%") },
	{ 0, XKB_KEY_XF86AudioMute,         spawn, SHCMD("pactl set-sink-mute @DEFAULT_SINK@ toggle") },
	{ 0, XKB_KEY_XF86AudioPlay,         spawn, SHCMD("playerctl play-pause") },
	{ 0, XKB_KEY_XF86AudioNext,         spawn, SHCMD("playerctl next") },
	{ 0, XKB_KEY_XF86AudioPrev,         spawn, SHCMD("playerctl previous") },
	{ 0, XKB_KEY_XF86MonBrightnessUp,   spawn, SHCMD("brightnessctl set +10%") },
	{ 0, XKB_KEY_XF86MonBrightnessDown, spawn, SHCMD("brightnessctl set 10%-") },
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
	{ 0, XKB_KEY_F1,    spawn, SHCMD("playerctl play-pause") },
	{ 0, XKB_KEY_F2,    spawn, SHCMD("playerctl previous") },
	{ 0, XKB_KEY_F3,    spawn, SHCMD("playerctl next") },
};

static const Button buttons[] = {
	{ MODKEY, BTN_LEFT,   moveresize,     {.ui = CurMove} },
	{ MODKEY, BTN_MIDDLE, togglefloating, {0} },
	{ MODKEY, BTN_RIGHT,  moveresize,     {.ui = CurResize} },
};
