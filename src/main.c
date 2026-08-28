/* SPDX-License-Identifier: 0BSD
 *
 * PlayOS -- a custom firmware for the TrimUI Brick that plays NES, TurboGrafx
 * -16 and Game Boy Advance games, and does nothing else.
 *
 * The whole design metric is speed. The launcher starts behind the boot
 * animation rather than after it, hands games to an emulator that is already
 * running rather than starting one, and never tears its own display down --
 * so coming back from a game is a frame, not a second and a half. What is on
 * screen is a row of cards, the name of the thing under the cursor, and a
 * rail saying where you are. Nothing else.
 */
#include "config.h"
#include "coverflow.h"
#include "library.h"
#include "platform.h"
#include "ui.h"

#include <SDL.h>
#include <SDL_image.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define BATT_LOW_PCT   10   /* show the low-battery dot at or below this */
#define TEX_KEEP_NEAR   8   /* card textures kept around the cursor, in view  */
#define TEX_KEEP_FAR    4   /* ...and around the cursor of a system you left  */

typedef enum { SCREEN_SYSTEMS, SCREEN_GAMES } screen_id;

typedef struct {
	game_list list;
	SDL_Texture **tex;
	int *tw, *th;
	int cursor;
	int dmode;              /* index into DMODES: how this system is scaled */
	coverflow cf;
} sysview;

/* Diatom's display modes, in the order PlayOS offers them: the sensible
 * default first, then whole-pixel scaling, then the ones that trade shape or
 * edges for coverage, with 1:1 last as a reference rather than a choice.
 *
 * The names are Diatom's protocol strings and have to match its own table in
 * src/scale.c exactly - it answers an unknown one with ERROR code=bad_display
 * and changes nothing. The labels are ours, and are what the menu shows. */
static const struct { const char *name, *label; } DMODES[] = {
	{ "aspect",           "Aspect"       },
	{ "integer",          "Integer"      },
	{ "integer-vertical", "Integer tall" },
	{ "overscale",        "Overscale"    },
	{ "fill",             "Fill"         },
	{ "stretch",          "Stretch"      },
	{ "native",           "Native 1:1"   },
};
#define DMODE_COUNT ((int)(sizeof DMODES / sizeof DMODES[0]))

typedef struct {
	systems_cfg sys;
	playos_cfg cfg;

	SDL_Texture *sys_tex[CFG_MAX_SYSTEMS];
	int sys_w[CFG_MAX_SYSTEMS], sys_h[CFG_MAX_SYSTEMS];
	sysview view[CFG_MAX_SYSTEMS];

	screen_id screen;
	int sys_cursor;
	int menu_w;             /* cached shelf-menu content width; 0 = unmeasured */
	coverflow cf_sys;
	unsigned tint;          /* eased toward the focused system's accent */
	in_state in;
	bool running;
	SDL_Renderer *r;
} app;

static volatile sig_atomic_t want_quit;
static void on_sigterm(int sig) { (void)sig; want_quit = 1; }

/* Boot time is the one number a launcher cannot be careless about, so the
 * phases are timed and logged rather than guessed at. On the project this
 * grew out of, the phase everyone assumed was expensive turned out to be 8%
 * of startup -- which is only knowable by measuring. */
static unsigned t_boot0;
static void t_mark(const char *what)
{
	fprintf(stderr, "boot: %-14s %5u ms\n", what, plat_now_ms() - t_boot0);
}

/* One number for the way back, because this path keeps being asked how fast it
 * is and a number is how the answer stays true. The five phase marks that took
 * it from 213ms to here are in the history if it ever needs breaking down
 * again; the budget they found was 5ms to black, 4ms of bookkeeping, and the
 * rest decoding the focused card's autosave preview off the SD card. */
static unsigned t_back0;

/* Get off the game's last frame before doing anything slow.
 *
 * Everything between EXIT and the first frame of the fade blocks and none of it
 * presents, so the panel holds whatever the launcher drew last - which after a
 * quit is the in-game menu, over a frame of the game that has ended. Measured
 * on the device: 123ms of it, 52 in plat_leds_off's seventy-nine sysfs writes
 * and 69 rebuilding the focused card off the SD card.
 *
 * One present, black, because the fade that follows starts from black and the
 * two should agree. This is not the buffer-establishing that was tried and
 * reverted: that guarded a stale back buffer, which a swap cannot show. This
 * gets a stale FRONT frame off the glass, which is measurable and was measured.
 */
static void present_black(app *a)
{
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_NONE);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
	SDL_RenderClear(a->r);
	SDL_RenderPresent(a->r);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
}

/* The boot animation plays in a background process while everything below
 * runs -- the card scan, GL init, font and asset decode, the lot. Both it and
 * this process draw to /dev/fb0, so presenting now would fight it: last
 * writer wins, at 60fps against its 30. Wait for it to clear the marker, then
 * draw.
 *
 * Bounded, because a boot that hangs behind a stuck decoder would be a far
 * worse bug than a seam in an animation. An absent marker means no animation
 * is playing -- every launcher restart after this one -- and this returns at
 * once. */
static void wait_for_boot_anim(void)
{
	const char *flag = getenv("PLAYOS_ANIM_FLAG");
	unsigned start;

	if (!flag || !*flag || access(flag, F_OK) != 0) return;
	start = plat_now_ms();
	while (access(flag, F_OK) == 0) {
		if (plat_now_ms() - start > 8000u) {
			fprintf(stderr, "boot: animation flag stuck, drawing anyway\n");
			break;
		}
		SDL_Delay(8);
	}
	t_mark("anim wait");
}

/* ---------- per-system display mode --------------------------------------- */

/* Keyed on the system's tag rather than its folder or its place in the list,
 * for the same reason saves are: renaming a ROM folder or reordering
 * systems.cfg then cannot quietly hand a system somebody else's setting. */
static void display_load(app *a)
{
	char p[CFG_STR * 2], line[256];
	FILE *f;

	snprintf(p, sizeof p, "%s/display.cfg", P_USERDATA);
	f = fopen(p, "r");
	if (!f) return;
	while (fgets(line, sizeof line, f)) {
		char *eq;
		int i, k;

		line[strcspn(line, "\r\n")] = '\0';
		eq = strchr(line, '=');
		if (!eq) continue;
		*eq++ = '\0';
		for (i = 0; i < a->sys.count; i++) {
			if (strcmp(a->sys.systems[i].tag, line) != 0) continue;
			for (k = 0; k < DMODE_COUNT; k++)
				if (strcmp(DMODES[k].name, eq) == 0) { a->view[i].dmode = k; break; }
			break;
		}
	}
	fclose(f);
}

/* Every system every time: the file is one short line each, and rewriting the
 * lot means there is no way for it to drift out of step with systems.cfg. */
static void display_save(app *a)
{
	char p[CFG_STR * 2];
	FILE *f;
	int i;

	snprintf(p, sizeof p, "%s/display.cfg", P_USERDATA);
	f = fopen(p, "w");
	if (!f) return;
	for (i = 0; i < a->sys.count; i++)
		fprintf(f, "%s=%s\n", a->sys.systems[i].tag, DMODES[a->view[i].dmode].name);
	fclose(f);
}

/* ---------- textures ----------------------------------------------------- */

static SDL_Texture *load_image(SDL_Renderer *r, const char *path, int *w, int *h)
{
	SDL_Texture *t = IMG_LoadTexture(r, path);
	if (t) SDL_QueryTexture(t, NULL, NULL, w, h);
	return t;
}

static SDL_Texture *sys_get_tex(void *ctx, int i, int *w, int *h)
{
	app *a = ctx;
	if (!a->sys_tex[i]) {
		char path[CFG_STR * 2];
		snprintf(path, sizeof path, "%s/cards/%s", P_ROOT, a->sys.systems[i].card);
		a->sys_tex[i] = load_image(a->r, path, &a->sys_w[i], &a->sys_h[i]);
		if (!a->sys_tex[i])
			a->sys_tex[i] = ui_make_card(a->r, a->sys.systems[i].name,
			                             a->sys.systems[i].accent,
			                             &a->sys_w[i], &a->sys_h[i]);
	}
	*w = a->sys_w[i];
	*h = a->sys_h[i];
	return a->sys_tex[i];
}

/* Where a game's autosave preview lives: the frame the player was looking at
 * when they stopped. The directory is named after the ROM's folder under
 * Roms/.
 *
 * The directory and the slot names are PlayOS's own. Both were briefly
 * borrowed from the emulator PlayOS replaced, and the borrowing cost more than
 * it saved: a name that describes another project invites a reader to treat
 * the contents as dead, and on 2026-08-28 the live autosave tree was deleted
 * by someone auditing the card for exactly that reason. Nothing was lost only
 * because states were being cleared for testing the same hour.
 *
 * Renamed with no migration path, deliberately - nothing has shipped, so there
 * are no cards in the world to be kind to. */
/* Six manual save slots and one autosave.
 *
 * Six because it is enough and it fits the carousel's dot rail without
 * crowding, not because any particular number is correct. An earlier eight was
 * copied from another launcher and had no reasoning of its own behind it.
 *
 * The resume slot is named rather than numbered on disk: `<game>.auto.state`
 * explains itself, where a number needs a reader to already know which slot
 * means auto. Its value is only an internal sentinel and must not collide with
 * 1..GM_SLOTS, so it is derived rather than picked. */
#define GM_SLOTS  6
#define SLOT_AUTO (GM_SLOTS + 1)

static const char *slot_name(int slot)
{
	static char buf[8];
	if (slot == SLOT_AUTO) return "auto";
	snprintf(buf, sizeof buf, "%d", slot);
	return buf;
}

static void preview_path(app *a, int s, const game_entry *g, char *out, size_t n)
{
	const char *base = strrchr(g->file, '/');
	base = base ? base + 1 : g->file;
	snprintf(out, n, "%s/.playos/%s/%s.%s.bmp",
	         P_SHARED, a->sys.systems[s].folder, base, slot_name(SLOT_AUTO));
}

/* The Diatom transport takes explicit paths rather than a slot number, so the
 * state lives beside the preview it belongs to, named the same way.
 *
 * The autosave is named `auto`, not a number. Slots 1..GM_SLOTS are the
 * player's own,
 * reachable from the in-game menu, and are genuinely numbered; the resume slot
 * is not one of them and calling it 9 only meant something to someone who knew
 * which emulator picked that number. Each slot is a state and a preview named
 * alike, so a slot that has a picture has a game behind it. */
static void slot_state_path(app *a, int s, const game_entry *g, int slot,
                            char *out, size_t n)
{
	const char *base = strrchr(g->file, '/');
	base = base ? base + 1 : g->file;
	snprintf(out, n, "%s/.playos/%s/%s.%s.state",
	         P_SHARED, a->sys.systems[s].folder, base, slot_name(slot));
}

static void slot_preview_path(app *a, int s, const game_entry *g, int slot,
                              char *out, size_t n)
{
	const char *base = strrchr(g->file, '/');
	base = base ? base + 1 : g->file;
	snprintf(out, n, "%s/.playos/%s/%s.%s.bmp",
	         P_SHARED, a->sys.systems[s].folder, base, slot_name(slot));
}

