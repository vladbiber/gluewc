/* Enough of Hyprland's IPC for a shell written against Hyprland (MacShell):
 * the request socket (hyprctl, Quickshell.Hyprland) answers the JSON
 * queries and the Lua/classic dispatchers the shell uses, the event socket
 * carries the window and workspace events. Workspaces are gluewc's own:
 * id = output index * NUMWS + space + 1; minimized and hidden windows sit on
 * the special workspaces "special:minimized" and "special:hidden". */

#include <ctype.h>
#include <fcntl.h>
#include <regex.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <wlr/interfaces/wlr_keyboard.h>

#define HYPR_MINIMIZED (-98)
#define HYPR_HIDDEN (-99)

typedef struct {
	int fd;
	struct wl_event_source *src;
	struct wl_list link;
} HyprConn;

typedef struct {
	uintptr_t addr;
	int ws, fs, min;
	char title[256], cls[128];
	int seen;
} HyprWin;

static int hypr_req_fd = -1, hypr_evt_fd = -1;
static struct wl_event_source *hypr_req_src, *hypr_evt_src;
static struct wl_list hypr_reqs, hypr_listeners;
static char hypr_dir[288], hypr_sig[64];
static HyprWin *hypr_prev;
static size_t hypr_nprev;
static int *hypr_prevws;
static size_t hypr_nprevws;
static char hypr_prevmons[16][64];
static int hypr_prevmonws[16], hypr_nprevmons = -1;
static struct wl_event_source *hypr_idle;

/* This file comes from gluemac. gluewc has no minimized windows, a fixed
 * number of workspaces and a tiling layout, so the gluemac calls below are
 * answered with what gluewc has. */
enum { MinNone, MinMinimized, MinHidden };
enum { TileLeft, TileRight, TileTop, TileBottom };

static int
hyprminimized(Client *c)
{
	return MinNone;
}

static void
macminimize(Client *c, int how)
{
}

static void
macunminimize(Client *c)
{
}

static int
macmanaged(Client *c)
{
	return c && c->mon && !client_is_unmanaged(c) && client_surface(c)->mapped;
}

static pid_t
macpid(Client *c)
{
	pid_t pid = 0;

#ifdef XWAYLAND
	if (client_is_x11(c))
		return c->surface.xwayland->pid;
#endif
	wl_client_get_credentials(c->surface.xdg->client->client, &pid, NULL, NULL);
	return pid;
}

static unsigned int
macspaces(Monitor *m)
{
	return NUMWS;
}

static int
macspaceinsert(Monitor *m, unsigned int at)
{
	return 0;
}

static void
macview(Monitor *m, unsigned int ws)
{
	Arg a = {.ui = ws};

	if (!m || m->ws == ws)
		return;
	selmon = m;
	viewws(&a);
}

static void
macmovespace(Client *c, unsigned int ws)
{
	Monitor *m = c->mon;

	if (!m || c->ws == ws)
		return;
	if (c->node || c->col || c->oncanvas) {
		detachclient(c);
		attachclient(m, ws, c);
	} else {
		c->ws = ws;
	}
	focusclient(focustop(selmon), 1);
	arrange(m);
}

static void
macactivate(Client *c, int allwindows)
{
	if (!macmanaged(c))
		return;
	macview(c->mon, c->ws);
	selmon = c->mon;
	focusclient(c, 1);
}

/* the layout's own swap and fake full screen act on the focused window */
static void
mactileclient(Client *c, int tile)
{
	Arg a = {.i = tile == TileLeft ? DirLeft : tile == TileRight ? DirRight
			: tile == TileTop ? DirUp : DirDown};

	macactivate(c, 0);
	swapdir(&a);
}

static void
maczoom(Client *c)
{
	macactivate(c, 0);
	togglefakefullscreen(NULL);
}

static int
statefile(char *path, size_t size, const char *name)
{
	char dir[256], *q;

	if (!layoutstatepath(dir, sizeof dir))
		return 0;
	for (q = dir + 1; *q; q++) {
		if (*q != '/')
			continue;
		*q = '\0';
		mkdir(dir, 0700);
		*q = '/';
	}
	mkdir(dir, 0700);
	snprintf(path, size, "%s/%s", dir, name);
	return 1;
}
static uintptr_t hypr_prevactive = 1;
static char hypr_prevfocmon[64];
static int hypr_prevfocws, hypr_prevfs = -1;

static int
hyprmonidx(Monitor *m)
{
	Monitor *o;
	int i = 0;

	wl_list_for_each(o, &mons, link) {
		if (o == m)
			return i;
		i++;
	}
	return 0;
}

static Monitor *
hyprmonat(int idx)
{
	Monitor *m;
	int i = 0;

	wl_list_for_each(m, &mons, link)
		if (i++ == idx)
			return m;
	return NULL;
}

static int
hyprlive(Monitor *m)
{
	return m && m->wlr_output->enabled && !m->mirroring;
}

static int
hyprwsof(Monitor *m, unsigned int ws)
{
	return hyprmonidx(m) * NUMWS + (int)ws + 1;
}

static int
hyprclientws(Client *c)
{
	if (hyprminimized(c) == MinMinimized)
		return HYPR_MINIMIZED;
	if (hyprminimized(c) == MinHidden)
		return HYPR_HIDDEN;
	return hyprwsof(c->mon, c->ws);
}

static void
hyprwsname(int id, char *buf, size_t n)
{
	if (id == HYPR_MINIMIZED)
		snprintf(buf, n, "special:minimized");
	else if (id == HYPR_HIDDEN)
		snprintf(buf, n, "special:hidden");
	else
		snprintf(buf, n, "%d", id);
}

/* "3", "special:hidden", "+1", "e-1", "name:3" -> id relative to m */
static int
hyprparsews(const char *s, Monitor *m, int *id)
{
	int cur = m ? hyprwsof(m, m->ws) : 1, base, n;

	if (!s || !*s)
		return 0;
	if (!strncmp(s, "name:", 5))
		s += 5;
	if (!strcmp(s, "special:hidden") || !strcmp(s, "special:hidden "))
		return *id = HYPR_HIDDEN, 1;
	if (!strncmp(s, "special:minimized", 17))
		return *id = HYPR_MINIMIZED, 1;
	if (!strncmp(s, "special", 7))
		return *id = HYPR_HIDDEN, 1;
	if (*s == 'e' || *s == 'r' || *s == 'm')
		s++;
	if (*s == '+' || *s == '-') {
		n = atoi(s);
		base = m ? hyprmonidx(m) * NUMWS : 0;
		cur += n;
		if (m)
			cur = MAX(base + 1, MIN(cur, base + (int)macspaces(m)));
		return *id = cur, 1;
	}
	if ((n = atoi(s)) > 0)
		return *id = n, 1;
	return 0;
}

