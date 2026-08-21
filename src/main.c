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
	coverflow cf;
} sysview;

typedef struct {
	systems_cfg sys;
	playos_cfg cfg;

	SDL_Texture *sys_tex[CFG_MAX_SYSTEMS];
	int sys_w[CFG_MAX_SYSTEMS], sys_h[CFG_MAX_SYSTEMS];
	sysview view[CFG_MAX_SYSTEMS];

	screen_id screen;
	int sys_cursor;
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

/* Where minarch keeps a game's autosave preview: the frame the player was
 * looking at when they stopped. minarch names that directory after the ROM's
 * folder under Roms/ (getEmuName), which is not the same as the save tag. */
static void preview_path(app *a, int s, const game_entry *g, char *out, size_t n)
{
	const char *base = strrchr(g->file, '/');
	base = base ? base + 1 : g->file;
	snprintf(out, n, "%s/.minui/%s/%s.9.bmp", P_SHARED, a->sys.systems[s].folder, base);
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
			v->tex[i] = ui_make_card(a->r, v->list.items[i].name,
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
	/* A wash of the focused system's colour along the bottom edge, so the
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
	ui_text(a->r, ui_font_small(), line, PLAYOS_SCREEN_W / 2, 694, 0, UI_TEXT_DIM);
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
		int tw = ui_text_width(ui_font_big(), g->name);
		int tx = PLAYOS_SCREEN_W / 2;
		/* A game with an autosave gets a dot in the system's colour beside
		 * its name: pressing A on it does not start it, it continues it. */
		if (game_has_state(a, a->sys_cursor, g)) {
			int dx = tx - tw / 2 - 26, dy = 74;
			SDL_SetRenderDrawColor(a->r, (Uint8)(s->accent >> 16),
			                       (Uint8)(s->accent >> 8), (Uint8)s->accent, 255);
			for (int k = -5; k <= 5; k++) {
				int w = (int)(sqrt((double)(25 - k * k)) + 0.5);
				SDL_RenderDrawLine(a->r, dx - w, dy + k, dx + w, dy + k);
			}
		}
		ui_text(a->r, ui_font_big(), g->name, tx, 46, 0, UI_TEXT);
		snprintf(count, sizeof count, "%d / %d", v->cursor + 1, v->list.count);
		ui_text(a->r, ui_font_small(), count, PLAYOS_SCREEN_W / 2, 700, 0, UI_TEXT_DIM);
	} else {
		ui_text(a->r, ui_font_big(), s->name, PLAYOS_SCREEN_W / 2, 46, 0, UI_TEXT);
	}
	ui_rail(a->r, PLAYOS_SCREEN_W, PLAYOS_SCREEN_H, v->cursor, v->list.count,
	        s->accent);
}

static void render(app *a)
{
	draw_background(a);
	if (a->screen == SCREEN_SYSTEMS) draw_systems(a);
	else draw_games(a);
	if (battery_low()) draw_low_battery_dot(a->r);
	plat_draw_osd(a->r);
	SDL_RenderPresent(a->r);
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

/* Coming back: the shelf fades up out of black. Short, because the launcher
 * never went anywhere -- it kept its context through the whole game and has
 * nothing to rebuild. */
static void anim_return(app *a, unsigned ms)
{
	unsigned t0 = plat_now_ms(), now;
	while ((now = plat_now_ms()) - t0 < ms) {
		float k = (float)(now - t0) / (float)ms;
		render(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, (Uint8)(255 * (1.0f - k)));
		SDL_RenderFillRect(a->r, NULL);
		SDL_RenderPresent(a->r);
		SDL_Delay(6);
	}
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
	remember_place(a);
	plat_request_poweroff();
	anim_poweroff(a);
	a->running = false;
}

/* ---------- launching ----------------------------------------------------- */

static char env_buf[10][CFG_STR * 2];
static const char *minarch_env[24];

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
	for (i = 0; i < sizeof fixed / sizeof *fixed; i++) minarch_env[n++] = fixed[i];
	snprintf(env_buf[0], sizeof env_buf[0], "ROMS_PATH=%s", P_ROMS);
	snprintf(env_buf[1], sizeof env_buf[1], "SYSTEM_PATH=%s", P_ROOT);
	snprintf(env_buf[2], sizeof env_buf[2], "CORES_PATH=%s/cores", P_ROOT);
	snprintf(env_buf[3], sizeof env_buf[3], "USERDATA_PATH=%s", P_USERDATA);
	snprintf(env_buf[4], sizeof env_buf[4], "SHARED_USERDATA_PATH=%s", P_SHARED);
	snprintf(env_buf[5], sizeof env_buf[5], "LOGS_PATH=%s/logs", P_USERDATA);
	snprintf(env_buf[6], sizeof env_buf[6], "HOME=%s", P_USERDATA);
	snprintf(env_buf[7], sizeof env_buf[7], "LD_LIBRARY_PATH=%s/lib:/usr/trimui/lib", P_ROOT);
	for (i = 0; i <= 7; i++) minarch_env[n++] = env_buf[i];
	minarch_env[n] = NULL;
}

static void launch(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	const system_cfg *s = &a->sys.systems[a->sys_cursor];
	char core[CFG_STR * 2], elf[CFG_STR * 2], rom[LIB_PATH * 2], tagenv[64];
	const char *env[28];
	char *argv[4];
	bool resident = false;
	int n = 0;
	FILE *f;

	if (v->list.count == 0) return;

	snprintf(core, sizeof core, "%s/cores/%s_libretro.so", P_ROOT, s->core);
	snprintf(elf, sizeof elf, "%s/minarch.elf", P_ROOT);
	snprintf(rom, sizeof rom, "%s/%s/%s", P_ROMS, s->folder, v->list.items[v->cursor].file);

	/* Ask for the auto-resume slot. minarch loads slot 9 if a state is there
	 * and starts fresh if it is not, and it writes that slot on every way out
	 * of a game -- so a game always comes up where it was left. */
	f = fopen("/tmp/resume_slot.txt", "w");
	if (f) { fputs("9", f); fclose(f); }

	remember_place(a);

	if (plat_resident_ready()) {
		/* The emulator is already up, holding the GL context and all three
		 * cores, so this is ~200ms rather than ~1100. Nothing here is torn
		 * down: this process keeps its own context too, and only one of the
		 * two draws at a time because this one is about to block. */
		char req[LIB_PATH * 2 + CFG_STR * 2 + 64];
		snprintf(req, sizeof req, "%s\t%s\t%s\n", s->tag, core, rom);
		if (plat_resident_send(req)) {
			anim_launch(a, 190);       /* runs WHILE the game loads */
			plat_resident_wait();
			resident = true;
		} else {
			fprintf(stderr, "resident emulator did not answer, falling back\n");
		}
	}

	if (!resident) {
		/* One game per process, the old way: the fallback for a resident that
		 * is missing or has died. It has to take the display, so the
		 * animation plays first rather than during. */
		anim_launch(a, 190);
		free_all_textures(a);
		ui_quit();
		plat_input_quit();
		plat_video_quit();

		while (minarch_env[n]) { env[n] = minarch_env[n]; n++; }
		snprintf(tagenv, sizeof tagenv, "PLAYOS_TAG=%s", s->tag);
		env[n++] = tagenv;
		env[n] = NULL;
		argv[0] = elf; argv[1] = core; argv[2] = rom; argv[3] = NULL;
		fprintf(stderr, "minarch exited %d\n", plat_run(argv, env, P_ROOT));

		if (!plat_video_init() || !plat_input_init()) { a->running = false; return; }
		a->r = plat_renderer();
		ui_init(a->r, P_FONT);
		prime_sys_window(a);
		prime_window(a, a->sys_cursor);
	}

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

	plat_leds_off();
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	if (access(PLAYOS_POWEROFF_FLAG, F_OK) == 0) { a->running = false; return; }
	/* Power was pressed during the game. The emulator no longer handles that
	 * key itself, so the press arrived here. */
	if (plat_run_power_pressed()) { power_off(a); return; }

	anim_return(a, 130);
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

static void update_games(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	int n = v->list.count;
	if (n <= 0) { a->screen = SCREEN_SYSTEMS; return; }
	if (in_repeat(&a->in, IN_LEFT))  v->cursor = (v->cursor - 1 + n) % n;
	if (in_repeat(&a->in, IN_RIGHT)) v->cursor = (v->cursor + 1) % n;
	/* L1/R1 jump a screenful, so a long shelf is crossable. */
	if (in_repeat(&a->in, IN_L1))    v->cursor = (v->cursor - CF_WINDOW + n * 2) % n;
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

static void take_shot(app *a)
{
	SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, PLAYOS_SCREEN_W,
	                                                  PLAYOS_SCREEN_H, 32,
	                                                  SDL_PIXELFORMAT_RGBA32);
	draw_background(a);
	if (a->screen == SCREEN_SYSTEMS) draw_systems(a);
	else draw_games(a);
	if (out) {
		/* Read BEFORE presenting: the backbuffer is invalid afterwards. */
		SDL_RenderReadPixels(a->r, NULL, SDL_PIXELFORMAT_RGBA32,
		                     out->pixels, out->pitch);
		SDL_RenderPresent(a->r);
		IMG_SavePNG(out, shot_path);
		SDL_FreeSurface(out);
		fprintf(stderr, "wrote %s\n", shot_path);
	}
}

int main(int argc, char *argv[])
{
	app a = { 0 };
	char path[CFG_STR * 2];
	unsigned last_tint_ms;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!strcmp(argv[i], "--screen") && i + 1 < argc)
			shot_screen = strcmp(argv[++i], "games") == 0 ? SCREEN_GAMES
			                                              : SCREEN_SYSTEMS;
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
	t_mark("scan");

	if (!plat_video_init()) { fprintf(stderr, "video init failed\n"); return 1; }
	IMG_Init(IMG_INIT_PNG);
	a.r = plat_renderer();
	plat_input_init();
	plat_settings_init();
	plat_leds_off();
	t_mark("video+input");

	if (!ui_init(a.r, P_FONT)) fprintf(stderr, "font init failed\n");
	if (a.cfg.volume >= 0) plat_volume_set_pct(a.cfg.volume);
	/* Always reapply brightness after InitSettings: it can come back with a
	 * stale, dimmer value than the one launch.sh set for the boot animation,
	 * and the step down is visible. */
	plat_brightness_set(a.cfg.brightness >= 0 ? a.cfg.brightness : 8);
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
		if (a.screen == SCREEN_GAMES) prime_window(&a, a.sys_cursor);
		take_shot(&a);
		goto done;
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

		if (a.screen == SCREEN_SYSTEMS) update_systems(&a);
		else update_games(&a);
		if (!a.running) break;

		/* Ease the background tint toward the focused system rather than
		 * snapping: the colour is meant to feel like the light the machine
		 * gives off, and light does not cut. */
		{
			unsigned now = plat_now_ms();
			float dt = (float)(now - last_tint_ms) / 1000.0f;
			last_tint_ms = now;
			if (dt > 0.1f) dt = 0.1f;
			a.tint = ui_mix(a.tint, a.sys.systems[a.sys_cursor].accent,
			                1.0f - expf(-dt * 9.0f));
		}

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