static void state_path(app *a, int s, const game_entry *g, char *out, size_t n)
{
	slot_state_path(a, s, g, SLOT_AUTO, out, n);
}

/* The launcher owns the paths, so the launcher makes the directories - the
 * emulator writes where it is told and fails where it cannot. A missing mkdir
 * here once cost every preview on the card, silently, because writing to a
 * path in a directory that does not exist fails per-write and looks like
 * nothing happening. */
static void persist_dir_ensure(app *a, int s)
{
	char d[LIB_PATH * 2];
	snprintf(d, sizeof d, "%s/.playos", P_SHARED);
	mkdir(d, 0755);
	snprintf(d, sizeof d, "%s/.playos/%s", P_SHARED, a->sys.systems[s].folder);
	mkdir(d, 0755);
}

static bool game_has_state(app *a, int s, game_entry *g)
{
	if (!g->state_known) {
		char p[LIB_PATH * 2];
		struct stat st;
		preview_path(a, s, g, p, sizeof p);
		g->has_state = (stat(p, &st) == 0 && st.st_size > 0);
		g->state_known = 1;
	}
	return g->has_state != 0;
}

/* Card art, in order of preference: the box art the user put in .media/, then
 * the autosave preview -- the last frame they saw, which for a game in
 * progress is a better card than any box -- then a generated slab. */
static SDL_Texture *game_get_tex(void *ctx, int i, int *w, int *h)
{
	app *a = ctx;
	int s = a->sys_cursor;
	sysview *v = &a->view[s];

	if (!v->tex[i]) {
		char path[LIB_PATH * 3];
		snprintf(path, sizeof path, "%s/%s/.media/%s.png",
		         P_ROMS, a->sys.systems[s].folder, v->list.items[i].name);
		v->tex[i] = load_image(a->r, path, &v->tw[i], &v->th[i]);
		if (!v->tex[i]) {
			preview_path(a, s, &v->list.items[i], path, sizeof path);
			v->tex[i] = load_image(a->r, path, &v->tw[i], &v->th[i]);
		}
		if (!v->tex[i])
			v->tex[i] = ui_make_card(a->r, v->list.items[i].title,
			                         a->sys.systems[s].accent,
			                         &v->tw[i], &v->th[i]);
	}
	*w = v->tw[i];
	*h = v->th[i];
	return v->tex[i];
}

static void evict_far(sysview *v, int keep)
{
	for (int i = 0; i < v->list.count; i++) {
		int d = abs(i - v->cursor);
		if (v->list.count >= CF_WINDOW)
			d = d > v->list.count / 2 ? v->list.count - d : d;
		if (v->tex[i] && d > keep) {
			SDL_DestroyTexture(v->tex[i]);
			v->tex[i] = NULL;
		}
	}
}

/* Decode the cards across the visible window up front, so the first scroll
 * does not hitch while a PNG decodes in the middle of the slide. */
static void prime_window(app *a, int s)
{
	sysview *v = &a->view[s];
	int span = CF_HALF_WINDOW + 2, k, save = a->sys_cursor;

	if (v->list.count <= 0) return;
	a->sys_cursor = s;
	for (k = -span; k <= span; k++) {
		int i = v->cursor + k, w, h;
		if (v->list.count >= 2) {
			i %= v->list.count;
			if (i < 0) i += v->list.count;
		} else if (i < 0 || i >= v->list.count) {
			continue;
		}
		game_get_tex(a, i, &w, &h);
	}
	a->sys_cursor = save;
}

static void prime_sys_window(app *a)
{
	for (int i = 0; i < a->sys.count; i++) {
		int w, h;
		sys_get_tex(a, i, &w, &h);
	}
}

static void free_all_textures(app *a)
{
	for (int i = 0; i < a->sys.count; i++) {
		if (a->sys_tex[i]) { SDL_DestroyTexture(a->sys_tex[i]); a->sys_tex[i] = NULL; }
		for (int k = 0; k < a->view[i].list.count; k++)
			if (a->view[i].tex[k]) {
				SDL_DestroyTexture(a->view[i].tex[k]);
				a->view[i].tex[k] = NULL;
			}
	}
}

/* ---------- where you were ------------------------------------------------ */

/* Coming back to the shelf you left is worth four lines of file handling: the
 * launcher restarts after every game, and starting at the beginning of the
 * list every time would undo the point of it being quick. */
static void remember_place(app *a)
{
	char p[CFG_STR * 2];
	sysview *v = &a->view[a->sys_cursor];
	FILE *f;
	snprintf(p, sizeof p, "%s/.last", P_ROOT);
	f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "%s\n%s\n", a->sys.systems[a->sys_cursor].tag,
	        v->list.count ? v->list.items[v->cursor].file : "");
	fclose(f);
}

static void restore_place(app *a)
{
	char p[CFG_STR * 2], tag[64] = { 0 }, file[LIB_PATH] = { 0 };
	FILE *f;
	snprintf(p, sizeof p, "%s/.last", P_ROOT);
	f = fopen(p, "r");
	if (!f) return;
	if (fgets(tag, sizeof tag, f)) tag[strcspn(tag, "\r\n")] = 0;
	if (fgets(file, sizeof file, f)) file[strcspn(file, "\r\n")] = 0;
	fclose(f);
	for (int i = 0; i < a->sys.count; i++) {
		if (strcmp(a->sys.systems[i].tag, tag) != 0) continue;
		a->sys_cursor = i;
		for (int k = 0; k < a->view[i].list.count; k++)
			if (strcmp(a->view[i].list.items[k].file, file) == 0) {
				a->view[i].cursor = k;
				break;
			}
		return;
	}
}

/* ---------- drawing ------------------------------------------------------- */

static void draw_triangle(SDL_Renderer *r, float cx, float cy, float size,
                          SDL_Color col)
{
	SDL_Vertex v[3];
	static const int idx[3] = { 0, 1, 2 };
	float h = size, w = size * 0.88f;
	v[0].position.x = cx - w * 0.42f; v[0].position.y = cy - h * 0.5f;
	v[1].position.x = cx + w * 0.58f; v[1].position.y = cy;
	v[2].position.x = cx - w * 0.42f; v[2].position.y = cy + h * 0.5f;
	for (int i = 0; i < 3; i++) {
		v[i].color = col;
		v[i].tex_coord.x = v[i].tex_coord.y = 0;
	}
	SDL_RenderGeometry(r, NULL, v, 3, idx, 3);
}

static bool battery_low(void)
{
	static Uint32 next_check;
	static bool low;
	Uint32 now = SDL_GetTicks();
	if (next_check == 0 || now >= next_check) {
		next_check = now + 5000;
		int pct;
		bool charging;
		low = plat_battery(&pct, &charging) && !charging && pct <= BATT_LOW_PCT;
	}
	return low;
}

/* The one piece of chrome: a small accent disc, top right, when the battery is
 * low. Drawn with horizontal spans -- SDL has no circle. */
static void draw_low_battery_dot(SDL_Renderer *r)
{
	int cx = PLAYOS_SCREEN_W - 34, cy = 34, rad = 9;
	SDL_SetRenderDrawColor(r, 224, 72, 72, 255);
	for (int dy = -rad; dy <= rad; dy++) {
		int dx = (int)(sqrt((double)(rad * rad - dy * dy)) + 0.5);
		SDL_RenderDrawLine(r, cx - dx, cy + dy, cx + dx, cy + dy);
	}
}

static void draw_background(app *a)
{
	SDL_Renderer *r = a->r;
	SDL_SetRenderDrawColor(r, UI_BG_R, UI_BG_G, UI_BG_B, 255);
	SDL_RenderClear(r);
	/* A wash of the focused system's color along the bottom edge, so the
	 * whole screen belongs to the machine you are looking at. Faint enough to
	 * read as light rather than as a panel. */
	{
		SDL_Rect band = { 0, PLAYOS_SCREEN_H - 240, PLAYOS_SCREEN_W, 480 };
		ui_glow(r, &band, a->tint, 34, 1.7f);
	}
}

static void draw_systems(app *a)
{
	SDL_Rect focus;
	const system_cfg *s = &a->sys.systems[a->sys_cursor];
	char line[128];

	cf_focus_rect(&CF_LAYOUT_SYSTEMS, PLAYOS_SCREEN_W, PLAYOS_SCREEN_H, &focus);
	ui_glow(a->r, &focus, s->accent, 110, 2.4f);
	cf_draw(&a->cf_sys, a->r, PLAYOS_SCREEN_W, PLAYOS_SCREEN_H, a->sys.count,
	        sys_get_tex, a, &CF_LAYOUT_SYSTEMS);

	if (a->view[a->sys_cursor].list.count > 0)
		snprintf(line, sizeof line, "%d games", a->view[a->sys_cursor].list.count);
	else
		snprintf(line, sizeof line, "no games in Roms/%s", s->folder);
	ui_text(a->r, ui_font(UI_F_META), line, PLAYOS_SCREEN_W / 2, 690, 0, UI_TEXT_DIM);
	ui_rail(a->r, PLAYOS_SCREEN_W, PLAYOS_SCREEN_H, a->sys_cursor, a->sys.count,
	        s->accent);
}

static void draw_games(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	const system_cfg *s = &a->sys.systems[a->sys_cursor];
	SDL_Rect focus;
	char count[64];

	cf_focus_rect(&CF_LAYOUT_GAMES, PLAYOS_SCREEN_W, PLAYOS_SCREEN_H, &focus);
	ui_glow(a->r, &focus, s->accent, 100, 2.3f);
	cf_draw(&v->cf, a->r, PLAYOS_SCREEN_W, PLAYOS_SCREEN_H, v->list.count,
	        game_get_tex, a, &CF_LAYOUT_GAMES);
	evict_far(v, TEX_KEEP_NEAR);

	if (v->list.count > 0) {
		game_entry *g = &v->list.items[v->cursor];
		int tw = ui_text_width(ui_font(UI_F_TITLE), g->title);
		int tx = PLAYOS_SCREEN_W / 2;
		/* A game with an autosave gets a dot in the system's color beside
		 * its name: pressing A on it does not start it, it continues it.
		 * Sized and centered off the title's own line, so it keeps sitting
		 * with the text when the type scale moves. */
		if (game_has_state(a, a->sys_cursor, g)) {
			int line = ui_font_line(UI_F_TITLE);
			int rad = line / 8, dx = tx - tw / 2 - rad * 3, dy = 40 + line / 2;
			SDL_SetRenderDrawColor(a->r, (Uint8)(s->accent >> 16),
			                       (Uint8)(s->accent >> 8), (Uint8)s->accent, 255);
			for (int k = -rad; k <= rad; k++) {
				int w = (int)(sqrt((double)(rad * rad - k * k)) + 0.5);
				SDL_RenderDrawLine(a->r, dx - w, dy + k, dx + w, dy + k);
			}
		}
		ui_text(a->r, ui_font(UI_F_TITLE), g->title, tx, 40, 0, UI_TEXT);
		snprintf(count, sizeof count, "%d / %d", v->cursor + 1, v->list.count);
		ui_text(a->r, ui_font(UI_F_META), count, PLAYOS_SCREEN_W / 2, 690, 0,
		        UI_TEXT_DIM);
	} else {
		ui_text(a->r, ui_font(UI_F_TITLE), s->name, PLAYOS_SCREEN_W / 2, 40, 0,
		        UI_TEXT);
	}
	ui_rail(a->r, PLAYOS_SCREEN_W, PLAYOS_SCREEN_H, v->cursor, v->list.count,
	        s->accent);
}