static Client *
hyprfocused(void)
{
	Client *c = focustop(selmon);
	return c && macmanaged(c) ? c : NULL;
}

static int
hyprlisted(Client *c)
{
	return macmanaged(c);
}

static void
jstr(FILE *f, const char *s)
{
	fputc('"', f);
	for (; s && *s; s++) {
		unsigned char ch = (unsigned char)*s;
		if (ch == '"' || ch == '\\')
			fprintf(f, "\\%c", ch);
		else if (ch == '\n')
			fputs("\\n", f);
		else if (ch == '\t')
			fputs("\\t", f);
		else if (ch < 0x20)
			fprintf(f, "\\u%04x", ch);
		else
			fputc(ch, f);
	}
	fputc('"', f);
}

static void
hyprboxof(Client *c, struct wlr_box *b)
{
	b->x = c->geom.x + (int)c->bw;
	b->y = c->geom.y + (int)c->bw;
	b->width = MAX(0, c->geom.width - 2 * (int)c->bw);
	b->height = MAX(0, c->geom.height - 2 * (int)c->bw);
}

static int
hyprhistory(Client *c)
{
	Client *o;
	int i = 0;

	wl_list_for_each(o, &fstack, flink) {
		if (o == c)
			return i;
		i++;
	}
	return -1;
}

static void
hyprjclient(FILE *f, Client *c)
{
	struct wlr_box b;
	char ws[32];
	int id = hyprclientws(c);
	int fs = c->isfullscreen ? 2 : c->isfakefull ? 1 : 0;

	hyprboxof(c, &b);
	hyprwsname(id, ws, sizeof ws);
	fprintf(f, "{\"address\":\"0x%lx\",\"mapped\":true,\"hidden\":%s,"
			"\"at\":[%d,%d],\"size\":[%d,%d],\"workspace\":{\"id\":%d,\"name\":",
			(unsigned long)(uintptr_t)c, hyprminimized(c) ? "true" : "false",
			b.x, b.y, b.width, b.height, id);
	jstr(f, ws);
	fprintf(f, "},\"floating\":%s,\"pseudo\":false,\"monitor\":%d,\"class\":",
			c->isfloating ? "true" : "false", hyprmonidx(c->mon));
	jstr(f, client_get_appid(c));
	fputs(",\"title\":", f);
	jstr(f, client_get_title(c));
	fputs(",\"initialClass\":", f);
	jstr(f, client_get_appid(c));
	fputs(",\"initialTitle\":", f);
	jstr(f, client_get_title(c));
	fprintf(f, ",\"pid\":%d,\"xwayland\":%s,\"pinned\":false,\"fullscreen\":%d,"
			"\"fullscreenClient\":%d,\"overFullscreen\":false,\"grouped\":[],\"tags\":[],"
			"\"swallowing\":\"0x0\",\"focusHistoryID\":%d,\"inhibitingIdle\":false,"
			"\"xdgTag\":\"\",\"xdgDescription\":\"\",\"contentType\":\"none\","
			"\"stableId\":\"%lx\"}",
			(int)macpid(c), client_is_x11(c) ? "true" : "false", fs, fs,
			hyprhistory(c), (unsigned long)(uintptr_t)c);
}

static void
hyprjclients(FILE *f)
{
	Client *c;
	int first = 1;

	fputc('[', f);
	wl_list_for_each_reverse(c, &clients, link) {
		if (!hyprlisted(c))
			continue;
		if (!first)
			fputc(',', f);
		first = 0;
		hyprjclient(f, c);
	}
	fputc(']', f);
}

static int
hyprwswindows(int id, Client **last)
{
	Client *c;
	int n = 0;

	if (last)
		*last = NULL;
	wl_list_for_each(c, &fstack, flink) {
		if (!hyprlisted(c) || hyprclientws(c) != id)
			continue;
		if (last && !*last)
			*last = c;
		n++;
	}
	return n;
}

static int
hyprwsfull(int id)
{
	Client *c;

	wl_list_for_each(c, &clients, link)
		if (hyprlisted(c) && hyprclientws(c) == id && c->isfullscreen)
			return 1;
	return 0;
}

static void
hyprjws(FILE *f, int id, Monitor *m)
{
	Client *last;
	char ws[32];
	int n = hyprwswindows(id, &last);

	hyprwsname(id, ws, sizeof ws);
	fprintf(f, "{\"id\":%d,\"name\":", id);
	jstr(f, ws);
	fputs(",\"monitor\":", f);
	jstr(f, m ? m->wlr_output->name : "");
	fprintf(f, ",\"monitorID\":%d,\"windows\":%d,\"hasfullscreen\":%s,"
			"\"lastwindow\":\"0x%lx\",\"lastwindowtitle\":",
			m ? hyprmonidx(m) : 0, n, hyprwsfull(id) ? "true" : "false",
			(unsigned long)(uintptr_t)last);
	jstr(f, last ? client_get_title(last) : "");
	fputs(",\"ispersistent\":false}", f);
}

/* every workspace that exists right now, in id order */
static size_t
hyprwslist(int **out)
{
	Monitor *m;
	unsigned int i, n;
	size_t k = 0;
	int *ids = NULL;

	wl_list_for_each(m, &mons, link) {
		if (!hyprlive(m))
			continue;
		n = macspaces(m);
		ids = realloc(ids, (k + n + 2) * sizeof(*ids));
		for (i = 0; i < n; i++)
			ids[k++] = hyprwsof(m, i);
	}
	ids = realloc(ids, (k + 2) * sizeof(*ids));
	if (hyprwswindows(HYPR_HIDDEN, NULL))
		ids[k++] = HYPR_HIDDEN;
	if (hyprwswindows(HYPR_MINIMIZED, NULL))
		ids[k++] = HYPR_MINIMIZED;
	*out = ids;
	return k;
}

static Monitor *
hyprwsmon(int id)
{
	Monitor *m;

	if (id < 0)
		return selmon;
	m = hyprmonat((id - 1) / NUMWS);
	return m ? m : selmon;
}

static void
hyprjworkspaces(FILE *f)
{
	int *ids;
	size_t n = hyprwslist(&ids), i;

	fputc('[', f);
	for (i = 0; i < n; i++) {
		if (i)
			fputc(',', f);
		hyprjws(f, ids[i], hyprwsmon(ids[i]));
	}
	fputc(']', f);
	free(ids);
}

static void
hyprjmonitor(FILE *f, Monitor *m)
{
	struct wlr_output *o = m->wlr_output;
	char ws[32];
	int id = hyprwsof(m, m->ws);
	float hz = o->current_mode ? o->current_mode->refresh / 1000.0f
			: o->refresh / 1000.0f;

	hyprwsname(id, ws, sizeof ws);
	fprintf(f, "{\"id\":%d,\"name\":", hyprmonidx(m));
	jstr(f, o->name);
	fputs(",\"description\":", f);
	jstr(f, o->description ? o->description : o->name);
	fputs(",\"make\":", f);
	jstr(f, o->make ? o->make : "");
	fputs(",\"model\":", f);
	jstr(f, o->model ? o->model : "");
	fputs(",\"serial\":", f);
	jstr(f, o->serial ? o->serial : "");
	fprintf(f, ",\"width\":%d,\"height\":%d,\"physicalWidth\":%d,"
			"\"physicalHeight\":%d,\"refreshRate\":%.5f,\"x\":%d,\"y\":%d,"
			"\"activeWorkspace\":{\"id\":%d,\"name\":",
			o->width, o->height, o->phys_width, o->phys_height, hz,
			m->m.x, m->m.y, id);
	jstr(f, ws);
	fprintf(f, "},\"specialWorkspace\":{\"id\":0,\"name\":\"\"},"
			"\"reserved\":[%d,%d,%d,%d],\"scale\":%.2f,\"transform\":%d,"
			"\"focused\":%s,\"dpmsStatus\":%s,\"vrr\":%s,\"solitary\":\"0x0\","
			"\"activelyTearing\":false,\"directScanoutTo\":\"0x0\",\"disabled\":false,"
			"\"currentFormat\":\"XRGB8888\",\"mirrorOf\":\"none\",\"availableModes\":[",
			m->w.x - m->m.x, m->w.y - m->m.y,
			(m->m.x + m->m.width) - (m->w.x + m->w.width),
			(m->m.y + m->m.height) - (m->w.y + m->w.height),
			o->scale, (int)o->transform, m == selmon ? "true" : "false",
			m->asleep ? "false" : "true", o->adaptive_sync_status ? "true" : "false");
	{
		struct wlr_output_mode *mode;
		int first = 1;
		wl_list_for_each(mode, &o->modes, link) {
			fprintf(f, "%s\"%dx%d@%.2fHz\"", first ? "" : ",",
					mode->width, mode->height, mode->refresh / 1000.0f);
			first = 0;
		}
	}
	fputs("]}", f);
}

static void
hyprjmonitors(FILE *f)
{
	Monitor *m;
	int first = 1;

	fputc('[', f);
	wl_list_for_each(m, &mons, link) {
		if (!hyprlive(m))
			continue;
		if (!first)
			fputc(',', f);
		first = 0;
		hyprjmonitor(f, m);
	}
	fputc(']', f);
}

static void
hyprjdevices(FILE *f)
{
	struct wlr_keyboard *kb = &kb_group->wlr_group->keyboard;
	xkb_layout_index_t cur = kb->keymap ? xkb_state_serialize_layout(kb->xkb_state,
			XKB_STATE_LAYOUT_EFFECTIVE) : 0;
	const char *name = kb->keymap ? xkb_keymap_layout_get_name(kb->keymap, cur) : NULL;

	fputs("{\"mice\":[],\"keyboards\":[{\"address\":\"0x1\",\"name\":\"gluewc-keyboard\","
			"\"rules\":\"\",\"model\":\"\",\"layout\":", f);
	jstr(f, xkb_rules.layout ? xkb_rules.layout : "us");
	fputs(",\"variant\":", f);
	jstr(f, xkb_rules.variant ? xkb_rules.variant : "");
	fputs(",\"options\":", f);
	jstr(f, xkb_rules.options ? xkb_rules.options : "");
	fputs(",\"active_keymap\":", f);
	jstr(f, name ? name : "English (US)");
	fprintf(f, ",\"capsLock\":%s,\"numLock\":%s,\"main\":true}],"
			"\"tablets\":[],\"touch\":[],\"switches\":[]}",
			kb->keymap && xkb_state_led_name_is_active(kb->xkb_state, XKB_LED_NAME_CAPS) > 0
				? "true" : "false",
			kb->keymap && xkb_state_led_name_is_active(kb->xkb_state, XKB_LED_NAME_NUM) > 0
				? "true" : "false");
}

/* ---- dispatchers ---- */

/* the value of KEY in a Lua table literal: "x", 12, true, or a bare word */
static int
hyprarg(const char *s, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	const char *p = s, *q;

	for (; (p = strstr(p, key)); p += kl) {
		if (p > s && (isalnum((unsigned char)p[-1]) || p[-1] == '_'))
			continue;
		q = p + kl;
		while (*q == ' ')
			q++;
		if (*q != '=' || q[1] == '=')
			continue;
		q++;
		while (*q == ' ')
			q++;
		if (*q == '"' || *q == '\'') {
			char quote = *q++;
			size_t i = 0;
			while (*q && *q != quote && i + 1 < n) {
				if (*q == '\\' && q[1])
					q++;
				out[i++] = *q++;
			}
			out[i] = '\0';
			return 1;
		}
		{
			size_t i = 0;
			while (*q && *q != ',' && *q != '}' && *q != ')' && *q != ' ' && i + 1 < n)
				out[i++] = *q++;
			out[i] = '\0';
		}
		return 1;
	}
	return 0;
}

static int
hyprhasword(const char *s, const char *w)
{
	char pat[64];

	snprintf(pat, sizeof pat, "\"%s\"", w);
	return strstr(s, pat) != NULL;
}

static int
hyprmatch(const char *pattern, const char *text)
{
	regex_t re;
	int ok;

	if (regcomp(&re, pattern, REG_EXTENDED | REG_NOSUB))
		return !strcmp(pattern, text ? text : "");
	ok = !regexec(&re, text ? text : "", 0, NULL, 0);
	regfree(&re);
	return ok;
}

/* "address:0x..", "title:^(..)$", "class:..", "pid:..", "activewindow" */
static Client *
hyprselect(const char *sel)
{
	Client *c;
	uintptr_t a;

	if (!sel || !*sel || !strcmp(sel, "activewindow"))
		return hyprfocused();
	if (!strncmp(sel, "address:", 8)) {
		a = (uintptr_t)strtoull(sel + 8, NULL, 16);
		wl_list_for_each(c, &clients, link)
			if ((uintptr_t)c == a && hyprlisted(c))
				return c;
		return NULL;
	}
	wl_list_for_each(c, &fstack, flink) {
		if (!hyprlisted(c))
			continue;
		if (!strncmp(sel, "title:", 6) && hyprmatch(sel + 6, client_get_title(c)))
			return c;
		if (!strncmp(sel, "class:", 6) && hyprmatch(sel + 6, client_get_appid(c)))
			return c;
		if (!strncmp(sel, "initialclass:", 13) && hyprmatch(sel + 13, client_get_appid(c)))
			return c;
		if (!strncmp(sel, "pid:", 4) && macpid(c) == atoi(sel + 4))
			return c;
		if (!strchr(sel, ':') && hyprmatch(sel, client_get_appid(c)))
			return c;
	}
	return NULL;
}