/* The shelf, without presenting it: the options menu draws over a live one, so
 * the cards keep their tint easing behind the panel rather than freezing into
 * a still. */
static void draw_shelf(app *a)
{
	draw_background(a);
	if (a->screen == SCREEN_SYSTEMS) draw_systems(a);
	else draw_games(a);
	if (battery_low()) draw_low_battery_dot(a->r);
}

static void render(app *a)
{
	draw_shelf(a);
	plat_draw_osd(a->r);
	SDL_RenderPresent(a->r);
}

/* Ease the background tint toward the focused system rather than snapping: the
 * color is meant to feel like the light the machine gives off, and light does
 * not cut. Called from every loop that draws the shelf. */
static unsigned last_tint_ms;
static void tick_tint(app *a)
{
	unsigned now = plat_now_ms();
	float dt = (float)(now - last_tint_ms) / 1000.0f;
	last_tint_ms = now;
	if (dt > 0.1f) dt = 0.1f;
	a->tint = ui_mix(a->tint, a->sys.systems[a->sys_cursor].accent,
	                 1.0f - expf(-dt * 9.0f));
}

/* ---------- transitions --------------------------------------------------- */

/* Starting a game: the focused card comes at you and the screen goes with it.
 * Played WHILE the resident emulator loads the ROM, not before -- the whole
 * ~200ms of a launch is covered by it, so the animation costs nothing. */
static void anim_launch(app *a, unsigned ms)
{
	sysview *v = &a->view[a->sys_cursor];
	unsigned t0 = plat_now_ms(), now;
	SDL_Rect from;
	int tw = 0, th = 0;
	SDL_Texture *card;

	cf_focus_rect(&CF_LAYOUT_GAMES, PLAYOS_SCREEN_W, PLAYOS_SCREEN_H, &from);
	card = v->list.count ? game_get_tex(a, v->cursor, &tw, &th) : NULL;

	while ((now = plat_now_ms()) - t0 < ms) {
		float k = (float)(now - t0) / (float)ms;
		float e = k * k;                     /* accelerate away */
		float scale = 1.0f + e * 2.2f;
		SDL_Rect dst;

		SDL_SetRenderDrawColor(a->r, UI_BG_R, UI_BG_G, UI_BG_B, 255);
		SDL_RenderClear(a->r);
		if (card) {
			dst.w = (int)(from.w * scale);
			dst.h = (int)(from.h * scale);
			dst.x = PLAYOS_SCREEN_W / 2 - dst.w / 2;
			dst.y = PLAYOS_SCREEN_H / 2 - dst.h / 2;
			SDL_SetTextureAlphaMod(card, (Uint8)(255 * (1.0f - k)));
			SDL_RenderCopy(a->r, card, NULL, &dst);
			SDL_SetTextureAlphaMod(card, 255);
		}
		SDL_RenderPresent(a->r);
		SDL_Delay(6);
	}
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
	SDL_RenderClear(a->r);
	SDL_RenderPresent(a->r);
}

/* The send-off. The mark collapses to a point and the screen goes with it --
 * the same triangle the boot animation draws, run backwards. */
static void anim_poweroff(app *a)
{
	unsigned t0 = plat_now_ms(), now, ms = 620;
	while ((now = plat_now_ms()) - t0 < ms) {
		float k = (float)(now - t0) / (float)ms;
		float e = 1.0f - (1.0f - k) * (1.0f - k);
		SDL_Color col = { UI_CYAN_R, UI_CYAN_G, UI_CYAN_B, (Uint8)(255 * (1.0f - e)) };
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
		SDL_RenderClear(a->r);
		draw_triangle(a->r, PLAYOS_SCREEN_W / 2.0f, PLAYOS_SCREEN_H / 2.0f,
		              160.0f * (1.0f - e * 0.94f), col);
		SDL_RenderPresent(a->r);
		SDL_Delay(6);
	}
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
	SDL_RenderClear(a->r);
	SDL_RenderPresent(a->r);
}

static void power_off(app *a)
{
	/* Here rather than only on the return path: launch.sh runs its leds_off at
	 * the TOP of its restart loop, and a power-off breaks that loop instead of
	 * going round it, so this is the last chance to darken them. */
	plat_leds_off();
	remember_place(a);
	plat_request_poweroff();
	anim_poweroff(a);
	a->running = false;
}

/* ---------- menus --------------------------------------------------------- */

/* Both menus in PlayOS are the same shape - a short list on a slab, over a
 * paused game frame or over the shelf - so they are drawn by one function and
 * cannot drift apart. The slab is sized to its own widest row with the same
 * padding on every side, rather than to a number picked once and left behind
 * by the next label someone adds. */
typedef struct {
	const char *label;
	const char *value;  /* the right column, or NULL for a row that is only a label */
	bool live;          /* false: a placeholder, drawn quiet and doing nothing */
} menu_row;

#define MENU_RADIUS 20
/* genboot.py's CYAN, the boot animation's play triangle. */
#define MENU_ACCENT 0x3DD6FFu

/* The unit every menu measurement is in. Row height, padding and the gap
 * between the two columns are all cut from it, so the whole panel scales with
 * the type rather than with a set of numbers that have to be retuned together. */
static int menu_row_h(void) { return ui_font_line(UI_F_MENU) * 3 / 2; }

/* `fixed_w` is the content width to use, or 0 to size to these rows. The shelf
 * menus pass a width measured across both of them so the panel never resizes;
 * the in-game menu has no values to cycle and sizes to itself. */
/* `accent` is the panel's border and heading rule. The shelf's own menu passes
 * MENU_ACCENT because that menu is PlayOS, not whichever card is under the
 * cursor; a menu that belongs to a system passes that system's color. */
static void menu_draw(app *a, const char *heading, const menu_row *rows, int n,
                      int sel, int fixed_w, unsigned accent)
{
	TTF_Font *fm = ui_font(UI_F_MENU), *fh = ui_font(UI_F_LABEL);
	int row_h = menu_row_h();
	int pad = row_h * 3 / 4;
	int gap = row_h;                 /* between the label and value columns */
	int text_h = fm ? TTF_FontHeight(fm) : row_h;
	/* Center the ink, not the em box. The box reserves a descender's depth
	 * below the baseline that labels like "Wi-Fi" and "Bluetooth" never use,
	 * so centering the box leaves the visible line riding high in its row and
	 * reads as a highlight sitting too low. Descent is negative, so half of it
	 * subtracted moves the line down onto the middle of the plate. */
	int ink_off = fm ? -TTF_FontDescent(fm) / 2 : 0;
	/* The heading band runs from the panel's top edge down to the rule, and
	 * the heading is centered inside it rather than hung a fixed distance from
	 * the top - otherwise retuning the heading's size moves it off center,
	 * which is exactly what happened when it grew. `content_off` is the panel
	 * top to the first row, so a panel with no heading just pads instead. */
	int line_head   = ui_font_line(UI_F_LABEL);
	int head_h      = heading ? line_head + pad : 0;
	int content_off = heading ? head_h + pad / 2 : pad;
	bool two_col = false;
	int content_w = 0, i, k;
	SDL_Rect panel;
	int cx, content_x, content_y;
	/* The list can outgrow the screen from either end - the type scale turns
	 * up, and this menu is meant to gain rows - so the panel is capped to the
	 * screen and the rows window around the selection when they do not all
	 * fit. A menu that runs off the top is worse than one that scrolls. */
	const int margin = 24;
	int vis = n, first = 0;

	for (i = 0; i < n; i++) if (rows[i].value) two_col = true;
	for (i = 0; i < n; i++) {
		int w = ui_text_width(fm, rows[i].label);
		if (two_col && rows[i].value) w += gap + ui_text_width(fm, rows[i].value);
		if (w > content_w) content_w = w;
	}
	if (heading) {
		int w = ui_text_width(fh, heading);
		if (w > content_w) content_w = w;
	}
	if (fixed_w > 0) content_w = fixed_w;
	if (content_w > PLAYOS_SCREEN_W - margin * 2 - pad * 2)
		content_w = PLAYOS_SCREEN_W - margin * 2 - pad * 2;

	if (content_off + n * row_h + pad > PLAYOS_SCREEN_H - margin * 2) {
		vis = (PLAYOS_SCREEN_H - margin * 2 - content_off - pad) / row_h;
		if (vis < 1) vis = 1;
		if (vis > n) vis = n;
		first = sel - vis / 2;
		if (first < 0) first = 0;
		if (first > n - vis) first = n - vis;
	}

	panel.w = content_w + pad * 2;
	panel.h = content_off + vis * row_h + pad;
	panel.x = (PLAYOS_SCREEN_W - panel.w) / 2;
	panel.y = (PLAYOS_SCREEN_H - panel.h) / 2;
	cx = panel.x + panel.w / 2;
	content_x = panel.x + pad;
	content_y = panel.y + content_off;

	/* PlayOS's own color, not the focused system's. The menu belongs to the
	 * launcher rather than to whatever card happens to be under the cursor,
	 * and it is the same cyan as the boot animation's play triangle, so the
	 * first thing the device draws and the shelf's own chrome agree. Sampled
	 * from a rendered frame at 0x3BD6FF; the constant in tools/genboot.py is
	 * 0x3DD6FF and the difference is video compression. */
	ui_glow(a->r, &panel, accent, 60, 1.5f);
	ui_panel(a->r, &panel, MENU_RADIUS, accent);

	if (heading) {
		/* Centered in the band by its ink, on the same reasoning as the rows:
		 * the em box carries descender depth that "PlayOS" and "NES" mostly do
		 * not use. */
		int head_box = fh ? TTF_FontHeight(fh) : line_head;
		int hy = panel.y + (head_h - head_box) / 2
		         + (fh ? -TTF_FontDescent(fh) / 2 : 0);

		ui_text(a->r, fh, heading, cx, hy, 0, UI_TEXT_SOFT);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, (Uint8)(accent >> 16),
		                       (Uint8)(accent >> 8), (Uint8)accent, 70);
		SDL_RenderFillRect(a->r, &(SDL_Rect){ content_x, panel.y + head_h,
		                                      content_w, 2 });
	}

	for (k = 0; k < vis; k++) {
		int y = content_y + k * row_h;
		int ty = y + (row_h - text_h) / 2 + ink_off;
		SDL_Color lc, vc;

		i = first + k;
		if (i == sel) {
			/* A soft white plate, not the system's color. The accent already
			 * frames the panel; using it again for the cursor made the two
			 * compete, and on a dark red system the plate read as a stain on
			 * the row rather than a highlight under it. White is neutral
			 * against all nine accents.
			 *
			 * A flat plate, with no radial glow under it. The glow was
			 * brightest at the row's midpoint and fell off toward both ends,
			 * which put a soft blob behind the middle of every highlighted
			 * row and read as a smudge rather than as a selection. */
			SDL_Rect plate = { panel.x + pad / 2, y, panel.w - pad, row_h };
			ui_round_rect(a->r, &plate, row_h / 4,
			              (SDL_Color){ 255, 255, 255, 34 });
		}

		/* A placeholder row still highlights - it is a real place on the list -
		 * and stays one step quieter than a working row, which is the whole
		 * signal that it does nothing yet. One step, though, not two: most of
		 * this list is placeholders, and ranking them against an unselected
		 * working row as well left the entire menu reading as greyed out. */
		if (rows[i].live) lc = i == sel ? UI_TEXT      : UI_TEXT_SOFT;
		else              lc = i == sel ? UI_TEXT_SOFT : UI_TEXT_DIM;
		vc = rows[i].live && i == sel
		     ? (SDL_Color){ (Uint8)(a->tint >> 16), (Uint8)(a->tint >> 8),
		                    (Uint8)a->tint, 255 }
		     : UI_TEXT_DIM;

		if (two_col) {
			ui_text(a->r, fm, rows[i].label, content_x, ty, -1, lc);
			if (rows[i].value)
				ui_text(a->r, fm, rows[i].value, content_x + content_w, ty, 1, vc);
		} else {
			ui_text(a->r, fm, rows[i].label, cx, ty, 0, lc);
		}
	}

	/* Three dim dots where the list carries on, in the same vocabulary the
	 * slot carousel's rail uses. Hung just off the rows rather than centered in
	 * the padding: with a heading above, the padding is already spoken for by
	 * the separator, and the indicator belongs to the list in any case. */
	if (vis < n) {
		const int dw = 8, dsz = 3, off = 8;
		int x0 = cx - dw;
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 138, 143, 163, 200);
		for (k = 0; k < 3; k++) {
			if (first > 0)
				SDL_RenderFillRect(a->r, &(SDL_Rect){ x0 + k * dw,
				                   content_y - off - dsz, dsz, dsz });
			if (first + vis < n)
				SDL_RenderFillRect(a->r, &(SDL_Rect){ x0 + k * dw,
				                   content_y + vis * row_h + off, dsz, dsz });
		}
	}
}

/* MENU has two menus behind it, chosen by where it was pressed.
 *
 * From the systems row it is about the firmware. From inside a system it is
 * about THAT system, because a menu that repeated the firmware's settings
 * while a shelf of NES games sat behind it would be answering a question
 * nobody asked. Almost every row in both is a placeholder: the lists are here
 * to hold the shape of what PlayOS grows into, and a row that is drawn but
 * does nothing states that more honestly than an empty menu does. */
typedef enum {
	PM_WIFI, PM_BT, PM_ACHIEVEMENTS, PM_SCRAPE,
	PM_TEXT, PM_SLEEP, PM_ABOUT, PM_POWER, PM_ROWS
} pm_row;

/* The system menu. Games and Core carry real values rather than invented ones,
 * because a placeholder that lies about the machine it is describing is worse
 * than no row. The rest name capabilities that already exist on the emulator
 * side - Diatom has per-system display modes and core-supplied button labels -
 * so these are hooks waiting to be wired, not wishes. */
typedef enum {
	SM_GAMES, SM_CORE, SM_SORT, SM_SHOW,
	SM_DISPLAY, SM_BUTTONS, SM_BOXART, SM_RESCAN, SM_ROWS
} sm_row;

#define MENU_MAX_ROWS 12
typedef struct { char a[24], b[CFG_STR]; } menu_bufs;

/* Build whichever menu the current screen calls for. Returns the row count, so
 * the input loop never needs to know which of the two it is driving. */
static int menu_build(app *a, screen_id screen, int sys,
                      menu_row *out, menu_bufs *b, const char **heading)
{
	if (screen == SCREEN_GAMES) {
		const system_cfg *s = &a->sys.systems[sys];

		snprintf(b->a, sizeof b->a, "%d", a->view[sys].list.count);
		snprintf(b->b, sizeof b->b, "%s", s->core);
		*heading = s->name;
		out[SM_GAMES]   = (menu_row){ "Games",          b->a,            false };
		out[SM_CORE]    = (menu_row){ "Core",           b->b,            false };
		out[SM_SORT]    = (menu_row){ "Sort by",        "Name",          false };
		out[SM_SHOW]    = (menu_row){ "Show",           "All games",     false };
		out[SM_DISPLAY] = (menu_row){ "Display mode",
		                              DMODES[a->view[sys].dmode].label, true };
		out[SM_BUTTONS] = (menu_row){ "Button mapping", NULL,            false };
		out[SM_BOXART]  = (menu_row){ "Box art",        "not yet",       false };
		out[SM_RESCAN]  = (menu_row){ "Rescan folder",  NULL,            false };
		return SM_ROWS;
	}

	snprintf(b->a, sizeof b->a, "%d%%", (int)(ui_get_font_scale() * 100.0f + 0.5f));
	*heading = "PlayOS";
	out[PM_WIFI]         = (menu_row){ "Wi-Fi",             "not yet", false };
	out[PM_BT]           = (menu_row){ "Bluetooth",         "not yet", false };
	out[PM_ACHIEVEMENTS] = (menu_row){ "RetroAchievements", "not yet", false };
	out[PM_SCRAPE]       = (menu_row){ "Box art scraping",  "not yet", false };
	out[PM_TEXT]         = (menu_row){ "Text size",         b->a,      false };
	out[PM_SLEEP]        = (menu_row){ "Sleep timer",       "not yet", false };
	out[PM_ABOUT]        = (menu_row){ "About PlayOS",      NULL,      false };
	out[PM_POWER]        = (menu_row){ "Power off",         NULL,      true  };
	return PM_ROWS;
}

/* The widest row of one built menu. */
static int menu_measure(const menu_row *rows, int n, const char *heading)
{
	TTF_Font *fm = ui_font(UI_F_MENU), *fh = ui_font(UI_F_LABEL);
	int gap = menu_row_h(), w = 0, i;
	bool two_col = false;

	for (i = 0; i < n; i++) if (rows[i].value) two_col = true;
	for (i = 0; i < n; i++) {
		int rw = ui_text_width(fm, rows[i].label);
		if (two_col && rows[i].value) rw += gap + ui_text_width(fm, rows[i].value);
		if (rw > w) w = rw;
	}
	if (heading) {
		int hw = ui_text_width(fh, heading);
		if (hw > w) w = hw;
	}
	return w;
}

/* One width for both shelf menus, every system, and every value their rows can
 * cycle to. The panel is a frame the lists sit inside rather than something
 * that resizes to whatever is selected: without this, cycling display mode from
 * "Fill" to "Integer tall" widens the slab under the cursor, and walking from
 * the systems row into a system resizes it again.
 *
 * Measured across all of that rather than picked, so a longer label, another
 * system or a larger font scale widens the frame instead of overflowing it.
 * Cached because it is a few hundred text measurements and none of its inputs
 * change while a menu is open. */
static int menu_shelf_width(app *a)
{
	menu_row rows[MENU_MAX_ROWS];
	menu_bufs bufs;
	const char *heading;
	int w, n, i, k;

	if (a->menu_w) return a->menu_w;

	n = menu_build(a, SCREEN_SYSTEMS, a->sys_cursor, rows, &bufs, &heading);
	w = menu_measure(rows, n, heading);

	for (i = 0; i < a->sys.count; i++) {
		n = menu_build(a, SCREEN_GAMES, i, rows, &bufs, &heading);
		for (k = 0; k < DMODE_COUNT; k++) {
			int mw;
			rows[SM_DISPLAY].value = DMODES[k].label;
			mw = menu_measure(rows, n, heading);
			if (mw > w) w = mw;
		}
	}
	a->menu_w = w;
	return w;
}

static void playos_menu_draw(app *a, int sel)
{
	menu_row rows[MENU_MAX_ROWS];
	menu_bufs bufs;
	const char *heading;
	int n = menu_build(a, a->screen, a->sys_cursor, rows, &bufs, &heading);

	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
	/* The shelf's menu is PlayOS's own on the systems screen and a system's on
	 * a game list, which is where it gains rows that belong to that system. */
	menu_draw(a, heading, rows, n, sel, menu_shelf_width(a),
	          a->screen == SCREEN_SYSTEMS ? MENU_ACCENT : a->tint);
}

static void playos_menu(app *a)
{
	menu_row rows[MENU_MAX_ROWS];
	menu_bufs bufs;
	const char *heading;
	/* The screen cannot change while the menu is open, so the row count is
	 * settled here and the loop below is the same loop for either menu. */
	int n = menu_build(a, a->screen, a->sys_cursor, rows, &bufs, &heading);
	int sel = 0, done = 0;

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit && a->running) {
		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; return; }

		/* Rebuilt every frame: a row that can be changed from inside the menu
		 * has to show what it was changed to. */
		n = menu_build(a, a->screen, a->sys_cursor, rows, &bufs, &heading);

		if (in_repeat(&a->in, IN_UP))   sel = (sel + n - 1) % n;
		if (in_repeat(&a->in, IN_DOWN)) sel = (sel + 1) % n;

		/* Left and right cycle the value on a row that has one. Display mode
		 * is the only such row so far; it is saved the moment it changes,
		 * because there is no confirm step to hang the write off. */
		if (a->screen == SCREEN_GAMES && sel == SM_DISPLAY) {
			int d = in_repeat(&a->in, IN_RIGHT) ? 1
			      : in_repeat(&a->in, IN_LEFT)  ? -1 : 0;
			if (d) {
				sysview *v = &a->view[a->sys_cursor];
				v->dmode = (v->dmode + d + DMODE_COUNT) % DMODE_COUNT;
				display_save(a);
			}
		}
		/* Volume and brightness keep working here, as they do everywhere. */
		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		if (a->in.pressed[IN_BACK] || a->in.pressed[IN_MENU]) done = 1;
		if (a->in.pressed[IN_POWER]) { power_off(a); return; }
		/* The only row either menu acts on, and it exists in one of them - the
		 * screen has to be checked as well as the index, or row 7 of the
		 * system menu would power the device off. */
		if (a->in.pressed[IN_ACCEPT] && a->screen == SCREEN_SYSTEMS &&
		    sel == PM_POWER) { power_off(a); return; }

		tick_tint(a);
		draw_shelf(a);
		playos_menu_draw(a, sel);
		plat_draw_osd(a->r);
		SDL_RenderPresent(a->r);
		SDL_Delay(8);
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* ---------- launching ----------------------------------------------------- */

static char env_buf[10][CFG_STR * 2];
static const char *child_env[24];