static void
hyprfocusclient(Client *c)
{
	if (!c)
		return;
	selmon = c->mon;
	macactivate(c, 0);
}

static void
hyprviewws(int id)
{
	Monitor *m;
	Client *c;

	if (id == HYPR_HIDDEN || id == HYPR_MINIMIZED) {
		wl_list_for_each(c, &clients, link)
			if (hyprlisted(c) && hyprclientws(c) == id)
				macunminimize(c);
		if (selmon)
			focusclient(focustop(selmon), 1);
		return;
	}
	if (!(m = hyprmonat((id - 1) / NUMWS)))
		return;
	overviewset(0);
	macview(m, (unsigned int)((id - 1) % NUMWS));
}

static void
hyprmovews(Client *c, int id)
{
	Monitor *m;
	unsigned int ws;

	if (id == HYPR_HIDDEN) {
		macminimize(c, MinHidden);
		return;
	}
	if (id == HYPR_MINIMIZED) {
		macminimize(c, MinMinimized);
		return;
	}
	if (!(m = hyprmonat((id - 1) / NUMWS)))
		return;
	ws = (unsigned int)((id - 1) % NUMWS);
	if (ws >= macspaces(m) && !macspaceinsert(m, macspaces(m)))
		return;
	if (hyprminimized(c))
		macunminimize(c);
	if (c->mon != m)
		setmon(c, m, (int)ws);
	else
		macmovespace(c, ws);
	arrange(m);
	if (c->mon == selmon)
		focusclient(focustop(selmon), 1);
	ipcnotifyall();
}

static void
hyprplace(Client *c, int x, int y, int w, int h)
{
	struct wlr_box g = c->geom;

	if (x != INT_MIN)
		g.x = x - (int)c->bw;
	if (y != INT_MIN)
		g.y = y - (int)c->bw;
	if (w > 0)
		g.width = w + 2 * (int)c->bw;
	if (h > 0)
		g.height = h + 2 * (int)c->bw;
	/* a tiled window's place belongs to the layout */
	if (!c->isfloating || c->isfullscreen)
		return;
	resize(c, g, 0);
}

static void
hyprexec(const char *cmd)
{
	char *argv[] = {"/bin/sh", "-c", NULL, NULL};
	Arg a;

	if (!cmd || !*cmd)
		return;
	argv[2] = (char *)cmd;
	a.v = argv;
	spawn(&a);
}

static int
hyprmodsparse(const char *mods, char *out, size_t n)
{
	char buf[64], *tok, *save = NULL;
	size_t len = 0;

	out[0] = '\0';
	snprintf(buf, sizeof buf, "%s", mods);
	for (tok = strtok_r(buf, " +_", &save); tok; tok = strtok_r(NULL, " +_", &save)) {
		const char *name = NULL;
		if (!strcasecmp(tok, "CTRL") || !strcasecmp(tok, "CONTROL"))
			name = "ctrl";
		else if (!strcasecmp(tok, "SHIFT"))
			name = "shift";
		else if (!strcasecmp(tok, "ALT"))
			name = "alt";
		else if (!strcasecmp(tok, "SUPER") || !strcasecmp(tok, "WIN") || !strcasecmp(tok, "LOGO"))
			name = "super";
		if (name)
			len += (size_t)snprintf(out + len, n - MIN(len, n), "%s+", name);
	}
	return 1;
}

static void
hyprshortcut(const char *mods, const char *key)
{
	char combo[160], m[64];
	struct wlr_keyboard *kb = &kb_group->wlr_group->keyboard;
	struct wlr_keyboard_modifiers saved = kb->modifiers;
	uint32_t want;
	xkb_keysym_t sym;
	xkb_keycode_t k, code = 0;
	const xkb_keysym_t *syms;
	Client *c = hyprfocused();
	uint32_t t = now_ms();
	struct wlr_keyboard_modifiers mm;

	hyprmodsparse(mods ? mods : "", m, sizeof m);
	snprintf(combo, sizeof combo, "%s%s", m, key);
	if (!c || !kb->keymap || !cfgbindcombo(combo, &want, &sym))
		return;
	for (k = xkb_keymap_min_keycode(kb->keymap); !code && k <= xkb_keymap_max_keycode(kb->keymap); k++)
		if (xkb_keymap_key_get_syms_by_level(kb->keymap, k, 0, 0, &syms) > 0
				&& xkb_keysym_to_lower(syms[0]) == xkb_keysym_to_lower(sym))
			code = k;
	if (!code)
		return;
	if (seat->keyboard_state.focused_surface != client_surface(c))
		focusclient(c, 1);
	wlr_seat_set_keyboard(seat, kb);
	mm = saved;
	mm.depressed = want;
	wlr_seat_keyboard_notify_modifiers(seat, &mm);
	wlr_seat_keyboard_notify_key(seat, t, code - 8, WL_KEYBOARD_KEY_STATE_PRESSED);
	wlr_seat_keyboard_notify_key(seat, t + 1, code - 8, WL_KEYBOARD_KEY_STATE_RELEASED);
	wlr_seat_keyboard_notify_modifiers(seat, &saved);
}

static void
hyprsetlayout(const char *which)
{
	struct wlr_keyboard *kb = &kb_group->wlr_group->keyboard;
	xkb_layout_index_t n, cur;
	struct wlr_keyboard_modifiers m;

	if (!kb->keymap || !(n = xkb_keymap_num_layouts(kb->keymap)))
		return;
	cur = xkb_state_serialize_layout(kb->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE);
	if (!strcmp(which, "next"))
		cur = (cur + 1) % n;
	else if (!strcmp(which, "prev"))
		cur = (cur + n - 1) % n;
	else
		cur = (xkb_layout_index_t)MIN((unsigned int)atoi(which), n - 1);
	m = kb->modifiers;
	wlr_keyboard_notify_modifiers(kb, m.depressed, m.latched, m.locked, cur);
}

static void
hyprmonitorcfg(const char *expr)
{
	char out[64] = "", mode[64] = "", pos[64] = "", scale[32] = "", line[256];
	int x, y;
	size_t len;

	if (!hyprarg(expr, "output", out, sizeof out) || !*out)
		return;
	hyprarg(expr, "mode", mode, sizeof mode);
	hyprarg(expr, "position", pos, sizeof pos);
	hyprarg(expr, "scale", scale, sizeof scale);
	len = (size_t)snprintf(line, sizeof line, "%s", out);
	if (*mode && strcmp(mode, "preferred") && strcmp(mode, "highres")
			&& strcmp(mode, "highrr"))
		len += (size_t)snprintf(line + len, sizeof line - len, " mode=%s", mode);
	else if (*mode)
		len += (size_t)snprintf(line + len, sizeof line - len, " mode=preferred");
	if (*pos && sscanf(pos, "%dx%d", &x, &y) == 2)
		len += (size_t)snprintf(line + len, sizeof line - len, " pos=%d,%d", x, y);
	else if (*pos)
		len += (size_t)snprintf(line + len, sizeof line - len, " pos=auto");
	if (*scale && strcmp(scale, "auto"))
		snprintf(line + len, sizeof line - len, " scale=%s", scale);
	cfgoutput(line);
	applyoutputcfgs();
}

static void
hyprinputcfg(const char *expr)
{
	char v[32];
	struct wlr_keyboard *kb = &kb_group->wlr_group->keyboard;

	if (hyprarg(expr, "repeat_rate", v, sizeof v))
		repeat_rate = atoi(v);
	if (hyprarg(expr, "repeat_delay", v, sizeof v))
		repeat_delay = atoi(v);
	wlr_keyboard_set_repeat_info(kb, repeat_rate, repeat_delay);
}

/* one hl.dsp.X({...}) expression, or a classic "dispatcher args" line */
static const char *
hyprdispatchone(const char *e)
{
	char sel[256] = "", v[256] = "", w[64] = "";
	const char *name, *args;
	Client *c;
	int id;

	while (*e == ' ' || *e == '\t')
		e++;
	if (!strncmp(e, "hl.dispatch(", 12))
		e += 12;
	if (strncmp(e, "hl.dsp.", 7))
		goto classic;
	name = e + 7;
	args = strchr(name, '(');
	if (!args)
		return "invalid dispatcher";
	hyprarg(args, "window", sel, sizeof sel);

#define IS(n) (!strncmp(name, n "(", sizeof(n)))
	if (IS("no_op"))
		return "ok";
	if (IS("exec_cmd")) {
		const char *q = strchr(args, '"');
		char cmd[1024];
		size_t i = 0;
		if (!q)
			return "invalid command";
		for (q++; *q && *q != '"' && i + 1 < sizeof cmd; q++) {
			if (*q == '\\' && q[1])
				q++;
			cmd[i++] = *q;
		}
		cmd[i] = '\0';
		hyprexec(cmd);
		return "ok";
	}
	if (IS("exit")) {
		quit(NULL);
		return "ok";
	}
	if (IS("layout"))
		return "ok";
	if (IS("send_shortcut")) {
		char mods[64] = "", key[64] = "";
		hyprarg(args, "mods", mods, sizeof mods);
		hyprarg(args, "key", key, sizeof key);
		hyprshortcut(mods, key);
		return "ok";
	}
	if (IS("workspace.toggle_special")) {
		const char *q = strchr(args, '"');
		char ws[64] = "special:";
		if (q)
			sscanf(q + 1, "%55[^\"]", ws + 8);
		if (hyprparsews(ws, selmon, &id))
			hyprviewws(id);
		return "ok";
	}
	if (IS("focus")) {
		if (hyprarg(args, "workspace", v, sizeof v)) {
			if (hyprparsews(v, selmon, &id))
				hyprviewws(id);
			return "ok";
		}
		if (hyprarg(args, "monitor", v, sizeof v)) {
			Monitor *m;
			wl_list_for_each(m, &mons, link)
				if (!strcmp(m->wlr_output->name, v))
					selmon = m;
			focusclient(focustop(selmon), 1);
			return "ok";
		}
		if (!(c = hyprselect(sel)))
			return "no such window";
		hyprfocusclient(c);
		return "ok";
	}

	if (!(c = hyprselect(sel)))
		return "no such window";
	if (IS("window.close")) {
		client_send_close(c);
	} else if (IS("window.kill")) {
		if (macpid(c) > 1)
			kill(macpid(c), SIGKILL);
	} else if (IS("window.fullscreen_state")) {
		hyprarg(args, "internal", v, sizeof v);
		setfullscreen(c, atoi(v) == 2);
	} else if (IS("window.fullscreen")) {
		hyprarg(args, "mode", w, sizeof w);
		hyprarg(args, "action", v, sizeof v);
		if (!strcmp(w, "maximized")) {
			if (!strcmp(v, "toggle") || !*v || (!strcmp(v, "set") != !c->isfakefull))
				maczoom(c);
		} else {
			int on = !strcmp(v, "set") || !strcmp(v, "enable") ? 1
					: !strcmp(v, "unset") || !strcmp(v, "disable") ? 0
					: !c->isfullscreen;
			setfullscreen(c, on);
		}
	} else if (IS("window.float")) {
		hyprarg(args, "action", v, sizeof v);
		setfloating(c, !strcmp(v, "enable") ? 1 : !strcmp(v, "disable") ? 0
				: !c->isfloating);
	} else if (IS("window.pin") || IS("window.set_prop")) {
		/* no pinned windows in gluewc */
	} else if (IS("window.center")) {
		struct wlr_box b;
		hyprboxof(c, &b);
		hyprplace(c, c->mon->w.x + (c->mon->w.width - b.width) / 2,
				c->mon->w.y + (c->mon->w.height - b.height) / 2, 0, 0);
	} else if (IS("window.resize")) {
		struct wlr_box b;
		int x = 0, y = 0, rel;
		hyprboxof(c, &b);
		if (hyprarg(args, "x", v, sizeof v))
			x = atoi(v);
		if (hyprarg(args, "y", v, sizeof v))
			y = atoi(v);
		rel = hyprarg(args, "relative", v, sizeof v) && !strcmp(v, "true");
		if (rel)
			hyprplace(c, INT_MIN, INT_MIN, MAX(80, b.width + x), MAX(60, b.height + y));
		else
			hyprplace(c, INT_MIN, INT_MIN, x > 0 ? x : b.width, y > 0 ? y : b.height);
	} else if (IS("window.move")) {
		if (hyprarg(args, "workspace", v, sizeof v)) {
			if (hyprparsews(v, c->mon, &id))
				hyprmovews(c, id);
		} else if (hyprarg(args, "direction", v, sizeof v)) {
			if (!strcmp(v, "left") || !strcmp(v, "l"))
				mactileclient(c, TileLeft);
			else if (!strcmp(v, "right") || !strcmp(v, "r"))
				mactileclient(c, TileRight);
			else if (!strcmp(v, "up") || !strcmp(v, "u"))
				mactileclient(c, TileTop);
			else
				mactileclient(c, TileBottom);
		} else if (hyprarg(args, "monitor", v, sizeof v)) {
			Monitor *m = NULL, *o;
			int n = 0, i = hyprmonidx(c->mon);
			wl_list_for_each(o, &mons, link)
				n++;
			if (!strcmp(v, "+1") || !strcmp(v, "-1"))
				m = hyprmonat((i + (v[0] == '+' ? 1 : n - 1)) % MAX(1, n));
			else
				wl_list_for_each(o, &mons, link)
					if (!strcmp(o->wlr_output->name, v))
						m = o;
			if (m && m != c->mon && hyprlive(m))
				setmon(c, m, (int)m->ws);
		} else {
			int x = INT_MIN, y = INT_MIN;
			if (hyprarg(args, "x", v, sizeof v))
				x = atoi(v);
			if (hyprarg(args, "y", v, sizeof v))
				y = atoi(v);
			if (!hyprhasword(args, "exact") && x != INT_MIN && y != INT_MIN) {
				struct wlr_box b;
				hyprboxof(c, &b);
				x += b.x;
				y += b.y;
			}
			hyprplace(c, x, y, 0, 0);
		}
	} else {
		return "invalid dispatcher";
	}
#undef IS
	return "ok";

classic:
	{
		char d[64] = "";
		const char *rest;
		sscanf(e, "%63s", d);
		rest = e + strlen(d);
		while (*rest == ' ')
			rest++;
		if (!strcmp(d, "exec")) {
			hyprexec(rest);
		} else if (!strcmp(d, "exit")) {
			quit(NULL);
		} else if (!strcmp(d, "killactive")) {
			if ((c = hyprfocused()))
				client_send_close(c);
		} else if (!strcmp(d, "forcekillactive")) {
			if ((c = hyprfocused()) && macpid(c) > 1)
				kill(macpid(c), SIGKILL);
		} else if (!strcmp(d, "closewindow")) {
			if ((c = hyprselect(rest)))
				client_send_close(c);
		} else if (!strcmp(d, "focuswindow")) {
			if (!(c = hyprselect(rest)))
				return "no such window";
			hyprfocusclient(c);
		} else if (!strcmp(d, "workspace")) {
			if (hyprparsews(rest, selmon, &id))
				hyprviewws(id);
		} else if (!strcmp(d, "togglespecialworkspace")) {
			snprintf(v, sizeof v, "special:%s", *rest ? rest : "hidden");
			if (hyprparsews(v, selmon, &id))
				hyprviewws(id);
		} else if (!strcmp(d, "movetoworkspace") || !strcmp(d, "movetoworkspacesilent")) {
			char ws[64] = "";
			const char *comma = strchr(rest, ',');
			snprintf(ws, sizeof ws, "%.*s", comma ? (int)(comma - rest) : (int)strlen(rest), rest);
			c = comma ? hyprselect(comma + 1) : hyprfocused();
			if (c && hyprparsews(ws, c->mon, &id))
				hyprmovews(c, id);
		} else if (!strcmp(d, "fullscreen")) {
			if ((c = hyprfocused())) {
				if (atoi(rest) == 1)
					maczoom(c);
				else
					setfullscreen(c, !c->isfullscreen);
			}
		} else if (!strcmp(d, "togglefloating")) {
			if ((c = *rest ? hyprselect(rest) : hyprfocused()))
				setfloating(c, !c->isfloating);
		} else if (!strcmp(d, "pin") || !strcmp(d, "setprop") || !strcmp(d, "togglesplit")) {
			/* no pinning or split direction in gluewc */
		} else if (!strcmp(d, "centerwindow")) {
			if ((c = hyprfocused()))
				return hyprdispatchone("hl.dsp.window.center()");
		} else if (!strcmp(d, "switchxkblayout")) {
			const char *sp = strrchr(rest, ' ');
			hyprsetlayout(sp ? sp + 1 : rest);
		} else {
			return "invalid dispatcher";
		}
		return "ok";
	}
}