static void build_child_env(void)
{
	static const char *fixed[] = {
		"PLATFORM=tg5040", "DEVICE=brick",
		"SDCARD_PATH=/mnt/SDCARD",
		"BIOS_PATH=/mnt/SDCARD/Bios",
		"CHEATS_PATH=/mnt/SDCARD/Cheats",
		"SAVES_PATH=/mnt/SDCARD/Saves",
	};
	size_t i;
	int n = 0;
	for (i = 0; i < sizeof fixed / sizeof *fixed; i++) child_env[n++] = fixed[i];
	snprintf(env_buf[0], sizeof env_buf[0], "ROMS_PATH=%s", P_ROMS);
	snprintf(env_buf[1], sizeof env_buf[1], "SYSTEM_PATH=%s", P_ROOT);
	snprintf(env_buf[2], sizeof env_buf[2], "CORES_PATH=%s/cores", P_ROOT);
	snprintf(env_buf[3], sizeof env_buf[3], "USERDATA_PATH=%s", P_USERDATA);
	snprintf(env_buf[4], sizeof env_buf[4], "SHARED_USERDATA_PATH=%s", P_SHARED);
	snprintf(env_buf[5], sizeof env_buf[5], "LOGS_PATH=%s/logs", P_USERDATA);
	snprintf(env_buf[6], sizeof env_buf[6], "HOME=%s", P_USERDATA);
	snprintf(env_buf[7], sizeof env_buf[7], "LD_LIBRARY_PATH=%s/lib:/usr/trimui/lib", P_ROOT);
	for (i = 0; i <= 7; i++) child_env[n++] = env_buf[i];
	child_env[n] = NULL;
}

/* The in-game menu - the launcher's, over Diatom's pause.
 *
 * MENU in a game makes Diatom write a preview of the frame, stop presenting,
 * and say PAUSED. From that word the display is ours: the menu is that frame
 * dimmed, with the rows a player expects - Continue, Save, Load, Reset, Quit
 * - drawn with the launcher's own font and glow. The menu is the launcher's,
 * not something patched into the emulator.
 *
 * Every row acts through one protocol line. Save and Load use the same autosave
 * paths the launch handed over, so the autosave funnel stays one thing. */
/* Display sits with the things you do to the game rather than the things you do
 * to a save, because it is the one row whose effect you judge by looking at the
 * game behind the menu. */
typedef enum {
	GM_CONTINUE, GM_SAVE, GM_LOAD, GM_DISPLAY, GM_RESET, GM_QUIT, GM_ROWS
} gm_row;

/* The paused frame, drawn where the game actually is.
 *
 * Diatom reports its rect with every DISPLAY message (its ADR-0022 put it there
 * so a launcher need not recompute it from geometry it does not have), and the
 * preview it writes is the CORE'S frame - 256x224 for an NES, no display mode
 * applied. Stretching that to the panel, which is what this used to do, showed
 * a game that looked nothing like the one paused underneath at any mode that
 * does not fill the screen, and made the picture jump size the moment MENU was
 * pressed.
 *
 * Falls back to filling the panel when Diatom has not said - the standalone
 * path, and the first moments of a launch. */
static void draw_paused_frame(app *a, SDL_Texture *bg)
{
	SDL_Rect r;

	if (!bg) return;
	if (plat_resident_rect(&r)) SDL_RenderCopy(a->r, bg, NULL, &r);
	else                        SDL_RenderCopy(a->r, bg, NULL, NULL);
}

/* Copy the paused frame's preview beside a manual save, so the slot strip can
 * show what is inside each slot. The pause preview IS the frame the save
 * serialises - Diatom wrote it on the way into the menu - so a straight copy
 * is the truthful thumbnail, no protocol round trip needed. */
static void copy_file(const char *from, const char *to)
{
	FILE *a = fopen(from, "rb"), *b = to ? fopen(to, "wb") : NULL;
	char buf[16384];
	size_t n;

	if (a && b) while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
	if (a) fclose(a);
	if (b) fclose(b);
}

/* The slot carousel: Auto plus the manual slots for Load, the manual slots for
 * Save. One slot at a
 * time, large - the paused frame at a size you can actually read - with the
 * save's own timestamp under it and a dot rail for where you are. Left and
 * right cycle; Load skips slots with nothing behind them, Save cannot aim at
 * Auto, which belongs to the exit funnel alone. */
typedef struct {
	SDL_Texture *thumb[GM_SLOTS + 1];   /* [0]=Auto, [1..GM_SLOTS] */
	int have[GM_SLOTS + 1];
	char when[GM_SLOTS + 1][40];
	/* Every slot of one game holds the same machine's frame, so one aspect
	 * describes them all and the picture can be framed exactly rather than
	 * dropped into a fixed box with bars down its sides. 4:3 until a slot
	 * with a picture in it says otherwise. */
	float aspect;
	int saving;
} slot_view;

/* Drawing only, so one frame of it can be rendered by --shot without a game
 * running or a device in hand. */
static void slot_draw(app *a, const slot_view *sv, int sel)
{
	/* `area` is the layout slot the picture is fitted into; nothing is ever
	 * drawn to it. The border is the picture's own edge in the system's
	 * color - a 240x160 GBA frame and a 256x224 NES frame are different
	 * shapes, and neither should be padded out into the same rectangle. */
	const SDL_Rect area = { (PLAYOS_SCREEN_W - 700) / 2, 129, 700, 451 };
	const int bw = 12;
	SDL_Rect img = area, frame;
	char slotname[16];
	int line_menu = ui_font_line(UI_F_MENU), line_meta = ui_font_line(UI_F_META);
	int i;

	/* The heading sits close to the top edge so the picture gets the middle of
	 * the screen. Everything below hangs off the IMAGE rather than off `area`,
	 * so a frame shorter than the layout slot pulls its own caption up with it
	 * instead of leaving a gap.
	 *
	 * The margins are mirrored: the heading's top sits as far from the top of
	 * the screen as the marker rail's bottom sits from the bottom of it, and
	 * the heading is centred in the gap above the frame. That fixes every
	 * number here to one another rather than to taste, so changing the image
	 * size moves the rest to match instead of drifting into something. */
	ui_text(a->r, ui_font(UI_F_LABEL), sv->saving ? "Save to" : "Load from",
	        PLAYOS_SCREEN_W / 2, 39, 0, UI_TEXT_DIM);

	if ((float)area.w / sv->aspect <= (float)area.h) {
		img.w = area.w;
		img.h = (int)(area.w / sv->aspect + 0.5f);
	} else {
		img.h = area.h;
		img.w = (int)(area.h * sv->aspect + 0.5f);
	}
	img.x = area.x + (area.w - img.w) / 2;
	img.y = area.y + (area.h - img.h) / 2;
	frame = (SDL_Rect){ img.x - bw, img.y - bw, img.w + bw * 2, img.h + bw * 2 };

	ui_glow(a->r, &img, a->tint, 85, 1.35f);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	/* Opaque, matching the menu panel's border. At alpha 150 the frame let the
	 * background through and read as a darker accent than the system's own. The
	 * thumbnail itself is drawn at full brightness below - it never was
	 * darkened, only the frame around it.
	 *
	 * Rounded outside, square inside: the corner radius belongs to the chrome,
	 * and rounding the image would mean clipping the game's own pixels. */
	ui_round_rect(a->r, &frame, bw, (SDL_Color){
		(Uint8)(a->tint >> 16), (Uint8)(a->tint >> 8), (Uint8)a->tint, 255 });
	if (sv->thumb[sel]) {
		SDL_SetTextureColorMod(sv->thumb[sel], 255, 255, 255);
		SDL_RenderCopy(a->r, sv->thumb[sel], NULL, &img);
	} else {
		/* An empty slot is the same frame with nothing in it, so the strip
		 * does not change shape as you cycle past one. */
		SDL_SetRenderDrawColor(a->r, 12, 13, 18, 238);
		SDL_RenderFillRect(a->r, &img);
		ui_text(a->r, ui_font(UI_F_MENU), "Empty", img.x + img.w / 2,
		        img.y + (img.h - line_menu) / 2, 0, UI_TEXT_DIM);
	}

	if (sel == 0) snprintf(slotname, sizeof slotname, "Auto");
	else          snprintf(slotname, sizeof slotname, "Slot %d", sel);
	ui_text(a->r, ui_font(UI_F_MENU), slotname,
	        PLAYOS_SCREEN_W / 2, img.y + img.h + 36, 0, UI_TEXT);
	ui_text(a->r, ui_font(UI_F_META),
	        sv->have[sel] ? sv->when[sel] : (sv->saving ? "\xE2\x80\x94" : ""),
	        PLAYOS_SCREEN_W / 2, img.y + img.h + 42 + line_menu, 0, UI_TEXT_DIM);

	/* The dot rail: where you are among the slots, without showing every
	 * picture.
	 * A hollow-dim dot is a slot you cannot land on. */
	{
		int dots = GM_SLOTS + 1, dw = 36;
		int x0 = (PLAYOS_SCREEN_W - dots * dw) / 2 + dw / 2;
		int y  = img.y + img.h + 62 + line_menu + line_meta;

		/* One size for every marker. Sizing the selected one larger meant the
		 * rail changed shape as you cycled, and color already says which slot
		 * you are on - two signals for one fact, one of them moving. */
		for (i = 0; i < dots; i++) {
			int can = sv->saving ? i >= 1 : sv->have[i];
			int r2  = 12;
			SDL_Rect d = { x0 + i * dw - r2, y - r2, r2 * 2, r2 * 2 };
			SDL_Color c = i == sel
				? (SDL_Color){ (Uint8)(a->tint >> 16), (Uint8)(a->tint >> 8),
				               (Uint8)a->tint, 255 }
				: (SDL_Color){ 90, 94, 110, can ? 255 : 90 };
			ui_round_rect(a->r, &d, r2 / 2, c);
		}
	}
}

/* Returns the chosen slot (SLOT_AUTO, or 1..GM_SLOTS) or 0 for backed out. */
static int slot_strip(app *a, SDL_Texture *bg, int saving)
{
	slot_view sv = { .aspect = 4.0f / 3.0f, .saving = saving };
	sysview *v = &a->view[a->sys_cursor];
	game_entry *g = &v->list.items[v->cursor];
	int i, sel = -1, chosen = 0, done = 0;
	char pth[LIB_PATH * 2];

	for (i = 0; i <= GM_SLOTS; i++) {
		int slot = i == 0 ? SLOT_AUTO : i;
		struct stat st;

		slot_state_path(a, a->sys_cursor, g, slot, pth, sizeof pth);
		sv.have[i] = (stat(pth, &st) == 0 && st.st_size > 0);
		if (sv.have[i]) {
			/* The state's own mtime: when this moment was captured. The
			 * device clock is only as good as the device clock, and showing
			 * what the filesystem says beats pretending to know better. */
			struct tm *tm = localtime(&st.st_mtime);
			if (tm) {
				/* "Aug 23 9:21:05 AM". Built from the fields rather than with
				 * strftime's "%-I", which drops the leading zero but is a GNU
				 * extension and does nothing on the BSD libc the host build
				 * links against - it would have read right on the device and
				 * wrong in every screenshot. The month still comes from
				 * strftime so it stays whatever the locale calls it. */
				char mon[8];
				int h12 = tm->tm_hour % 12;
				strftime(mon, sizeof mon, "%b", tm);
				if (!h12) h12 = 12;
				snprintf(sv.when[i], sizeof sv.when[i], "%s %d %d:%02d:%02d %s",
				         mon, tm->tm_mday, h12, tm->tm_min, tm->tm_sec,
				         tm->tm_hour < 12 ? "AM" : "PM");
			}

			slot_preview_path(a, a->sys_cursor, g, slot, pth, sizeof pth);
			SDL_Surface *sf = IMG_Load(pth);
			if (sf) {
				sv.thumb[i] = SDL_CreateTextureFromSurface(a->r, sf);
				SDL_FreeSurface(sf);
			}
		}
		/* First selectable: saving starts on slot 1; loading starts on the
		 * newest thing there is to load, which is almost always Auto. */
		if (sel < 0 && (saving ? i >= 1 : sv.have[i])) sel = i;
	}
	for (i = 0; i <= GM_SLOTS; i++) {
		int tw = 0, th = 0;
		if (!sv.thumb[i]) continue;
		SDL_QueryTexture(sv.thumb[i], NULL, NULL, &tw, &th);
		if (tw > 0 && th > 0) { sv.aspect = (float)tw / (float)th; break; }
	}
	if (sel < 0) sel = saving ? 1 : -1;
	if (sel < 0) done = -1;                     /* nothing to load at all */

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit) {
		plat_input_poll(&a->in);

		if (in_repeat(&a->in, IN_LEFT) || in_repeat(&a->in, IN_RIGHT)) {
			int dir = in_repeat(&a->in, IN_RIGHT) ? 1 : -1, next = sel;
			do {
				next = (next + dir + GM_SLOTS + 1) % (GM_SLOTS + 1);
			} while ((saving ? next == 0 : !sv.have[next]) && next != sel);
			sel = next;
		}
		if (a->in.pressed[IN_BACK] || a->in.pressed[IN_MENU]) done = -1;
		if (a->in.pressed[IN_ACCEPT]) { chosen = sel == 0 ? 9 : sel; done = 1; }

		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
		SDL_RenderClear(a->r);
		draw_paused_frame(a, bg);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 185);
		SDL_RenderFillRect(a->r, NULL);

		slot_draw(a, &sv, sel);
		SDL_RenderPresent(a->r);
		SDL_Delay(8);
	}

	for (i = 0; i <= GM_SLOTS; i++)
		if (sv.thumb[i]) SDL_DestroyTexture(sv.thumb[i]);
	return done == 1 ? chosen : 0;
}

/* Change the mode, keep it, tell the running game, and take back the rect so
 * the backdrop behind the menu redraws where the game is about to be. */
static void gm_cycle_display(app *a, int d)
{
	sysview *v = &a->view[a->sys_cursor];

	v->dmode = (v->dmode + d + DMODE_COUNT) % DMODE_COUNT;
	display_save(a);
	plat_resident_line("SETDISPLAY\tmode=%s", DMODES[v->dmode].name);
	plat_resident_sync_rect(150);
}

/* The in-game rows, carrying the display mode's current label. */
static void gm_build(app *a, menu_row *out)
{
	static const char *label[GM_ROWS] = {
		"Continue", "Save", "Load", "Display", "Reset", "Quit"
	};
	int i;

	for (i = 0; i < GM_ROWS; i++) out[i] = (menu_row){ label[i], NULL, true };
	out[GM_DISPLAY].value = DMODES[a->view[a->sys_cursor].dmode].label;
}

/* Measured across every mode label, so cycling the row does not resize the
 * panel under the cursor - the same reason the shelf menus have a fixed width. */
static int gm_width(app *a)
{
	menu_row rows[GM_ROWS];
	int w = 0, k;

	gm_build(a, rows);
	for (k = 0; k < DMODE_COUNT; k++) {
		int mw;
		rows[GM_DISPLAY].value = DMODES[k].label;
		mw = menu_measure(rows, GM_ROWS, NULL);
		if (mw > w) w = mw;
	}
	return w;
}

static void game_menu(app *a)
{
	SDL_Texture *bg = NULL;
	const char *pv = plat_resident_last_preview();
	int sel = 0, done = 0, resume = 0;
	int width = gm_width(a);

	if (pv && *pv) {
		SDL_Surface *sf = IMG_Load(pv);
		if (sf) { bg = SDL_CreateTextureFromSurface(a->r, sf); SDL_FreeSurface(sf); }
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit) {
		plat_input_poll(&a->in);

		if (in_repeat(&a->in, IN_UP))   sel = (sel + GM_ROWS - 1) % GM_ROWS;
		if (in_repeat(&a->in, IN_DOWN)) sel = (sel + 1) % GM_ROWS;

		/* Applied to the running game at once, not on resume: the whole point
		 * of this row being here rather than on the shelf is judging the mode
		 * against the game it is being applied to. Diatom takes SETDISPLAY
		 * while paused (its ADR-0020), answers with the new rect, and the
		 * backdrop behind this menu redraws into it. */
		if (sel == GM_DISPLAY) {
			int d = in_repeat(&a->in, IN_RIGHT) ? 1
			      : in_repeat(&a->in, IN_LEFT)  ? -1 : 0;
			if (d) gm_cycle_display(a, d);
		}
		/* MENU again, or B: back to the game, same as Continue. */
		if (a->in.pressed[IN_MENU] || a->in.pressed[IN_BACK]) {
			resume = 1;
			done = 1;
		} else if (a->in.pressed[IN_ACCEPT]) {
			switch ((gm_row)sel) {
			case GM_CONTINUE:
				resume = 1;
				done = 1;
				break;
			case GM_SAVE:
			case GM_LOAD: {
				int slot = slot_strip(a, bg, sel == GM_SAVE);
				if (slot) {
					sysview *sv = &a->view[a->sys_cursor];
					char sp[LIB_PATH * 2], pp[LIB_PATH * 2];

					slot_state_path(a, a->sys_cursor,
					                &sv->list.items[sv->cursor], slot,
					                sp, sizeof sp);
					if (sel == GM_SAVE) {
						plat_resident_line("SAVE\tpath=%s", sp);
						/* The paused frame is what the save holds, and
						 * Diatom already wrote it as the pause preview:
						 * copy it beside the state so the strip can show
						 * what is inside the slot. */
						slot_preview_path(a, a->sys_cursor,
						                  &sv->list.items[sv->cursor], slot,
						                  pp, sizeof pp);
						copy_file(plat_resident_last_preview(), pp);
					} else {
						plat_resident_line("LOAD\tpath=%s", sp);
					}
					resume = 1;
					done = 1;
				}
				/* Backed out: fall through to the menu, still paused. */
				plat_input_flush();
				memset(&a->in, 0, sizeof a->in);
				break;
			}
			/* A cycles it forward as well as left and right. Every other
			 * row in this menu is something A does, so a row that only
			 * answered to left and right was a row that looked broken. */
			case GM_DISPLAY:
				gm_cycle_display(a, +1);
				break;
			case GM_RESET:
				plat_resident_line("RESET");
				resume = 1;
				done = 1;
				break;
			case GM_QUIT:
				/* The wait loop carries on until EXIT arrives; quitting is
				 * asking, not tearing down. */
				plat_resident_line("STOP");
				done = 1;
				break;
			default: break;
			}
		}
		if (a->in.pressed[IN_POWER]) {
			plat_resident_line("STOP");
			done = 1;
		}

		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
		SDL_RenderClear(a->r);
		draw_paused_frame(a, bg);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);

		{
			menu_row rows[GM_ROWS];
			gm_build(a, rows);
			menu_draw(a, NULL, rows, GM_ROWS, sel, width, a->tint);
		}
		SDL_RenderPresent(a->r);
		SDL_Delay(8);
	}

	if (bg) SDL_DestroyTexture(bg);

	/* Hand the pages back the way Diatom expects to find them.
	 *
	 * Diatom does not repaint the area outside its picture every frame - its
	 * pages start opaque black and it writes only the rect - which is sound
	 * while it owns the framebuffer and false the moment this process has
	 * drawn a full-screen menu into the same pages. At any display mode that
	 * does not fill the panel, resuming showed the game correctly sized with
	 * this menu still surrounding it, and flickering: Diatom cycles three
	 * pages and only the two this process presents into had been dirtied, so
	 * the border alternated menu, menu, black at the refresh rate.
	 *
	 * Twice because this process alternates two pages and one present only
	 * clears the one it lands on. Before RESUME and never after: afterwards
	 * Diatom is drawing, and this would be a second presenter. */
	if (resume) {
		present_black(a);
		present_black(a);
		plat_resident_line("RESUME");
	}

	/* Nothing presents from here: the next frame on screen is the game's. */
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

/* Bring the resident emulator back.
 *
 * With one process per game a core that segfaults takes down that game and
 * nothing else. With a resident emulator it takes down the emulator, and every
 * launch for the rest of the session would fall back to the slow path -- until
 * the launcher itself exits and launch.sh starts a new one. So the launcher
 * starts one too, once the fallback game has given the display back. Never
 * while a game is running: two GL contexts and a launcher is one more than
 * this device should be asked for. */
static void respawn_resident(app *a)
{
	char elf[CFG_STR * 2], save[CFG_STR * 2], bios[CFG_STR * 2];
	char *argv[8];
	(void)a;

	if (plat_resident_ready()) return;

	/* The resident is Diatom, and it preloads nothing - a core is mapped the
	 * first time a game needs it and kept (its ADR-0006), so there is no
	 * core list to hand over and nothing here changes when a system is
	 * added. */
	snprintf(elf, sizeof elf, "%s/diatom", P_ROOT);
	if (access(elf, X_OK) != 0) return;
	snprintf(save, sizeof save, "%s/Saves", P_CARD);
	snprintf(bios, sizeof bios, "%s/Bios", P_CARD);
	argv[0] = elf;
	argv[1] = (char *)"--socket";
	argv[2] = (char *)plat_resident_socket();
	argv[3] = (char *)"--save";   argv[4] = save;
	argv[5] = (char *)"--system"; argv[6] = bios;
	argv[7] = NULL;
	fprintf(stderr, "resident emulator is gone, starting diatom\n");
	plat_spawn_detached(argv, child_env, P_ROOT);
}