/* a Lua chunk: every hl.dispatch(...) in it, hl.config and hl.monitor */
static const char *
hypreval(const char *e)
{
	const char *p, *res = "ok";
	int any = 0;

	if (strstr(e, "hl.monitor("))
		hyprmonitorcfg(e), any = 1;
	if (strstr(e, "hl.config(") && strstr(e, "repeat_"))
		hyprinputcfg(e), any = 1;
	for (p = e; (p = strstr(p, "hl.dispatch(")); p += 12) {
		res = hyprdispatchone(p + 12);
		any = 1;
	}
	if (!any && strstr(e, "hl.dsp."))
		res = hyprdispatchone(strstr(e, "hl.dsp."));
	return res;
}

static void
hyprcommand(FILE *f, char *cmd)
{
	char *flags = "", *sp, *slash;
	int json;
	Client *c;

	while (*cmd == ' ')
		cmd++;
	/* hyprctl sends "j/clients", "/dispatch ...", Quickshell "dispatch ..." */
	sp = strchr(cmd, ' ');
	slash = strchr(cmd, '/');
	if (slash && (!sp || slash < sp) && strspn(cmd, "jarc") == (size_t)(slash - cmd)) {
		*slash = '\0';
		flags = cmd;
		cmd = slash + 1;
	}
	json = strchr(flags, 'j') != NULL;
	if (!strncmp(cmd, "dispatch ", 9)) {
		fputs(hyprdispatchone(cmd + 9), f);
	} else if (!strncmp(cmd, "eval ", 5)) {
		fputs(hypreval(cmd + 5), f);
	} else if (!strcmp(cmd, "clients")) {
		hyprjclients(f);
	} else if (!strncmp(cmd, "monitors", 8)) {
		hyprjmonitors(f);
	} else if (!strcmp(cmd, "workspaces")) {
		hyprjworkspaces(f);
	} else if (!strcmp(cmd, "activeworkspace")) {
		if (selmon)
			hyprjws(f, hyprwsof(selmon, selmon->ws), selmon);
		else
			fputs("{}", f);
	} else if (!strcmp(cmd, "activewindow")) {
		if ((c = hyprfocused()))
			hyprjclient(f, c);
		else
			fputs(json ? "{}" : "Invalid", f);
	} else if (!strcmp(cmd, "devices")) {
		hyprjdevices(f);
	} else if (!strcmp(cmd, "status")) {
		fputs("{\"configProvider\":\"lua\",\"compositor\":\"gluewc\"}", f);
	} else if (!strcmp(cmd, "version")) {
		fputs(json ? "{\"branch\":\"gluewc\",\"commit\":\"\",\"version\":\"0.56.0\","
				"\"dirty\":false,\"commit_message\":\"\",\"commit_date\":\"\",\"tag\":\"v0.56.0\","
				"\"commits\":\"0\",\"buildAquamarine\":\"\",\"flags\":[]}"
				: "gluewc (Hyprland IPC 0.56.0)", f);
	} else if (!strncmp(cmd, "switchxkblayout", 15)) {
		char *last = strrchr(cmd, ' ');
		hyprsetlayout(last ? last + 1 : "next");
		fputs("ok", f);
	} else if (!strcmp(cmd, "reload")) {
		reloadconfig(NULL);
		fputs("ok", f);
	} else if (!strncmp(cmd, "keyword ", 8) || !strncmp(cmd, "setcursor", 9)
			|| !strncmp(cmd, "dismissnotify", 13) || !strncmp(cmd, "notify", 6)) {
		fputs("ok", f);
	} else if (!strcmp(cmd, "layers") || !strcmp(cmd, "binds") || !strcmp(cmd, "globalshortcuts")
			|| !strcmp(cmd, "workspacerules") || !strcmp(cmd, "layouts")) {
		fputs(!strcmp(cmd, "layers") ? "{}" : "[]", f);
	} else if (!strcmp(cmd, "cursorpos")) {
		fprintf(f, json ? "{\"x\":%d,\"y\":%d}" : "%d, %d", (int)cursor->x, (int)cursor->y);
	} else if (!strncmp(cmd, "getoption ", 10)) {
		fputs(json ? "{\"option\":\"\",\"int\":0,\"set\":false}" : "no such option", f);
	} else {
		fputs("unknown request", f);
	}
}