static void launch(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	const system_cfg *s = &a->sys.systems[a->sys_cursor];
	char core[CFG_STR * 2], elf[CFG_STR * 2], rom[LIB_PATH * 2];
	char st[LIB_PATH * 2], pv[LIB_PATH * 2];
	char save[CFG_STR * 2], bios[CFG_STR * 2];
	/* 18 fixed entries plus NULL, then two per core option. Sized off the
	 * loader's own cap so the two cannot drift apart: the previous 20 was
	 * already 18 full, and a silent bound check would have dropped every
	 * option rather than failing loudly. */
	char *argv[20 + 2 * 32];
	bool resident = false;
	int n = 0;

	if (v->list.count == 0) return;

	snprintf(core, sizeof core, "%s/cores/%s_libretro.so", P_ROOT, s->core);
	snprintf(elf, sizeof elf, "%s/diatom", P_ROOT);
	snprintf(rom, sizeof rom, "%s/%s/%s", P_ROMS, s->folder, v->list.items[v->cursor].file);
	snprintf(save, sizeof save, "%s/Saves", P_CARD);
	snprintf(bios, sizeof bios, "%s/Bios", P_CARD);

	/* The autosave story, by path rather than by convention: the state and the
	 * preview live beside each other, the launch hands both over, and a game
	 * always comes up where it was left. */
	state_path(a, a->sys_cursor, &v->list.items[v->cursor], st, sizeof st);
	preview_path(a, a->sys_cursor, &v->list.items[v->cursor], pv, sizeof pv);
	persist_dir_ensure(a, a->sys_cursor);

	remember_place(a);

	if (plat_resident_ready()) {
		/* The emulator is already up, holding its context and every core,
		 * so this is ~200ms rather than ~1100. Nothing here is torn
		 * down -- this process keeps its own context through the whole game,
		 * which is why coming back is a frame rather than a second and a
		 * half. The two do overlap for the length of this animation, while
		 * the game loads behind it; after that only the emulator draws,
		 * because this process is blocked - except when the player opens
		 * the in-game menu, which is drawn HERE now, over the frame the
		 * emulator hands us on the way into its pause. */
		if (plat_resident_send(s->tag, core, rom, st, st, pv)) {
			int r;

			/* Straight after RUN and the levels, and for the same reason: the
			 * mode is Diatom's own global and survives from the last game, so
			 * a system that has never been set would otherwise inherit
			 * whatever the previous one chose. Ordered on the same socket, so
			 * it lands before the first frame. */
			plat_resident_line("SETDISPLAY\tmode=%s", DMODES[v->dmode].name);
			/* No launch animation, and it is a display-safety rule, not a
			 * taste call: Diatom presents through fbdev, this process
			 * through GL, and the handoff spike's one invariant is that
			 * they never present concurrently - a 190ms overlap that is
			 * harmless GL-on-GL is the exact case that wedges the display
			 * engine when one side is fbdev. A warm launch is ~15ms, so there is
			 * nothing to animate over anyway; the shelf simply holds until
			 * the game's first frame replaces it. */
			for (;;) {
				r = plat_resident_wait();
				if (r == RES_PAUSED) { game_menu(a); continue; }
				break;
			}
			resident = (r == RES_EXIT);
			/* RES_DEAD means it stopped answering -- it died, or the game
			 * never started. Say so by falling through to the path that
			 * runs it the slow way, rather than fading the shelf back up
			 * as though it had run. */
			if (!resident)
				fprintf(stderr, "resident emulator stopped answering, falling back\n");
		} else {
			fprintf(stderr, "resident emulator did not answer, falling back\n");
		}
	}

	if (!resident) {
		/* One game per process, the old way: the fallback for a resident that
		 * is missing or has died. Diatom standalone IS the one-shot mode -
		 * same binary, no socket - so the fallback stopped being a different
		 * emulator and became the same one held differently. It has to take
		 * the display, so this side tears its own down first. */
		anim_launch(a, 190);
		free_all_textures(a);
		ui_quit();
		plat_input_quit();
		plat_video_quit();

		argv[n++] = elf;
		argv[n++] = (char *)"--core";            argv[n++] = core;
		argv[n++] = (char *)"--rom";             argv[n++] = rom;
		argv[n++] = (char *)"--save";            argv[n++] = save;
		argv[n++] = (char *)"--system";          argv[n++] = bios;
		argv[n++] = (char *)"--display";         argv[n++] = (char *)DMODES[v->dmode].name;
		argv[n++] = (char *)"--load-state";      argv[n++] = st;
		argv[n++] = (char *)"--state-on-exit";   argv[n++] = st;
		argv[n++] = (char *)"--preview-on-exit"; argv[n++] = pv;
		/* Same opinions as the resident path gets over SETOPT, so a game plays
		 * the same whether the resident was up or the fallback ran it. */
		{
			int ci;
			for (ci = 0; ci < plat_coreopt_count(s->tag) && n < (int)(sizeof argv / sizeof argv[0]) - 3; ci++) {
				argv[n++] = (char *)"--core-option";
				argv[n++] = (char *)plat_coreopt(s->tag, ci);
			}
		}
		argv[n] = NULL;
		fprintf(stderr, "diatom exited %d\n", plat_run(argv, child_env, P_ROOT));

		if (!plat_video_init() || !plat_input_init()) { a->running = false; return; }
		a->r = plat_renderer();
		ui_init(a->r, P_FONT);
		a->menu_w = 0;   /* fonts reopened: remeasure the panel */
		prime_sys_window(a);
		prime_window(a, a->sys_cursor);
		/* The display is ours again and no game is running: the safe moment
		 * to put a resident emulator back, so the NEXT launch is fast. */
		respawn_resident(a);
	}

	t_back0 = plat_now_ms();
	present_black(a);

	/* The game has just written a fresh autosave preview; make the card pick
	 * it up rather than showing the one from last time. */
	{
		game_entry *g = &v->list.items[v->cursor];
		g->state_known = 0;
		if (v->tex[v->cursor]) {
			SDL_DestroyTexture(v->tex[v->cursor]);
			v->tex[v->cursor] = NULL;
		}
	}


	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	/* This one exits without power_off(), so it darkens the lights itself. */
	if (access(PLAYOS_POWEROFF_FLAG, F_OK) == 0) {
		plat_leds_off();
		a->running = false;
		return;
	}
	/* Power was pressed during the game. The emulator no longer handles that
	 * key itself, so the press arrived here. */
	if (plat_run_power_pressed()) { power_off(a); return; }

	/* Straight to the shelf, no fade. The card decode inside this render is
	 * the only real cost left on the way back, and a fade laid over the top of
	 * it is time spent easing in a picture the player has already been looking
	 * at all the way up to the moment they quit. render() is right here: it
	 * draws and presents exactly once. */
	render(a);
	fprintf(stderr, "exit: back in %u ms\n", plat_now_ms() - t_back0);
	/* Seventy-nine open/write/close round trips through sysfs, measured at
	 * 35-52ms. Nothing about them is urgent and the panel is what the player
	 * is waiting on, so they happen once the shelf is up rather than while it
	 * is still black. The two paths above that leave without reaching here do
	 * it themselves. */
	plat_leds_off();
}

/* ---------- input --------------------------------------------------------- */

static void enter_system(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	if (v->list.count == 0) return;
	a->screen = SCREEN_GAMES;
	cf_reset(&v->cf, v->cursor);
	prime_window(a, a->sys_cursor);
}

static void update_systems(app *a)
{
	int n = a->sys.count;
	if (in_repeat(&a->in, IN_LEFT))  a->sys_cursor = (a->sys_cursor - 1 + n) % n;
	if (in_repeat(&a->in, IN_RIGHT)) a->sys_cursor = (a->sys_cursor + 1) % n;
	if (a->in.pressed[IN_ACCEPT])    enter_system(a);
	cf_set_cursor(&a->cf_sys, a->sys_cursor, n);
}

/* The first alphanumeric character of a name, upper-cased, or '\0' for a name
 * with none at all - which groups those few together rather than giving each
 * one a group of its own. Leading articles and punctuation are deliberately
 * NOT skipped: lib_scan sorts the shelf with strcasecmp on this same string,
 * so grouping that disagreed with the order the cards are drawn in would make
 * the jump land somewhere that looks arbitrary. "The Legend of Zelda" files
 * under T here because it sits under T on the shelf. */
static char shelf_initial(const char *s)
{
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;
		if (c >= 'a' && c <= 'z') return (char)(c - 32);
		if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return (char)c;
	}
	return '\0';
}

/* The first game sharing idx's initial, walking backwards from it. */
static int shelf_group_start(sysview *v, int idx)
{
	int n = v->list.count, i = idx;
	char c = shelf_initial(v->list.items[idx].title);

	for (;;) {
		int p = (i - 1 + n) % n;
		if (p == idx) return idx;             /* one initial, the whole shelf */
		if (shelf_initial(v->list.items[p].title) != c) break;
		i = p;
	}
	return i;
}

/* Up and down cross the shelf one initial at a time, which on a long library
 * is the difference between forty presses and two. Down lands on the first
 * game of the next initial. Up lands on the first game of THIS one, and moves
 * to the previous initial only when the cursor is already there - so from any
 * group's first game, up and down are exact inverses. */
static int shelf_letter_jump(sysview *v, int dir)
{
	int n = v->list.count, i, cur = v->cursor;
	char c0 = shelf_initial(v->list.items[cur].title);

	if (n <= 1) return cur;
	if (dir > 0) {
		for (i = 1; i < n; i++) {
			int k = (cur + i) % n;
			if (shelf_initial(v->list.items[k].title) != c0) return k;
		}
		return cur;                           /* every name starts alike */
	}
	i = shelf_group_start(v, cur);
	if (i != cur) return i;
	/* Already at the top of the group. If what precedes it shares the initial
	 * then the shelf is one group and there is nowhere to go - the same answer
	 * down gives, rather than shuffling back by one. */
	i = (cur - 1 + n) % n;
	if (shelf_initial(v->list.items[i].title) == c0) return cur;
	return shelf_group_start(v, i);
}

static void update_games(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	int n = v->list.count;
	if (n <= 0) { a->screen = SCREEN_SYSTEMS; return; }
	if (in_repeat(&a->in, IN_LEFT))  v->cursor = (v->cursor - 1 + n) % n;
	if (in_repeat(&a->in, IN_RIGHT)) v->cursor = (v->cursor + 1) % n;
	if (in_repeat(&a->in, IN_DOWN))  v->cursor = shelf_letter_jump(v, +1);
	if (in_repeat(&a->in, IN_UP))    v->cursor = shelf_letter_jump(v, -1);
	/* L1/R1 jump a screenful, so a long shelf is crossable. Wrapped the long
	 * way round on purpose: C's % truncates toward zero, so on a shelf of
	 * three games the obvious `(cursor - CF_WINDOW + n*2) % n` lands on -1 and
	 * the draw walks off the front of the list. */
	if (in_repeat(&a->in, IN_L1))    v->cursor = ((v->cursor - CF_WINDOW) % n + n) % n;
	if (in_repeat(&a->in, IN_R1))    v->cursor = (v->cursor + CF_WINDOW) % n;
	if (a->in.pressed[IN_BACK]) {
		a->screen = SCREEN_SYSTEMS;
		evict_far(v, TEX_KEEP_FAR);
		return;
	}
	if (a->in.pressed[IN_ACCEPT]) { launch(a); return; }
	cf_set_cursor(&v->cf, v->cursor, n);
}

/* ---------- main ---------------------------------------------------------- */

static void scan_all(app *a)
{
	for (int i = 0; i < a->sys.count; i++) {
		sysview *v = &a->view[i];
		lib_scan(P_ROMS, a->sys.systems[i].folder, a->sys.systems[i].exts, &v->list);
		if (v->list.count > 0) {
			v->tex = calloc((size_t)v->list.count, sizeof *v->tex);
			v->tw = calloc((size_t)v->list.count, sizeof *v->tw);
			v->th = calloc((size_t)v->list.count, sizeof *v->th);
			/* A count with no array behind it would be dereferenced on the
			 * next frame. An empty shelf is the honest answer. */
			if (!v->tex || !v->tw || !v->th) {
				free(v->tex); free(v->tw); free(v->th);
				v->tex = NULL; v->tw = NULL; v->th = NULL;
				lib_free(&v->list);
			}
		}
		fprintf(stderr, "scan: %-16s %d games\n", a->sys.systems[i].folder,
		        v->list.count);
	}
}

/* --shot <file.png> [--screen games|systems] draws one frame, writes it out
 * and exits. This is how the shelf gets looked at without a device in hand:
 * the host build renders exactly what the handheld renders. */
static const char *shot_path;
static int shot_screen = -1;
static int shot_menu, shot_menu_sel;
static int shot_slots, shot_slot_sel;
static int shot_jump;               /* letter-jumps to apply before drawing */
static float shot_slot_aspect = 4.0f / 3.0f;

/* A stand-in for a paused game frame, at whatever shape was asked for: the
 * slot carousel frames the picture to its own aspect, so looking at that
 * without a device means being able to hand it one. */
static SDL_Texture *fake_frame(SDL_Renderer *r, float aspect)
{
	int h = 224, w = (int)(224 * aspect + 0.5f);
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
	                                                SDL_PIXELFORMAT_ARGB8888);
	SDL_Texture *t;
	int x, y;

	if (!s) return NULL;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			Uint32 *px = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
			int checker = ((x / 16) + (y / 16)) & 1;
			px[x] = SDL_MapRGBA(s->format,
			                    (Uint8)(30 + 180 * x / w),
			                    (Uint8)(40 + 150 * y / h),
			                    (Uint8)(checker ? 150 : 90), 255);
		}
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	return t;
}

static void shot_draw_slots(app *a)
{
	slot_view sv = { .aspect = shot_slot_aspect, .saving = 0 };
	int i;

	for (i = 0; i <= GM_SLOTS; i++) {
		/* Slot 5 left empty, so the empty state is in the picture too. */
		if (i == 5) continue;
		sv.have[i] = 1;
		snprintf(sv.when[i], sizeof sv.when[i], "Aug %d %d:%02d:%02d PM",
		         20 + i, 1 + i, i * 7, i * 9);
		sv.thumb[i] = fake_frame(a->r, shot_slot_aspect);
	}
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 185);
	SDL_RenderFillRect(a->r, NULL);
	slot_draw(a, &sv, shot_slot_sel);
	for (i = 0; i <= GM_SLOTS; i++)
		if (sv.thumb[i]) SDL_DestroyTexture(sv.thumb[i]);
}

static void take_shot(app *a)
{
	SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, PLAYOS_SCREEN_W,
	                                                  PLAYOS_SCREEN_H, 32,
	                                                  SDL_PIXELFORMAT_RGBA32);
	draw_shelf(a);
	if (shot_menu) playos_menu_draw(a, shot_menu_sel);
	if (shot_slots) shot_draw_slots(a);
	if (out) {
		/* Read BEFORE presenting: the backbuffer is invalid afterwards. */
		SDL_RenderReadPixels(a->r, NULL, SDL_PIXELFORMAT_RGBA32,
		                     out->pixels, out->pitch);
		SDL_RenderPresent(a->r);
		IMG_SavePNG(out, shot_path);
		SDL_FreeSurface(out);
		/* Say what was drawn, not just that something was: a tool whose whole
		 * job is rendering one state is far more useful when the state it
		 * chose is in the output beside the filename. */
		{
			sysview *v = &a->view[a->sys_cursor];
			const char *what = a->screen == SCREEN_GAMES && v->list.count > 0
			                 ? v->list.items[v->cursor].title
			                 : a->sys.systems[a->sys_cursor].name;
			fprintf(stderr, "wrote %s  [%s] %s\n", shot_path,
			        a->screen == SCREEN_GAMES ? "games" : "systems", what);
		}
	}
}

int main(int argc, char *argv[])
{
	app a = { 0 };
	char path[CFG_STR * 2];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!strcmp(argv[i], "--screen") && i + 1 < argc)
			shot_screen = strcmp(argv[++i], "games") == 0 ? SCREEN_GAMES
			                                              : SCREEN_SYSTEMS;
		/* --menu [row] draws the PlayOS menu over whichever screen --screen
		 * asked for, so the panel can be looked at without a device. */
		else if (!strcmp(argv[i], "--menu")) {
			shot_menu = 1;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_menu_sel = atoi(argv[++i]);
		}
		/* --slots <sel> [aspect] draws one frame of the save/load carousel
		 * over synthetic frames, which is the only way to look at it without
		 * a game running on a device. */
		/* --jump N applies N letter-jumps before the shot (negative for up),
		 * so the d-pad's behavior on a real library can be checked without a
		 * device or a hand on it. */
		else if (!strcmp(argv[i], "--jump") && i + 1 < argc) {
			shot_jump = atoi(argv[++i]);
		}
		else if (!strcmp(argv[i], "--slots")) {
			shot_slots = 1;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_slot_sel = atoi(argv[++i]);
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_slot_aspect = (float)atof(argv[++i]);
		}
	}
	signal(SIGTERM, on_sigterm);
	signal(SIGINT, on_sigterm);

	t_boot0 = plat_now_ms();
	paths_init();
	build_child_env();

	snprintf(path, sizeof path, "%s/systems.cfg", P_ROOT);
	if (!cfg_load_systems(path, &a.sys)) {
		fprintf(stderr, "no usable %s\n", path);
		return 1;
	}
	snprintf(path, sizeof path, "%s/playos.cfg", P_ROOT);
	cfg_load_playos(path, &a.cfg);

	/* Scan every system now, not when one is opened: it is three directory
	 * reads, it happens behind the boot animation, and it means walking into
	 * a system is a frame rather than a wait. */
	scan_all(&a);
	/* After the scan, because it indexes by system, and before anything can
	 * launch, because the mode has to reach Diatom with the first RUN. */
	display_load(&a);
	t_mark("scan");

	/* BEFORE the first frame this process ever draws: if a previous launcher
	 * died while a game was running, the resident is still presenting through
	 * fbdev right now, and drawing the shelf over it is the two-presenter
	 * case that wedges the display engine in-kernel (Diatom's handoff spike;
	 * it cost a power cycle to prove, twice). plat_resident_ready() connects,
	 * and on READY state=running it stops the game and drains to EXIT - so by
	 * the time video comes up, only one presenter exists. The check at launch
	 * time was too late by definition: this process draws long before the
	 * player launches anything. */
	plat_resident_ready();

	if (!plat_video_init()) { fprintf(stderr, "video init failed\n"); return 1; }
	IMG_Init(IMG_INIT_PNG);
	a.r = plat_renderer();
	plat_input_init();
	/* The config values are defaults handed to the settings code, which
	 * prefers the player's saved levels over them. They are NOT reapplied
	 * afterwards: doing that put playos.cfg ahead of the level the player last
	 * chose and undid every nudge on the next restart. */
	plat_settings_init(a.cfg.volume, a.cfg.brightness);
	plat_leds_off();
	t_mark("video+input");

	/* Before ui_init, which is where the sizes are decided; it persists across
	 * the ui_quit/ui_init pair the standalone-emulator fallback goes through. */
	ui_set_font_scale(a.cfg.font_scale);
	if (!ui_init(a.r, P_FONT)) fprintf(stderr, "font init failed\n");
	t_mark("font+settings");

	a.sys_cursor = 0;
	if (a.cfg.startup_system[0])
		for (int i = 0; i < a.sys.count; i++)
			if (strcasecmp(a.sys.systems[i].name, a.cfg.startup_system) == 0) {
				a.sys_cursor = i;
				break;
			}
	restore_place(&a);
	cf_reset(&a.cf_sys, a.sys_cursor);
	a.tint = a.sys.systems[a.sys_cursor].accent;
	prime_sys_window(&a);
	prime_window(&a, a.sys_cursor);
	t_mark("card assets");

	a.running = true;
	last_tint_ms = plat_now_ms();

	if (shot_path) {
		if (shot_screen >= 0) a.screen = (screen_id)shot_screen;
		if (shot_jump) {
			sysview *v = &a.view[a.sys_cursor];
			int k, dir = shot_jump > 0 ? 1 : -1;
			for (k = 0; k < (shot_jump < 0 ? -shot_jump : shot_jump); k++)
				if (v->list.count > 0) v->cursor = shelf_letter_jump(v, dir);
			cf_reset(&v->cf, v->cursor);
		}
		if (a.screen == SCREEN_GAMES) prime_window(&a, a.sys_cursor);
		take_shot(&a);
		goto done;
	}

	/* Dev instrumentation, same standing as --shot: launch one game with no
	 * buttons pressed, so the resident path can be exercised over adb with
	 * nobody holding the device. PLAYOS_AUTOLAUNCH="TAG<tab>rom-filename";
	 * pair with PLAYOS_AUTOSTOP_S to end the game on a clock. */
	{
		const char *auto_spec = getenv("PLAYOS_AUTOLAUNCH");
		if (auto_spec && strchr(auto_spec, '\t')) {
			char tag[64], file[LIB_PATH];
			const char *bar = strchr(auto_spec, '\t');
			int si, gi, found = 0;
			snprintf(tag, sizeof tag, "%.*s", (int)(bar - auto_spec), auto_spec);
			snprintf(file, sizeof file, "%s", bar + 1);
			for (si = 0; si < a.sys.count && !found; si++) {
				if (strcmp(a.sys.systems[si].tag, tag) != 0) continue;
				for (gi = 0; gi < a.view[si].list.count; gi++) {
					const char *b = strrchr(a.view[si].list.items[gi].file, '/');
					b = b ? b + 1 : a.view[si].list.items[gi].file;
					if (strcmp(b, file) == 0) {
						a.sys_cursor = si;
						a.view[si].cursor = gi;
						found = 1;
						break;
					}
				}
			}
			if (found) {
				fprintf(stderr, "autolaunch: %s / %s\n", tag, file);
				launch(&a);
			} else {
				fprintf(stderr, "autolaunch: no %s / %s in the library\n", tag, file);
			}
			goto done;
		}
	}

	/* Everything above ran while the boot animation was on screen. Only now
	 * is it this process's turn to own the framebuffer. */
	wait_for_boot_anim();

	/* Swallow the input noise a boot produces -- replayed wake presses, the
	 * bursts input devices emit as they come up -- before honouring anything. */
	{
		Uint32 grace = SDL_GetTicks() + 350;
		while (SDL_GetTicks() < grace) { plat_input_poll(&a.in); SDL_Delay(8); }
		memset(&a.in, 0, sizeof a.in);
	}

	while (a.running) {
		plat_input_poll(&a.in);
		if (a.in.quit_requested || want_quit) break;

		if (a.in.pressed[IN_POWER]) { power_off(&a); break; }

		if (in_repeat(&a.in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a.in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a.in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a.in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		/* MENU on the shelf is PlayOS's own menu, the counterpart to the one
		 * MENU opens in a game. It draws over the shelf and returns here. */
		if (a.in.pressed[IN_MENU]) { playos_menu(&a); continue; }

		if (a.screen == SCREEN_SYSTEMS) update_systems(&a);
		else update_games(&a);
		if (!a.running) break;

		tick_tint(&a);
		render(&a);
	}

done:
	free_all_textures(&a);
	for (int i = 0; i < a.sys.count; i++) {
		free(a.view[i].tex); free(a.view[i].tw); free(a.view[i].th);
		lib_free(&a.view[i].list);
	}
	ui_quit();
	IMG_Quit();
	plat_input_quit();
	plat_video_quit();
	SDL_Quit();
	return 0;
}