static int
hyprreqread(int fd, uint32_t mask, void *data)
{
	HyprConn *hc = data;
	char buf[8192], *text = NULL, *cmd, *save = NULL;
	ssize_t n = 0, r;
	size_t len = 0, off = 0;
	FILE *f;

	while (n < (ssize_t)sizeof buf - 1 && (r = read(fd, buf + n, sizeof buf - 1 - n)) > 0)
		n += r;
	if (n <= 0 && !(mask & WL_EVENT_READABLE))
		goto done;
	buf[n > 0 ? n : 0] = '\0';
	if (!(f = open_memstream(&text, &len)))
		goto done;
	if (!strncmp(buf, "[[BATCH]]", 9)) {
		for (cmd = strtok_r(buf + 9, ";", &save); cmd; cmd = strtok_r(NULL, ";", &save)) {
			hyprcommand(f, cmd);
			fputs("\n\n", f);
		}
	} else {
		hyprcommand(f, buf);
	}
	fclose(f);
	{
		int fl = fcntl(fd, F_GETFL);
		fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
	}
	while (off < len && (r = write(fd, text + off, len - off)) > 0)
		off += (size_t)r;
	free(text);
done:
	wl_event_source_remove(hc->src);
	wl_list_remove(&hc->link);
	close(fd);
	free(hc);
	return 0;
}

static int
hypraccept(int lfd, uint32_t mask, void *data)
{
	struct wl_list *list = data;
	struct timeval tv = {0, 500000};
	HyprConn *hc;
	int fd;

	if ((fd = accept(lfd, NULL, NULL)) < 0)
		return 0;
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	hc = ecalloc(1, sizeof(*hc));
	hc->fd = fd;
	wl_list_insert(list, &hc->link);
	if (list == &hypr_reqs)
		hc->src = wl_event_loop_add_fd(wl_display_get_event_loop(dpy), fd,
				WL_EVENT_READABLE | WL_EVENT_HANGUP, hyprreqread, hc);
	return 0;
}

static int
hyprlisten(const char *name, struct wl_list *list, struct wl_event_source **src)
{
	struct sockaddr_un sa = {.sun_family = AF_UNIX};
	int fd;

	if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s/%s", hypr_dir, name)
			>= (int)sizeof sa.sun_path)
		return -1;
	unlink(sa.sun_path);
	if ((fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
		return -1;
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) || listen(fd, 64)) {
		close(fd);
		return -1;
	}
	*src = wl_event_loop_add_fd(wl_display_get_event_loop(dpy), fd,
			WL_EVENT_READABLE, hypraccept, list);
	return fd;
}

/* $XDG_RUNTIME_DIR/hypr/gluewc-<display>/.socket{,2}.sock; the session
 * hands HYPRLAND_INSTANCE_SIGNATURE to the shell only, so other programs
 * never take gluewc for Hyprland */
static void
hyprsetup(const char *socket)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	char base[200], path[300];
	FILE *f;

	wl_list_init(&hypr_reqs);
	wl_list_init(&hypr_listeners);
	if (!rt || !socket)
		return;
	snprintf(hypr_sig, sizeof hypr_sig, "gluewc-%s", socket);
	snprintf(base, sizeof base, "%s/hypr", rt);
	mkdir(base, 0700);
	snprintf(hypr_dir, sizeof hypr_dir, "%s/%s", base, hypr_sig);
	mkdir(hypr_dir, 0700);
	hypr_req_fd = hyprlisten(".socket.sock", &hypr_reqs, &hypr_req_src);
	hypr_evt_fd = hyprlisten(".socket2.sock", &hypr_listeners, &hypr_evt_src);
	if (hypr_req_fd < 0 || hypr_evt_fd < 0)
		wlr_log(WLR_ERROR, "hyprland ipc: cannot listen in %s", hypr_dir);
	if (statefile(path, sizeof path, "hypr-signature") && (f = fopen(path, "w"))) {
		fprintf(f, "%s\n", hypr_sig);
		fclose(f);
	}
}

static void
hyprcleanup(void)
{
	HyprConn *hc, *tmp;
	char path[320];

	if (!hypr_dir[0])
		return;
	wl_list_for_each_safe(hc, tmp, &hypr_listeners, link) {
		close(hc->fd);
		wl_list_remove(&hc->link);
		free(hc);
	}
	wl_list_for_each_safe(hc, tmp, &hypr_reqs, link) {
		wl_event_source_remove(hc->src);
		close(hc->fd);
		wl_list_remove(&hc->link);
		free(hc);
	}
	if (hypr_req_src)
		wl_event_source_remove(hypr_req_src);
	if (hypr_evt_src)
		wl_event_source_remove(hypr_evt_src);
	if (hypr_req_fd >= 0)
		close(hypr_req_fd);
	if (hypr_evt_fd >= 0)
		close(hypr_evt_fd);
	snprintf(path, sizeof path, "%s/.socket.sock", hypr_dir);
	unlink(path);
	snprintf(path, sizeof path, "%s/.socket2.sock", hypr_dir);
	unlink(path);
	rmdir(hypr_dir);
}

static void
hyprevent(const char *fmt, ...)
{
	HyprConn *hc, *tmp;
	char line[1024];
	va_list ap;
	int n;

	if (wl_list_empty(&hypr_listeners))
		return;
	va_start(ap, fmt);
	n = vsnprintf(line, sizeof line - 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if (n > (int)sizeof line - 2)
		n = (int)sizeof line - 2;
	line[n++] = '\n';
	wl_list_for_each_safe(hc, tmp, &hypr_listeners, link) {
		if (send(hc->fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) != n) {
			close(hc->fd);
			wl_list_remove(&hc->link);
			free(hc);
		}
	}
}

static int
hyprhasid(const int *ids, size_t n, int id)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (ids[i] == id)
			return 1;
	return 0;
}

/* compares the world with what the shell was last told and sends the
 * difference, in the order Hyprland would */
static void
hyprsync(void)
{
	HyprWin *now = NULL, *p;
	size_t nnow = 0, i, j, nws;
	int *ws;
	char name[32], wsn[32];
	Monitor *m;
	Client *c, *foc;
	int nm = 0;
	uintptr_t active;

	if (hypr_evt_fd < 0)
		return;
	/* outputs */
	if (hypr_nprevmons >= 0) {
		wl_list_for_each(m, &mons, link) {
			int known = 0;
			if (!hyprlive(m))
				continue;
			for (i = 0; i < (size_t)hypr_nprevmons; i++)
				known |= !strcmp(hypr_prevmons[i], m->wlr_output->name);
			if (!known) {
				hyprevent("monitoradded>>%s", m->wlr_output->name);
				hyprevent("monitoraddedv2>>%d,%s,%s", hyprmonidx(m), m->wlr_output->name,
						m->wlr_output->description ? m->wlr_output->description : "");
			}
		}
		for (i = 0; i < (size_t)hypr_nprevmons; i++) {
			int alive = 0;
			wl_list_for_each(m, &mons, link)
				alive |= hyprlive(m) && !strcmp(hypr_prevmons[i], m->wlr_output->name);
			if (!alive) {
				hyprevent("monitorremoved>>%s", hypr_prevmons[i]);
				hyprevent("monitorremovedv2>>%d,%s,%s", (int)i, hypr_prevmons[i], "");
			}
		}
	}
	/* workspaces that appeared */
	nws = hyprwslist(&ws);
	for (i = 0; i < nws; i++) {
		if (hyprhasid(hypr_prevws, hypr_nprevws, ws[i]))
			continue;
		hyprwsname(ws[i], name, sizeof name);
		hyprevent("createworkspace>>%s", name);
		hyprevent("createworkspacev2>>%d,%s", ws[i], name);
	}
	/* windows */
	wl_list_for_each_reverse(c, &clients, link) {
		if (!hyprlisted(c))
			continue;
		now = realloc(now, (nnow + 1) * sizeof(*now));
		p = &now[nnow++];
		memset(p, 0, sizeof(*p));
		p->addr = (uintptr_t)c;
		p->ws = hyprclientws(c);
		p->fs = c->isfullscreen;
		p->min = hyprminimized(c);
		snprintf(p->title, sizeof p->title, "%s", client_get_title(c) ? client_get_title(c) : "");
		snprintf(p->cls, sizeof p->cls, "%s", client_get_appid(c) ? client_get_appid(c) : "");
	}
	for (i = 0; i < hypr_nprev; i++)
		hypr_prev[i].seen = 0;
	for (j = 0; j < nnow; j++) {
		HyprWin *o = NULL;
		for (i = 0; i < hypr_nprev; i++)
			if (hypr_prev[i].addr == now[j].addr)
				o = &hypr_prev[i];
		hyprwsname(now[j].ws, wsn, sizeof wsn);
		if (!o) {
			hyprevent("openwindow>>%lx,%s,%s,%s", (unsigned long)now[j].addr, wsn,
					now[j].cls, now[j].title);
			continue;
		}
		o->seen = 1;
		if (o->ws != now[j].ws) {
			hyprevent("movewindow>>%lx,%s", (unsigned long)now[j].addr, wsn);
			hyprevent("movewindowv2>>%lx,%d,%s", (unsigned long)now[j].addr, now[j].ws, wsn);
		}
		if (strcmp(o->title, now[j].title)) {
			hyprevent("windowtitle>>%lx", (unsigned long)now[j].addr);
			hyprevent("windowtitlev2>>%lx,%s", (unsigned long)now[j].addr, now[j].title);
		}
		if (!!o->min != !!now[j].min)
			hyprevent("minimized>>%lx,%d", (unsigned long)now[j].addr, !!now[j].min);
	}
	for (i = 0; i < hypr_nprev; i++)
		if (!hypr_prev[i].seen)
			hyprevent("closewindow>>%lx", (unsigned long)hypr_prev[i].addr);
	/* workspaces that went away */
	for (i = 0; i < hypr_nprevws; i++) {
		if (hyprhasid(ws, nws, hypr_prevws[i]))
			continue;
		hyprwsname(hypr_prevws[i], name, sizeof name);
		hyprevent("destroyworkspace>>%s", name);
		hyprevent("destroyworkspacev2>>%d,%s", hypr_prevws[i], name);
	}
	/* the visible workspace of every output, then the focused output */
	wl_list_for_each(m, &mons, link) {
		int id = hyprwsof(m, m->ws), was = -1;
		if (!hyprlive(m) || nm >= 16)
			continue;
		for (i = 0; hypr_nprevmons >= 0 && i < (size_t)hypr_nprevmons; i++)
			if (!strcmp(hypr_prevmons[i], m->wlr_output->name))
				was = hypr_prevmonws[i];
		if (was != id && m != selmon) {
			hyprwsname(id, name, sizeof name);
			hyprevent("workspace>>%s", name);
			hyprevent("workspacev2>>%d,%s", id, name);
		}
		snprintf(hypr_prevmons[nm], sizeof hypr_prevmons[nm], "%s", m->wlr_output->name);
		hypr_prevmonws[nm++] = id;
	}
	hypr_nprevmons = nm;
	if (selmon && hyprlive(selmon)) {
		int id = hyprwsof(selmon, selmon->ws);
		hyprwsname(id, name, sizeof name);
		if (strcmp(hypr_prevfocmon, selmon->wlr_output->name)) {
			hyprevent("focusedmon>>%s,%s", selmon->wlr_output->name, name);
			hyprevent("focusedmonv2>>%s,%d", selmon->wlr_output->name, id);
			snprintf(hypr_prevfocmon, sizeof hypr_prevfocmon, "%s", selmon->wlr_output->name);
		}
		if (hypr_prevfocws != id) {
			hyprevent("workspace>>%s", name);
			hyprevent("workspacev2>>%d,%s", id, name);
			hypr_prevfocws = id;
		}
	}
	/* focus */
	foc = hyprfocused();
	active = (uintptr_t)foc;
	if (active != hypr_prevactive) {
		if (foc) {
			hyprevent("activewindow>>%s,%s", client_get_appid(foc) ? client_get_appid(foc) : "",
					client_get_title(foc) ? client_get_title(foc) : "");
			hyprevent("activewindowv2>>%lx", (unsigned long)active);
		} else {
			hyprevent("activewindow>>,");
			hyprevent("activewindowv2>>");
		}
		hypr_prevactive = active;
	}
	if (hypr_prevfs != (foc && foc->isfullscreen)) {
		if (hypr_prevfs != -1)
			hyprevent("fullscreen>>%d", foc && foc->isfullscreen);
		hypr_prevfs = foc && foc->isfullscreen;
	}
	free(hypr_prev);
	hypr_prev = now;
	hypr_nprev = nnow;
	free(hypr_prevws);
	hypr_prevws = ws;
	hypr_nprevws = nws;
}
