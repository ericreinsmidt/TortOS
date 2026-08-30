/* SPDX-License-Identifier: 0BSD
 *
 * TortOS -- a custom firmware for the TrimUI Brick that plays NES, TurboGrafx
 * -16 and Game Boy Advance games, and does nothing else.
 *
 * The whole design metric is speed. The launcher starts behind the boot
 * animation rather than after it, hands games to an emulator that is already
 * running rather than starting one, and never tears its own display down --
 * so coming back from a game is a frame, not a second and a half. What is on
 * screen is a row of cards, the name of the thing under the cursor, and a
 * rail saying where you are. Nothing else.
 */
#include "cheevos.h"
#include "config.h"
#include "coverflow.h"
#include "library.h"
#include "platform.h"
#include "favorites.h"
#include "notice.h"
#include "rafetch.h"
#include "rahash.h"
#include "ranet.h"
#include "keyboard.h"
#include "wifi.h"
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
	/* NULL on a real shelf, where every game belongs to the system whose
	 * shelf it is. Favorites is a shelf of games drawn from many systems, so
	 * each entry has to carry its own - the core to load, the folder the ROM
	 * is under, the accent, and where the save state lives all follow from
	 * it, and every one of them would be wrong if taken from the shelf. */
	int *owner;
} sysview;

/* Diatom's display modes, in the order TortOS offers them: the sensible
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
	tortos_cfg cfg;

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
	/* Set for one launch only: the game that comes back after a shutdown
	 * opens with the menu up. Cleared as it is used, so quitting to the
	 * shelf and launching the same game again behaves normally. */
	bool resume_menu;
	SDL_Renderer *r;
} app;

/* Defined down with the shelf building it belongs to, declared here because
 * the input loop calls it the moment a favorite changes. */
static void refresh_favorites_shelf(app *a);

static volatile sig_atomic_t want_quit;
static void on_sigterm(int sig) { (void)sig; want_quit = 1; plat_terminate(); }

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
	const char *flag = getenv("TORTOS_ANIM_FLAG");
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

/* Every visible system every time, plus whatever was already in the file for
 * systems that are not on the shelf right now.
 *
 * The second half is not tidiness. Empty systems are hidden, so "every system"
 * means "every system with games in it today" - and a plain rewrite would
 * quietly erase the display mode of any system whose ROMs happen to be off
 * the card at the moment. Take the ROMs out, change one unrelated setting,
 * put the ROMs back, and the mode you had chosen is gone with no indication
 * it ever existed. Preserving unknown tags costs one pass over a file that is
 * nine short lines. */
static void display_save(app *a)
{
	char p[CFG_STR * 2], line[CFG_STR];
	char keep[CFG_MAX_SYSTEMS * 2][CFG_STR];
	int nkeep = 0;
	FILE *f;
	int i;

	snprintf(p, sizeof p, "%s/display.cfg", P_USERDATA);

	f = fopen(p, "r");
	if (f) {
		while (fgets(line, sizeof line, f) && nkeep < (int)(sizeof keep / sizeof keep[0])) {
			char *eq;
			line[strcspn(line, "\r\n")] = '\0';
			if (!line[0]) continue;
			eq = strchr(line, '=');
			if (!eq) continue;
			*eq = '\0';
			for (i = 0; i < a->sys.count; i++)
				if (!strcmp(a->sys.systems[i].tag, line)) break;
			*eq = '=';
			if (i == a->sys.count) snprintf(keep[nkeep++], CFG_STR, "%s", line);
		}
		fclose(f);
	}

	f = fopen(p, "w");
	if (!f) return;
	for (i = 0; i < a->sys.count; i++)
		fprintf(f, "%s=%s\n", a->sys.systems[i].tag, DMODES[a->view[i].dmode].name);
	for (i = 0; i < nkeep; i++)
		fprintf(f, "%s\n", keep[i]);
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
 * The directory and the slot names are TortOS's own. Both were briefly
 * borrowed from the emulator TortOS replaced, and the borrowing cost more than
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
	snprintf(out, n, "%s/.tortos/%s/%s.%s.bmp",
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
	snprintf(out, n, "%s/.tortos/%s/%s.%s.state",
	         P_SHARED, a->sys.systems[s].folder, base, slot_name(slot));
}

static void slot_preview_path(app *a, int s, const game_entry *g, int slot,
                              char *out, size_t n)
{
	const char *base = strrchr(g->file, '/');
	base = base ? base + 1 : g->file;
	snprintf(out, n, "%s/.tortos/%s/%s.%s.bmp",
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
	snprintf(d, sizeof d, "%s/.tortos", P_SHARED);
	mkdir(d, 0755);
	snprintf(d, sizeof d, "%s/.tortos/%s", P_SHARED, a->sys.systems[s].folder);
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
/* The system an entry on a shelf belongs to. Real shelves answer with
 * themselves; Favorites answers per entry. */
static int shelf_owner(app *a, int sysidx, int item)
{
	sysview *v = &a->view[sysidx];
	if (v->owner && item >= 0 && item < v->list.count) return v->owner[item];
	return sysidx;
}

static SDL_Texture *game_get_tex(void *ctx, int i, int *w, int *h)
{
	app *a = ctx;
	int s = a->sys_cursor;
	sysview *v = &a->view[s];
	/* The game's own system, not the shelf's: on Favorites those differ, and
	 * every one of these three lookups would otherwise go to the wrong
	 * folder, the wrong state directory and the wrong accent. */
	int o = shelf_owner(a, s, i);

	if (!v->tex[i]) {
		char path[LIB_PATH * 3];
		snprintf(path, sizeof path, "%s/%s/.media/%s.png",
		         P_ROMS, a->sys.systems[o].folder, v->list.items[i].name);
		v->tex[i] = load_image(a->r, path, &v->tw[i], &v->th[i]);
		if (!v->tex[i]) {
			preview_path(a, o, &v->list.items[i], path, sizeof path);
			v->tex[i] = load_image(a->r, path, &v->tw[i], &v->th[i]);
		}
		if (!v->tex[i])
			v->tex[i] = ui_make_card(a->r, v->list.items[i].title,
			                         a->sys.systems[o].accent,
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

/* The text sizes offered, spanning ui_set_font_scale's own 0.75..1.50 clamp
 * so every step is reachable and none is silently clamped into its neighbor.
 * A ladder rather than a nudge, because the fonts are reopened on every
 * change and there is no sense doing that for one percent. */
static const float TEXT_SCALES[] = { 0.75f, 0.85f, 1.00f, 1.15f, 1.30f, 1.50f };
#define TEXT_SCALE_COUNT ((int)(sizeof TEXT_SCALES / sizeof TEXT_SCALES[0]))

static int text_scale_step(void)
{
	float cur = ui_get_font_scale();
	int i, best = 0;
	float bd = 1e9f;

	/* Nearest, not equal: the value may have come from tortos.cfg, where
	 * anything in range is legal and 1.07 is as valid as 1.00. */
	for (i = 0; i < TEXT_SCALE_COUNT; i++) {
		float d = cur > TEXT_SCALES[i] ? cur - TEXT_SCALES[i] : TEXT_SCALES[i] - cur;
		if (d < bd) { bd = d; best = i; }
	}
	return best;
}

static void text_scale_save(float scale)
{
	char p[CFG_STR * 2];
	FILE *f;

	snprintf(p, sizeof p, "%s/textsize.cfg", P_USERDATA);
	f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "font_scale=%.2f\n", (double)scale);
	fclose(f);
}

static float text_scale_load(float fallback)
{
	char p[CFG_STR * 2];
	FILE *f;
	float v = 0.0f;

	snprintf(p, sizeof p, "%s/textsize.cfg", P_USERDATA);
	f = fopen(p, "r");
	if (!f) return fallback;
	if (fscanf(f, "font_scale=%f", &v) != 1) v = 0.0f;
	fclose(f);
	return v > 0.0f ? v : fallback;
}

/* Whether the radio should come up at boot.
 *
 * In .userdata rather than tortos.cfg, on the same split as brightness: the
 * shipped config says what a fresh card does, .userdata says what THIS device
 * was last doing. launch.sh reads this file first and falls back to the
 * config, so a card with no preference yet behaves as shipped.
 *
 * Written the moment it changes rather than at shutdown, because a handheld
 * is switched off by holding a button or by running the battery flat, and
 * neither of those is a chance to save anything. */
static void wifi_pref_save(bool on)
{
	char p[CFG_STR * 2];
	FILE *f;

	snprintf(p, sizeof p, "%s/wifi.cfg", P_USERDATA);
	f = fopen(p, "w");
	if (!f) return;
	fprintf(f, "wifi=%d\n", on ? 1 : 0);
	fclose(f);
}

/* Beside the save states and keyed like them, so a favorite travels with the
 * saves it belongs next to and survives a card reflash. */
static void fav_path(char *out, size_t n)
{
	snprintf(out, n, "%s/.tortos/favorites.cfg", P_SHARED);
}

/* Beside favorites, and shared rather than per-device for the same reason:
 * what a player has earned belongs to them, not to the card it was earned on.
 * Keyed by RetroAchievements game id and achievement id, so it survives a ROM
 * being renamed or moved between folders - which the tag-and-file key that
 * favorites use would not. */
static void chv_store_path(char *out, size_t n)
{
	snprintf(out, n, "%s/.tortos/cheevos.cfg", P_SHARED);
}

/* Diatom watches this file, and it is rewritten per launch: it is the set
 * minus what has already been earned, which only this side knows. Runtime
 * scratch, so per-device rather than shared. */
static void chv_active_path(char *out, size_t n)
{
	snprintf(out, n, "%s/cheevos-active.set", P_USERDATA);
}

/* The account. Per-device rather than shared, because it holds a session
 * token: a card moved to another device should not carry one with it. */
static void ra_creds_path(char *out, size_t n)
{
	snprintf(out, n, "%s/ra.cfg", P_USERDATA);
}

/* Defined with the Wi-Fi screen it began in, and used here because signing in
 * and syncing are the other two things that make the player wait. */
static void wait_panel(app *a, const char *heading, const char *msg);

/* Pull down what the account already holds for the loaded game, and mark
 * those earned locally so they are not watched, not re-announced, and not
 * submitted again.
 *
 * The account wins on what EXISTS; the local store wins on what is still
 * owed, because it is the only record of anything earned offline. Neither is
 * discarded. Silent when offline or not signed in - that is the ordinary
 * case, and it leaves the device working from what it knows. */
static void ra_merge_unlocks(const int *ids, int n)
{
	int i, added = 0;

	if (n <= 0 || chv_game() <= 0) return;

	/* Only ids the set actually has. The account carries entries the set does
	 * not - RetroAchievements' "Unknown Emulator" notice is one, and it went
	 * straight into the store the first time this ran. Recording those adds a
	 * row that can never be displayed and could later be submitted as a
	 * duplicate of something that was never an achievement. */
	for (i = 0; i < n; i++) {
		int k;

		for (k = 0; k < chv_count(); k++)
			if (chv_at(k)->id == ids[i]) break;
		if (k == chv_count()) continue;
		if (chv_note_earned(chv_game(), ids[i], true)) added++;
	}

	if (added) {
		char p[CFG_STR * 2];
		chv_store_path(p, sizeof p);
		chv_earned_save(p);
	}
}

/* And the one that runs on every other launch: started here, collected after
 * the game. See rafetch.h for the measurement that made this necessary. */
static void ra_sync_collect_and_merge(void)
{
	int ids[CHV_MAX], n = ra_sync_collect(ids, CHV_MAX);

	if (n > 0) ra_merge_unlocks(ids, n);
}

/* Send what is owed. Called once the game is over and the launcher has the
 * screen back - never from the wait loop.
 *
 * Anything that will not send stays pending and is tried again next time,
 * which is the offline queueing RA's own requirements ask for and the right
 * shape regardless: an unlock earned on a plane is still earned. */
static void ra_flush_unlocks(const char *rom, const char *tag)
{
	char hash[33] = "";
	int game, id, sent = 0, settled = 0;

	if (!ra_signed_in() || chv_pending_count() == 0 || !ra_online()) return;

	/* The hash is what a real client sends alongside an award, and it is the
	 * one for the game just played - so only its own unlocks carry it. */
	ra_hash_rom(rom, tag, hash);

	/* Index 0 every time: a successful send marks the row synced, so the next
	 * pending one becomes index 0. A failure stops the loop rather than
	 * spinning on it - if one will not go, the rest will not either. */
	while (chv_pending_at(0, &game, &id)) {
		int rc = ra_submit_unlock(id, game == chv_game() ? hash : NULL);

		/* 0 is "the account already had it", which settles the row just as
		 * surely as sending it. Only a real failure stops the loop - if one
		 * will not go, the rest will not either. */
		if (rc < 0) break;
		chv_mark_synced(game, id);
		settled++;
		if (rc > 0) sent++;
	}
	/* On `settled`, not on `sent`. A row the account already had is settled
	 * without being sent, and writing only when something was sent left that
	 * mark in memory only - so it came back pending on the next boot and was
	 * retried, forever. Seen on the device: one unlock submitted by hand
	 * outside the launcher, refused as already-held, and asked again at the
	 * end of every game after that. */
	if (settled) {
		char p[CFG_STR * 2];
		chv_store_path(p, sizeof p);
		chv_earned_save(p);
		fprintf(stderr, "ra: %d sent, %d already held, %d still queued\n",
		        sent, settled - sent, chv_pending_count());
	}
}

/* Where the launcher's own work happens during a game. Ten times a second,
 * from inside the wait loop, and it must stay cheap: that loop is the power
 * button's watchdog.
 *
 * Its one job is the first play of a game. The set is being found behind the
 * game rather than in front of it, and when it lands the player is told the
 * only way anyone can be told during a game - over Diatom's overlay. */
static char g_pending_set[LIB_PATH * 2];
static char g_pending_active[LIB_PATH * 2];

static void on_game_tick(void)
{
	char msg[96];

	int rc;

	if (!g_pending_set[0]) return;

	/* Once. Calling it twice advances the state machine twice, which is a
	 * lookup answered and then thrown away. */
	rc = ra_fetch_step();
	if (rc == 0) return;                        /* still working */
	if (rc < 0) { g_pending_set[0] = '\0'; return; }  /* RA has never seen it */

	if (!chv_load(g_pending_set)) { g_pending_set[0] = '\0'; return; }
	ra_sync_begin(chv_game());        /* the account's answer, collected at exit */

	if (chv_write_active(g_pending_active)) {
		plat_resident_line("SETCHEEVOS	path=%s	console=%d",
		                   g_pending_active, chv_console());
		snprintf(msg, sizeof msg, "%d to earn", chv_count());
	} else {
		snprintf(msg, sizeof msg, "all %d already earned", chv_count());
	}

	/* Rendering is safe here; presenting would not be. Same rule as an
	 * unlock notice, and the same path. */
	{
		char p[CFG_STR * 2];

		snprintf(p, sizeof p, "%s/notice.dtov", P_USERDATA);
		if (notice_render(msg, chv_game_title(), p))
			plat_resident_line("OVERLAY	path=%s	ms=3500", p);
	}
	g_pending_set[0] = '\0';
}

/* Called from inside plat_resident_wait, mid-game, when the launcher owns
 * nothing and can draw nothing. Recording is all that happens here; the
 * telling happens the next time this process has the screen. */
static void on_cheevo_unlocked(int id)
{
	char p[CFG_STR * 2];
	const cheevo *c = NULL;
	int i;

	if (!chv_note_unlock(id)) return;

	chv_store_path(p, sizeof p);
	/* Written through immediately rather than at exit. A game ends by power
	 * button as often as by menu, and an achievement lost to a flat battery is
	 * one the player has to earn twice. */
	chv_earned_save(p);

	/* And say so, on screen, now. This process owns no display while a game
	 * runs, so it renders the line and Diatom composites it (its ADR-0027).
	 * Rendering is safe here; presenting would not be. */
	for (i = 0; i < chv_count(); i++)
		if (chv_at(i)->id == id) { c = chv_at(i); break; }

	{
		char head[64];

		/* Points on the context line, not the name: "First Blood!" is what
		 * the player wants to read, and "1 point" is what it was worth. */
		if (c && c->points > 0)
			snprintf(head, sizeof head, "Unlocked  -  %d point%s",
			         c->points, c->points == 1 ? "" : "s");
		else
			snprintf(head, sizeof head, "Unlocked");

		snprintf(p, sizeof p, "%s/notice.dtov", P_USERDATA);
		if (notice_render(head, c && c->title[0] ? c->title : "Unlocked", p))
			plat_resident_line("OVERLAY\tpath=%s\tms=4000", p);
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

/* Whether a game was running when this process last stopped.
 *
 * `.last` says what you were LOOKING at, which is not the same question - it
 * is written when the shelf moves as well as when a game starts, and it says
 * nothing about whether that game was still up. This is written at launch and
 * cleared only on a clean return to the shelf, so a power-off mid-game and a
 * quit are distinguishable, which is the whole point.
 *
 * Beside `.last` and in the same shape, because it is the same two facts. */
static void playing_path(char *out, size_t n)
{
	snprintf(out, n, "%s/.playing", P_ROOT);
}

static void playing_set(app *a)
{
	char p[CFG_STR * 2];
	sysview *v = &a->view[a->sys_cursor];
	int o = shelf_owner(a, a->sys_cursor, v->cursor);
	FILE *f;

	if (v->list.count == 0) return;
	playing_path(p, sizeof p);
	f = fopen(p, "w");
	if (!f) return;
	/* The GAME's system, not the shelf's - launching from Favorites otherwise
	 * records a system that does not own the ROM, exactly as the launch path
	 * itself had to learn. */
	fprintf(f, "%s\n%s\n", a->sys.systems[o].tag, v->list.items[v->cursor].file);
	fclose(f);
}

static void playing_clear(void)
{
	char p[CFG_STR * 2];

	playing_path(p, sizeof p);
	remove(p);
}

/* Point the cursor at whatever was being played, if anything was, and say so.
 * Only when there is a state to come back to: without one, "resume" would
 * mean starting the game from its title screen, which is not what anyone
 * powering off mid-game is asking for. */
static bool playing_restore(app *a)
{
	char p[CFG_STR * 2], tag[64] = { 0 }, file[LIB_PATH] = { 0 }, st[LIB_PATH * 2];
	FILE *f;
	int i, k;

	playing_path(p, sizeof p);
	f = fopen(p, "r");
	if (!f) return false;
	if (fgets(tag, sizeof tag, f)) tag[strcspn(tag, "\r\n")] = 0;
	if (fgets(file, sizeof file, f)) file[strcspn(file, "\r\n")] = 0;
	fclose(f);
	if (!tag[0] || !file[0]) { playing_clear(); return false; }

	for (i = 0; i < a->sys.count; i++) {
		if (strcmp(a->sys.systems[i].tag, tag) != 0) continue;
		for (k = 0; k < a->view[i].list.count; k++) {
			if (strcmp(a->view[i].list.items[k].file, file) != 0) continue;

			state_path(a, i, &a->view[i].list.items[k], st, sizeof st);
			if (access(st, R_OK) != 0) {
				/* The game is gone from under the state, or the autosave
				 * never landed. Clear the marker rather than trying every
				 * boot from here on. */
				playing_clear();
				return false;
			}
			a->sys_cursor = i;
			a->view[i].cursor = k;
			return true;
		}
	}
	playing_clear();                  /* the ROM or its system is no longer here */
	return false;
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

/* One cell of the shell: a filled hexagon as a six-triangle fan, because
 * SDL_RenderGeometry is the only primitive here that antialiases nothing and
 * therefore looks identical to the boot animation's rasteriser. */
static void draw_hex(SDL_Renderer *r, float cx, float cy, float rad, SDL_Color col)
{
	SDL_Vertex v[7];
	int idx[18], i;
	v[0].position.x = cx; v[0].position.y = cy;
	for (i = 0; i < 6; i++) {
		float a = (float)(M_PI / 6.0 + i * M_PI / 3.0);
		v[i + 1].position.x = cx + rad * cosf(a);
		v[i + 1].position.y = cy + rad * sinf(a);
	}
	for (i = 0; i < 7; i++) {
		v[i].color = col;
		v[i].tex_coord.x = v[i].tex_coord.y = 0;
	}
	for (i = 0; i < 6; i++) {
		idx[i * 3 + 0] = 0;
		idx[i * 3 + 1] = 1 + i;
		idx[i * 3 + 2] = 1 + (i + 1) % 6;
	}
	SDL_RenderGeometry(r, NULL, v, 7, idx, 18);
}

/* The mark, drawn about its own center.
 *
 * `head` slides the dark cell back along the lattice: 1.0 is where it sits at
 * rest, 0.0 is home on the center. `dim` then fades that same dark over the
 * blue center cell, 0 to 1.
 *
 * The two are separate because at head = 0 the dark cell lands exactly on the
 * center and is drawn UNDER the blue, so on its own the head just disappears.
 * `dim` is what makes its arrival visible: the blue is the one lit thing in
 * either mark - the same color as the boot line and the menu chrome - so
 * covering it is the light going out, and the shell closes dark. */
/* A five-pointed star, as a fan over ten alternating points. Same primitive
 * as draw_hex: SDL has no polygon fill, and SDL_RenderGeometry is already
 * how the mark is drawn. The inner radius is 0.45 rather than the 0.382 a
 * true pentagram wants - at the size this is drawn, the geometric answer
 * reads as spindly. */
static void draw_star(SDL_Renderer *r, float cx, float cy, float rad,
                      SDL_Color col)
{
	SDL_Vertex v[11];
	int idx[30], i;

	v[0].position.x = cx; v[0].position.y = cy;
	for (i = 0; i < 10; i++) {
		float a = (float)(-M_PI / 2.0 + i * M_PI / 5.0);
		float rr = (i & 1) ? rad * 0.45f : rad;
		v[i + 1].position.x = cx + rr * cosf(a);
		v[i + 1].position.y = cy + rr * sinf(a);
	}
	for (i = 0; i < 11; i++) {
		v[i].color = col;
		v[i].tex_coord.x = v[i].tex_coord.y = 0;
	}
	for (i = 0; i < 10; i++) {
		idx[i * 3 + 0] = 0;
		idx[i * 3 + 1] = 1 + i;
		idx[i * 3 + 2] = 1 + (i + 1) % 10;
	}
	SDL_RenderGeometry(r, NULL, v, 11, idx, 30);
}

static void draw_shell(SDL_Renderer *r, float cx, float cy, float rad,
                       float head, float dim, Uint8 alpha)
{
	/* Mirrors tools/markdef.py, which is the definition. C cannot import it,
	 * so this is the one hand-kept copy: change one, change both. The ring
	 * alternates light, mid, light, mid, light, mid for three-fold symmetry. */
	static const struct { float i, j; Uint8 c[3]; } cells[] = {
		{ -0.5f, -1.0f, { 128, 176, 118 } }, {  0.5f, -1.0f, { 104, 138,  96 } },
		{  1.0f,  0.0f, { 128, 176, 118 } }, {  0.5f,  1.0f, { 104, 138,  96 } },
		{ -0.5f,  1.0f, { 128, 176, 118 } }, { -1.0f,  0.0f, { 104, 138,  96 } },
	};
	float dx = 1.7320508f * rad, dy = 1.5f * rad;
	SDL_Color col;
	int k;

	/* Head first, so the shell cells draw over it as it withdraws. */
	col.r = 61; col.g = 89; col.b = 67; col.a = alpha;
	draw_hex(r, cx + 2.0f * dx * head, cy, rad * 0.95f, col);
	for (k = 0; k < 6; k++) {
		col.r = cells[k].c[0]; col.g = cells[k].c[1]; col.b = cells[k].c[2];
		col.a = alpha;
		draw_hex(r, cx + cells[k].i * dx, cy + cells[k].j * dy, rad * 0.95f, col);
	}
	/* markdef.CYAN, and the same value as MENU_ACCENT below. */
	col.r = 61; col.g = 214; col.b = 255; col.a = alpha;
	draw_hex(r, cx, cy, rad * 0.95f, col);
	if (dim > 0.0f) {
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		col.r = 61; col.g = 89; col.b = 67;
		col.a = (Uint8)(alpha * (dim > 1.0f ? 1.0f : dim));
		draw_hex(r, cx, cy, rad * 0.95f, col);
	}
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
	int cx = TORTOS_SCREEN_W - 34, cy = 34, rad = 9;
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
		SDL_Rect band = { 0, TORTOS_SCREEN_H - 240, TORTOS_SCREEN_W, 480 };
		ui_glow(r, &band, a->tint, 34, 1.7f);
	}
}

static void draw_systems(app *a)
{
	SDL_Rect focus;
	const system_cfg *s = &a->sys.systems[a->sys_cursor];
	char line[128];

	cf_focus_rect(&CF_LAYOUT_SYSTEMS, TORTOS_SCREEN_W, TORTOS_SCREEN_H, &focus);
	ui_glow(a->r, &focus, s->accent, 110, 2.4f);
	cf_draw(&a->cf_sys, a->r, TORTOS_SCREEN_W, TORTOS_SCREEN_H, a->sys.count,
	        sys_get_tex, a, &CF_LAYOUT_SYSTEMS);

	if (a->view[a->sys_cursor].list.count > 0)
		snprintf(line, sizeof line, "%d games", a->view[a->sys_cursor].list.count);
	else
		snprintf(line, sizeof line, "no games in Roms/%s", s->folder);
	ui_text(a->r, ui_font(UI_F_META), line, TORTOS_SCREEN_W / 2, 690, 0, UI_TEXT_DIM);
	ui_rail(a->r, TORTOS_SCREEN_W, TORTOS_SCREEN_H, a->sys_cursor, a->sys.count,
	        s->accent);
}

static void draw_games(app *a)
{
	sysview *v = &a->view[a->sys_cursor];
	const system_cfg *s = &a->sys.systems[a->sys_cursor];
	/* On Favorites the focused game's own system, so its accent, its state
	 * and its favorite key all come from the console it belongs to. */
	const system_cfg *gs = &a->sys.systems[
		shelf_owner(a, a->sys_cursor, v->cursor)];
	SDL_Rect focus;
	char count[64];

	cf_focus_rect(&CF_LAYOUT_GAMES, TORTOS_SCREEN_W, TORTOS_SCREEN_H, &focus);
	ui_glow(a->r, &focus, s->accent, 100, 2.3f);
	cf_draw(&v->cf, a->r, TORTOS_SCREEN_W, TORTOS_SCREEN_H, v->list.count,
	        game_get_tex, a, &CF_LAYOUT_GAMES);
	evict_far(v, TEX_KEEP_NEAR);

	if (v->list.count > 0) {
		game_entry *g = &v->list.items[v->cursor];
		int tw = ui_text_width(ui_font(UI_F_TITLE), g->title);
		int tx = TORTOS_SCREEN_W / 2;
		/* A game with an autosave gets a dot in the system's color beside
		 * its name: pressing A on it does not start it, it continues it.
		 * Sized and centered off the title's own line, so it keeps sitting
		 * with the text when the type scale moves. */
		if (game_has_state(a, shelf_owner(a, a->sys_cursor, v->cursor), g)) {
			TTF_Font *ft = ui_font(UI_F_TITLE);
			int line = ui_font_line(UI_F_TITLE);
			int rad = line / 8, dx = tx - tw / 2 - rad * 3;
			/* Same ink-centering as the star opposite it. This sat on the em
			 * box's middle, which is a descender's depth below the letters,
			 * and next to a mark that was centered properly the two read as
			 * misaligned rather than as a pair. */
			int dy = 40 + line / 2 + (ft ? TTF_FontDescent(ft) / 2 : 0);
			SDL_SetRenderDrawColor(a->r, (Uint8)(gs->accent >> 16),
			                       (Uint8)(gs->accent >> 8), (Uint8)gs->accent, 255);
			for (int k = -rad; k <= rad; k++) {
				int w = (int)(sqrt((double)(rad * rad - k * k)) + 0.5);
				SDL_RenderDrawLine(a->r, dx - w, dy + k, dx + w, dy + k);
			}
		}
		/* On the right, because the autosave dot already owns the left. The
		 * two say different things - one is "this is where you left off", the
		 * other "you chose this" - and a reader should not have to work out
		 * which mark is which by its shape alone. */
		if (fav_is(gs->tag, g->file)) {
			TTF_Font *ft = ui_font(UI_F_TITLE);
			int line = ui_font_line(UI_F_TITLE);
			int rad = line / 4;
			/* Centered on the title's INK, not its em box. The box reserves a
			 * descender's depth that most titles never use, so a mark placed
			 * at the box's middle sits visibly below the letters. Descent is
			 * negative, so half of it lifts. The autosave dot opposite gets
			 * the same correction, so the pair sits on one line. */
			int dy = 40 + line / 2 + (ft ? TTF_FontDescent(ft) / 2 : 0);
			SDL_Color c = { (Uint8)(gs->accent >> 16), (Uint8)(gs->accent >> 8),
			                (Uint8)gs->accent, 255 };
			draw_star(a->r, (float)(tx + tw / 2 + rad * 2), (float)dy,
			          (float)rad, c);
		}
		ui_text(a->r, ui_font(UI_F_TITLE), g->title, tx, 40, 0, UI_TEXT);
		snprintf(count, sizeof count, "%d / %d", v->cursor + 1, v->list.count);
		ui_text(a->r, ui_font(UI_F_META), count, TORTOS_SCREEN_W / 2, 690, 0,
		        UI_TEXT_DIM);
	} else {
		ui_text(a->r, ui_font(UI_F_TITLE), s->name, TORTOS_SCREEN_W / 2, 40, 0,
		        UI_TEXT);
	}
	ui_rail(a->r, TORTOS_SCREEN_W, TORTOS_SCREEN_H, v->cursor, v->list.count,
	        s->accent);
}

/* The shelf, without presenting it: the options menu draws over a live one, so
 * the cards keep their tint easing behind the panel rather than freezing into
 * a still. */
/* Nothing on the card at all.
 *
 * Hiding empty systems has an edge that hiding one system does not: if every
 * system is empty the shelf is simply blank, and a blank screen is what a
 * broken launcher looks like. Someone who has just written a card, or put the
 * ROMs one directory too deep, needs to be told where the games go rather
 * than left to conclude the device is dead. The path is named because that is
 * the actual question being asked. */
static void draw_no_games(app *a)
{
	int cy = TORTOS_SCREEN_H / 2;

	ui_text(a->r, ui_font(UI_F_TITLE), "No games found",
	        TORTOS_SCREEN_W / 2, cy - 60, 0, UI_TEXT_SOFT);
	ui_text(a->r, ui_font(UI_F_MENU), "Put ROMs in Roms/<System>/ on the card",
	        TORTOS_SCREEN_W / 2, cy + 6, 0, UI_TEXT_DIM);
	ui_text(a->r, ui_font(UI_F_META), "one folder per system, named as in systems.cfg",
	        TORTOS_SCREEN_W / 2, cy + 56, 0, UI_TEXT_DIM);
}

static void draw_shelf(app *a)
{
	draw_background(a);
	if (a->sys.count <= 0) draw_no_games(a);
	else if (a->screen == SCREEN_SYSTEMS) draw_systems(a);
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

	cf_focus_rect(&CF_LAYOUT_GAMES, TORTOS_SCREEN_W, TORTOS_SCREEN_H, &from);
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
			dst.x = TORTOS_SCREEN_W / 2 - dst.w / 2;
			dst.y = TORTOS_SCREEN_H / 2 - dst.h / 2;
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

/* The send-off, and the inverse of the boot animation's gesture: there he
 * launches head-first out of frame, here he arrives and pulls the head in.
 *
 * Timed against the shutdown rather than chosen. Measured 2026-08-28: 1.42 s
 * from the Power off press to adbd dying, of which the old 620 ms animation
 * was the first slice - the rest was a black screen while sync and the kernel
 * finished. So the animation runs to about 900 ms and then DOES NOT clear.
 *
 * Not clearing is the point. Whatever was last presented stays on the panel
 * until the kernel cuts it, so the mark sits there through the remainder of
 * the shutdown and the screen going dark is the device going dark. The boot
 * animation relies on exactly the same thing at the other end. */
static void anim_poweroff(app *a)
{
	const unsigned T_IN = 430, T_HEAD = 260, T_DIM = 210;
	const unsigned ms = T_IN + T_HEAD + T_DIM;
	const float cx = TORTOS_SCREEN_W / 2.0f, cy = TORTOS_SCREEN_H / 2.0f;
	const float rad = 46.0f;
	unsigned t0 = plat_now_ms(), now;

	while ((now = plat_now_ms()) - t0 < ms) {
		unsigned t = now - t0;
		float x = cx, head = 1.0f, dim = 0.0f;
		if (t < T_IN) {
			/* In from the left, decelerating onto the center. */
			float k = (float)t / (float)T_IN;
			float e = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
			x = -TORTOS_SCREEN_W * 0.35f + (cx + TORTOS_SCREEN_W * 0.35f) * e;
		} else if (t < T_IN + T_HEAD) {
			float k = (float)(t - T_IN) / (float)T_HEAD;
			head = 1.0f - k * k;          /* accelerating in, like a flinch */
		} else {
			float k = (float)(t - T_IN - T_HEAD) / (float)T_DIM;
			head = 0.0f;
			dim = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
		}
		SDL_SetRenderDrawColor(a->r, 17, 19, 16, 255);
		SDL_RenderClear(a->r);
		draw_shell(a->r, x, cy, rad, head, dim, 255);
		SDL_RenderPresent(a->r);
		SDL_Delay(6);
	}
	/* Land on the closed state exactly, in case the loop exited a frame early,
	 * and then deliberately no clear: the dark shell is the last thing on
	 * screen and stays there while the device powers down. */
	SDL_SetRenderDrawColor(a->r, 17, 19, 16, 255);
	SDL_RenderClear(a->r);
	draw_shell(a->r, cx, cy, rad, 0.0f, 1.0f, 255);
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

/* Both menus in TortOS are the same shape - a short list on a slab, over a
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
/* The system's one accent: this menu, the volume OSD, and the mark's center
 * cell in both animations. Hand-kept equal to markdef.CYAN and UI_CYAN_*. */
#define MENU_ACCENT 0x3DD6FFu

/* The unit every menu measurement is in. Row height, padding and the gap
 * between the two columns are all cut from it, so the whole panel scales with
 * the type rather than with a set of numbers that have to be retuned together. */
static int menu_row_h(void) { return ui_font_line(UI_F_MENU) * 3 / 2; }

/* `fixed_w` is the content width to use, or 0 to size to these rows. The shelf
 * menus pass a width measured across both of them so the panel never resizes;
 * the in-game menu has no values to cycle and sizes to itself. */
/* `accent` is the panel's border and heading rule. The shelf's own menu passes
 * MENU_ACCENT because that menu is TortOS, not whichever card is under the
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
	if (content_w > TORTOS_SCREEN_W - margin * 2 - pad * 2)
		content_w = TORTOS_SCREEN_W - margin * 2 - pad * 2;

	if (content_off + n * row_h + pad > TORTOS_SCREEN_H - margin * 2) {
		vis = (TORTOS_SCREEN_H - margin * 2 - content_off - pad) / row_h;
		if (vis < 1) vis = 1;
		if (vis > n) vis = n;
		first = sel - vis / 2;
		if (first < 0) first = 0;
		if (first > n - vis) first = n - vis;
	}

	panel.w = content_w + pad * 2;
	panel.h = content_off + vis * row_h + pad;
	panel.x = (TORTOS_SCREEN_W - panel.w) / 2;
	panel.y = (TORTOS_SCREEN_H - panel.h) / 2;
	cx = panel.x + panel.w / 2;
	content_x = panel.x + pad;
	content_y = panel.y + content_off;

	/* TortOS's own color, not the focused system's. The menu belongs to the
	 * launcher rather than to whatever card happens to be under the cursor, so
	 * the boot animation, the mark and this chrome are one accent: the device's
	 * first frame and the shelf agree.
	 *
	 * They did not always. The mark carried its own blue, (74,158,255), while
	 * this was (61,214,255), and comments in both files called them the same
	 * color without either having been checked against the other. Unified on
	 * this cyan on 2026-08-28, that being the direction that stays clear of the
	 * nine system accents; the mark's old blue sat close to Genesis. */
	ui_glow(a->r, &panel, accent, 60, 1.5f);
	ui_panel(a->r, &panel, MENU_RADIUS, accent);

	if (heading) {
		/* Centered in the band by its ink, on the same reasoning as the rows:
		 * the em box carries descender depth that "TortOS" and "NES" do not use.
		 * This read "mostly do not" while the heading was "PlayOS", whose y was
		 * the exception; the shift is by font metrics rather than by the string,
		 * so nothing here changed with the name, it just got exactly true. */
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
		 * working row as well left the entire menu reading as grayed out. */
		if (rows[i].live) lc = i == sel ? UI_TEXT      : UI_TEXT_SOFT;
		else              lc = i == sel ? UI_TEXT_SOFT : UI_TEXT_DIM;
		/* The panel's own accent, not a->tint. These were the same value for
		 * as long as Display mode was the only live row, because that row is
		 * in the SYSTEM menu where the accent IS the system tint. The first
		 * live row in the TortOS menu made them diverge and drew a red value
		 * inside a cyan panel. `accent` is already MENU_ACCENT for one menu
		 * and the system tint for the other, which is the answer in both. */
		vc = rows[i].live && i == sel
		     ? (SDL_Color){ (Uint8)(accent >> 16), (Uint8)(accent >> 8),
		                    (Uint8)accent, 255 }
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
 * to hold the shape of what TortOS grows into, and a row that is drawn but
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
	*heading = "TortOS";
	{	/* Reported rather than remembered: the radio can be brought up or
		 * dropped from outside the launcher, so asking is the only answer that
		 * is true when it is drawn.
		 *
		 * Cached for two seconds, on the same reasoning as battery_low(), and
		 * for a sharper reason: menu_build runs every frame and wifi_status
		 * forks wpa_cli, so the first version of this was spawning a process
		 * about 125 times a second on a 1GHz device for a line that changes
		 * a few times an hour.
		 *
		 * The network's name, not its address. A settings row should say what
		 * the setting IS; the address is a fact about the machine and lives on
		 * the About page with the other ones. */
		static unsigned next_check;
		static char cached[CFG_STR];
		unsigned now = plat_now_ms();

		if (next_check == 0 || now >= next_check) {
			char ss[WIFI_SSID_MAX], ip[64];
			wifi_state ws = wifi_status(ss, sizeof ss, ip, sizeof ip);
			next_check = now + 2000;
			(void)ip;
			if (ws == WIFI_CONNECTED && ss[0]) snprintf(cached, sizeof cached, "%s", ss);
			else snprintf(cached, sizeof cached, "%s",
			              ws == WIFI_CONNECTING ? "connecting" :
			              ws == WIFI_IDLE       ? "not connected" : "off");
		}
		snprintf(b->b, sizeof b->b, "%s", cached);
		out[PM_WIFI]     = (menu_row){ "Wi-Fi",             b->b,      true  };
	}
	out[PM_BT]           = (menu_row){ "Bluetooth",         "not yet", false };
	out[PM_ACHIEVEMENTS] = (menu_row){ "RetroAchievements",
	                                   ra_signed_in() ? ra_user() : "sign in",
	                                   true };
	out[PM_SCRAPE]       = (menu_row){ "Box art scraping",  "not yet", false };
	out[PM_TEXT]         = (menu_row){ "Text size",         b->a,      true  };
	out[PM_SLEEP]        = (menu_row){ "Sleep timer",       "not yet", false };
	out[PM_ABOUT]        = (menu_row){ "About TortOS",      NULL,      true  };
	out[PM_POWER]        = (menu_row){ "Power off",         NULL,      true  };
	return PM_ROWS;
}


/* ---------- the WiFi screen ----------------------------------------------- */

/* One frame with a single line on it, drawn before each blocking call.
 *
 * wifi_up can take twenty seconds waiting out the stock service's retry loop,
 * a scan takes several, and an association takes up to twenty more. None of
 * that is asynchronous here, so the loop is not running and input is not read
 * while it happens. Painting the reason first is the difference between a
 * device that is working and a device that has hung: the picture is identical
 * otherwise, and the second reading is the one that gets a power cycle. */
/* One line on a panel over the shelf, for the moments something is happening
 * and there is nothing yet to show. The heading is a parameter because it was
 * not: this began as the Wi-Fi screen's own and said "Wi-Fi" over every
 * message it was given, so signing in to RetroAchievements and fetching a set
 * both announced themselves as Wi-Fi. */
static void wait_panel(app *a, const char *heading, const char *msg)
{
	menu_row row = { msg, NULL, false };

	draw_shelf(a);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
	menu_draw(a, heading, &row, 1, -1, 0, MENU_ACCENT);
	plat_draw_osd(a->r);
	SDL_RenderPresent(a->r);
}

/* Signal as a word. dBm is the honest number and it is also jargon; the list
 * is sorted strongest first anyway, so the word only has to separate "this
 * will work" from "this will not". */
static const char *wifi_strength(int dbm)
{
	if (dbm >= -60) return "strong";
	if (dbm >= -72) return "good";
	return "weak";
}

static void wifi_backdrop(void *ctx)
{
	app *a = ctx;
	draw_shelf(a);
	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 150);
	SDL_RenderFillRect(a->r, NULL);
}

static void wifi_screen(app *a)
{
	wifi_net nets[WIFI_MAX_NETS];
	char vals[WIFI_MAX_NETS][32];
	menu_row rows[WIFI_MAX_NETS + 1];
	char status[96], ssid[WIFI_SSID_MAX], ip[64];
	int nrows;
	int n = 0, sel = 0, i;
	bool done = false, rescan = false, on;

	/* Entering does not switch the radio on. It used to, which made the
	 * screen impossible to leave in the off state: you opened it to turn
	 * wifi OFF and the act of opening it turned wifi on. */
	on = wifi_status(NULL, 0, NULL, 0) != WIFI_OFF;
	if (on) rescan = true;

	while (!done && !want_quit && a->running) {
		if (rescan) {
			wait_panel(a, "Wi-Fi", "Scanning...");
			char cur[WIFI_SSID_MAX];
			n = wifi_scan(nets, WIFI_MAX_NETS);
			if (n < 0) n = 0;
			if (wifi_status(cur, sizeof cur, NULL, 0) != WIFI_CONNECTED)
				cur[0] = '\0';
			for (i = 0; i < n; i++)
				/* "connected" outranks "saved": after a successful connect the
				 * list was redrawn saying only that the network was known,
				 * which is what it said before the connect too, so the screen
				 * gave no sign anything had happened. */
				snprintf(vals[i], sizeof vals[i], "%s%s",
				         wifi_strength(nets[i].signal),
				         (cur[0] && !strcmp(cur, nets[i].ssid)) ? " - connected"
				         : nets[i].known ? " - saved"
				                         : nets[i].secured ? "" : " - open");
			if (sel >= n) sel = n ? n - 1 : 0;
			rescan = false;
		}

		/* The switch is row 0 and the networks follow it, so the state of the
		 * radio is the first thing read and the first thing reachable. */
		rows[0] = (menu_row){ "Wi-Fi", on ? "on" : "off", true };
		for (i = 0; i < n; i++)
			rows[i + 1] = (menu_row){ nets[i].ssid, vals[i], true };
		nrows = n + 1;
		if (!on)
			rows[nrows++] = (menu_row){ "Turn Wi-Fi on to scan", NULL, false };
		else if (n == 0)
			rows[nrows++] = (menu_row){ "No networks found", NULL, false };

		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; return; }
		if (a->in.pressed[IN_POWER]) { power_off(a); return; }

		if (in_repeat(&a->in, IN_UP))   sel = (sel + nrows - 1) % nrows;
		if (in_repeat(&a->in, IN_DOWN)) sel = (sel + 1) % nrows;
		if (a->in.pressed[IN_Y] && on) rescan = true;
		if (a->in.pressed[IN_BACK] || a->in.pressed[IN_MENU]) done = true;

		/* The switch. Saved on every change rather than on the way out: the
		 * way out of a handheld is often the power button. */
		if (a->in.pressed[IN_ACCEPT] && sel == 0) {
			if (on) {
				wifi_down();
				wifi_pref_save(false);
				on = false;
				n = 0;
			} else {
				wait_panel(a, "Wi-Fi", "Turning Wi-Fi on...");
				on = wifi_up();
				wifi_pref_save(on);
				if (!on) {
					wait_panel(a, "Wi-Fi", "Wi-Fi did not come up");
					SDL_Delay(1800);
				}
				rescan = on;
			}
			continue;
		}

		if (a->in.pressed[IN_ACCEPT] && sel >= 1 && sel <= n) {
			int k = sel - 1;
			char psk[80] = "";
			bool ok;

			/* A saved network already has its passphrase in
			 * wpa_supplicant.conf, so asking again would be asking the user
			 * to retype something the device is holding. An open network has
			 * none to ask for. */
			if (nets[k].secured && !nets[k].known) {
				kb_result kr = kb_prompt(a->r, &a->in, nets[k].ssid,
				                         psk, (int)sizeof psk, MENU_ACCENT,
				                         wifi_backdrop, a);
				if (kr == KB_POWER) { power_off(a); return; }
				if (kr != KB_ACCEPT) continue;
			}

			wait_panel(a, "Wi-Fi", "Connecting...");
			ok = wifi_connect(nets[k].ssid, psk[0] ? psk : NULL);
			/* Wiped as soon as it has been handed over. It still exists in
			 * the supplicant's config, which is the point, but there is no
			 * reason for a copy to sit in the launcher's stack afterwards. */
			memset(psk, 0, sizeof psk);

			if (ok) {
				/* Connecting is turning it on, whatever the switch said. */
				wifi_pref_save(true);
				on = true;
				wifi_status(ssid, sizeof ssid, ip, sizeof ip);
				snprintf(status, sizeof status, "Connected to %s", ssid);
			} else {
				snprintf(status, sizeof status,
				         "Could not connect to %s", nets[k].ssid);
			}
			wait_panel(a, "Wi-Fi", status);
			SDL_Delay(1800);
			/* Out on success, because the job is done and the menu row behind
			 * this screen already names the network - staying in the list
			 * makes you back out by hand to see the result. On failure stay,
			 * because the next thing wanted is another try or another
			 * network, and both are here. */
			if (ok) { done = true; continue; }
			rescan = true;
			continue;
		}

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, "Wi-Fi", rows, nrows, sel, 0, MENU_ACCENT);
		plat_draw_osd(a->r);
		SDL_RenderPresent(a->r);
		SDL_Delay(8);
	}
}


/* ---------- About --------------------------------------------------------- */

#ifndef TORTOS_VERSION
#define TORTOS_VERSION "0.0"       /* set by the makefiles from Makefile's VERSION */
#endif

/* Facts about the machine, which is a different thing from settings. The
 * address in particular has nowhere else to live: the Wi-Fi screen names the
 * network, and short of asking the router there is no way to find out what
 * address the device took. */
/* Signing in to RetroAchievements. Two prompts and a request; the account
 * screen is deliberately not a screen, because there is nothing to look at
 * until there is something to say.
 *
 * The password is used to get a token and then wiped. Nothing stores it -
 * ra.cfg holds the token, which is what every later call uses anyway. */
static void ra_signin_screen(app *a)
{
	char user[RA_USER_MAX] = "", pass[96] = "", err[160] = "";
	char path[CFG_STR * 2];
	menu_row row;
	kb_result kr;
	bool ok;

	snprintf(user, sizeof user, "%s", ra_user());

	if (!ra_online()) {
		row = (menu_row){ "Not on a network. Connect Wi-Fi first.", NULL, false };
		wifi_backdrop(a);
		menu_draw(a, "RetroAchievements", &row, 1, -1, 0, MENU_ACCENT);
		plat_draw_osd(a->r);
		SDL_RenderPresent(a->r);
		SDL_Delay(1600);
		return;
	}

	kr = kb_prompt(a->r, &a->in, "RetroAchievements user", user,
	               (int)sizeof user, MENU_ACCENT, wifi_backdrop, a);
	if (kr == KB_POWER) { power_off(a); return; }
	if (kr != KB_ACCEPT || !user[0]) return;

	kr = kb_prompt(a->r, &a->in, "Password", pass,
	               (int)sizeof pass, MENU_ACCENT, wifi_backdrop, a);
	if (kr == KB_POWER) { memset(pass, 0, sizeof pass); power_off(a); return; }
	if (kr != KB_ACCEPT || !pass[0]) { memset(pass, 0, sizeof pass); return; }

	wait_panel(a, "RetroAchievements", "Signing in...");
	ok = ra_sign_in(user, pass, err, sizeof err);
	/* Wiped the moment it has been used, the same as the Wi-Fi passphrase.
	 * The token it bought is the thing worth keeping. */
	memset(pass, 0, sizeof pass);

	if (ok) {
		ra_creds_path(path, sizeof path);
		ra_creds_save(path);
		snprintf(err, sizeof err, "Signed in as %s", ra_user());
	} else if (!err[0]) {
		snprintf(err, sizeof err, "Sign-in failed");
	}

	row = (menu_row){ err, NULL, ok };
	wifi_backdrop(a);
	menu_draw(a, "RetroAchievements", &row, 1, -1, 0, MENU_ACCENT);
	plat_draw_osd(a->r);
	SDL_RenderPresent(a->r);
	SDL_Delay(1800);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
}

static void about_screen(app *a)
{
	char ver[48], addr[80], batt[32], up[48];
	menu_row rows[4];
	char ssid[WIFI_SSID_MAX], ip[64];
	int pct = 0;
	bool charging = false;
	unsigned secs;
	bool done = false;

	snprintf(ver, sizeof ver, "%s", TORTOS_VERSION);

	while (!done && !want_quit && a->running) {
		static unsigned next_check;
		unsigned now = plat_now_ms();

		/* Same two-second cache as the menu row, and for the same reason:
		 * this loop runs every frame and wifi_status forks wpa_cli. */
		if (next_check == 0 || now >= next_check) {
			wifi_state ws = wifi_status(ssid, sizeof ssid, ip, sizeof ip);
			next_check = now + 2000;
			if (ws == WIFI_CONNECTED && ip[0])
				snprintf(addr, sizeof addr, "%s", ip);
			else if (ws == WIFI_CONNECTED)
				snprintf(addr, sizeof addr, "no address yet");
			else
				snprintf(addr, sizeof addr, "not connected");
			if (plat_battery(&pct, &charging))
				snprintf(batt, sizeof batt, "%d%%%s", pct,
				         charging ? " charging" : "");
			else
				snprintf(batt, sizeof batt, "unknown");
		}
		secs = now / 1000;
		snprintf(up, sizeof up, "%uh %02um", secs / 3600, (secs / 60) % 60);

		rows[0] = (menu_row){ "Version",   ver,  false };
		rows[1] = (menu_row){ "Address",   addr, false };
		rows[2] = (menu_row){ "Battery",   batt, false };
		rows[3] = (menu_row){ "Awake for", up,   false };

		plat_input_poll(&a->in);
		if (a->in.quit_requested) { a->running = false; return; }
		if (a->in.pressed[IN_POWER]) { power_off(a); return; }
		if (a->in.pressed[IN_BACK] || a->in.pressed[IN_MENU]) done = true;

		if (in_repeat(&a->in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a->in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a->in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a->in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		tick_tint(a);
		draw_shelf(a);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, "About TortOS", rows, 4, -1, 0, MENU_ACCENT);
		plat_draw_osd(a->r);
		SDL_RenderPresent(a->r);
		SDL_Delay(8);
	}
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

static void tortos_menu_draw(app *a, int sel)
{
	menu_row rows[MENU_MAX_ROWS];
	menu_bufs bufs;
	const char *heading;
	int n = menu_build(a, a->screen, a->sys_cursor, rows, &bufs, &heading);

	SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(a->r, 0, 0, 0, 120);
	SDL_RenderFillRect(a->r, NULL);
	/* The shelf's menu is TortOS's own on the systems screen and a system's on
	 * a game list, which is where it gains rows that belong to that system. */
	menu_draw(a, heading, rows, n, sel, menu_shelf_width(a),
	          a->screen == SCREEN_SYSTEMS ? MENU_ACCENT : a->tint);
}

static void tortos_menu(app *a)
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
		/* Text size, on the same left/right idiom as Display mode. Changing it
		 * reopens every font, so the whole UI is rebuilt: the panel's cached
		 * width is measured from font metrics, and any card generated for a
		 * game with no box art has its title baked in at the old size. Both
		 * are dropped here rather than left to look subtly wrong. */
		if (a->screen == SCREEN_SYSTEMS && sel == PM_TEXT) {
			int d = in_repeat(&a->in, IN_RIGHT) ? 1
			      : in_repeat(&a->in, IN_LEFT)  ? -1 : 0;
			if (d) {
				int k = text_scale_step() + d;
				if (k < 0) k = 0;
				if (k >= TEXT_SCALE_COUNT) k = TEXT_SCALE_COUNT - 1;
				if (TEXT_SCALES[k] != ui_get_font_scale()) {
					free_all_textures(a);
					ui_quit();
					ui_set_font_scale(TEXT_SCALES[k]);
					ui_init(a->r, P_FONT);
					a->menu_w = 0;      /* fonts reopened: remeasure the panel */
					prime_sys_window(a);
					text_scale_save(TEXT_SCALES[k]);
				}
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
		if (a->in.pressed[IN_ACCEPT] && a->screen == SCREEN_SYSTEMS &&
		    sel == PM_WIFI) wifi_screen(a);
		if (a->in.pressed[IN_ACCEPT] && a->screen == SCREEN_SYSTEMS &&
		    sel == PM_ACHIEVEMENTS) ra_signin_screen(a);
		if (a->in.pressed[IN_ACCEPT] && a->screen == SCREEN_SYSTEMS &&
		    sel == PM_ABOUT) about_screen(a);

		tick_tint(a);
		draw_shelf(a);
		tortos_menu_draw(a, sel);
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
	GM_CONTINUE, GM_SAVE, GM_LOAD, GM_DISPLAY, GM_CHEEVOS, GM_RESET, GM_QUIT,
	GM_ROWS
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
	const SDL_Rect area = { (TORTOS_SCREEN_W - 700) / 2, 129, 700, 451 };
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
	 * the heading is centered in the gap above the frame. That fixes every
	 * number here to one another rather than to taste, so changing the image
	 * size moves the rest to match instead of drifting into something. */
	ui_text(a->r, ui_font(UI_F_LABEL), sv->saving ? "Save to" : "Load from",
	        TORTOS_SCREEN_W / 2, 39, 0, UI_TEXT_DIM);

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
	        TORTOS_SCREEN_W / 2, img.y + img.h + 36, 0, UI_TEXT);
	ui_text(a->r, ui_font(UI_F_META),
	        sv->have[sel] ? sv->when[sel] : (sv->saving ? "\xE2\x80\x94" : ""),
	        TORTOS_SCREEN_W / 2, img.y + img.h + 42 + line_menu, 0, UI_TEXT_DIM);

	/* The dot rail: where you are among the slots, without showing every
	 * picture.
	 * A hollow-dim dot is a slot you cannot land on. */
	{
		int dots = GM_SLOTS + 1, dw = 36;
		int x0 = (TORTOS_SCREEN_W - dots * dw) / 2 + dw / 2;
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
		"Continue", "Save", "Load", "Display", "Achievements", "Reset", "Quit"
	};
	/* Static because menu_row holds a pointer, not a copy, and the row has to
	 * outlive this function. */
	static char cheevo_val[24];
	int i;

	for (i = 0; i < GM_ROWS; i++) out[i] = (menu_row){ label[i], NULL, true };
	out[GM_DISPLAY].value = DMODES[a->view[a->sys_cursor].dmode].label;

	/* Most of a library has no set, and a row that says so plainly is better
	 * than one that is missing: "none" answers the question the player opened
	 * the menu to ask. Drawn quiet, and does nothing when chosen. */
	if (chv_count() > 0) {
		snprintf(cheevo_val, sizeof cheevo_val, "%d / %d",
		         chv_earned(), chv_count());
	} else {
		snprintf(cheevo_val, sizeof cheevo_val, "none");
		out[GM_CHEEVOS].live = false;
	}
	out[GM_CHEEVOS].value = cheevo_val;
}

/* The list itself, over the paused frame. menu_draw already windows a list
 * longer than the screen, which a set of 166 certainly is.
 *
 * Earned rows are drawn live and unearned quiet - the same distinction
 * menu_draw makes for a placeholder, and it reads correctly here: what you
 * have is bright, what is still ahead of you is not. */
static void cheevos_screen(app *a, SDL_Texture *bg)
{
	int n = chv_count(), sel = 0, i, done = 0;
	menu_row *rows;
	char (*vals)[16];
	char heading[192];

	if (n <= 0) return;
	rows = calloc((size_t)n, sizeof *rows);
	vals = calloc((size_t)n, sizeof *vals);
	if (!rows || !vals) { free(rows); free(vals); return; }

	for (i = 0; i < n; i++) {
		const cheevo *c = chv_at(i);

		snprintf(vals[i], sizeof vals[i], "%d", c->points);
		rows[i].label = c->title;
		rows[i].value = vals[i];
		rows[i].live  = c->earned || c->earned_now;
	}
	/* The counts first, and the title only if it fits.
	 *
	 * menu_draw sizes its panel to the widest ROW and clamps to the screen, so
	 * a heading wider than that is centred into a clip - which took both ends
	 * off "Hagane: The Final Conflict   0/36   0/415 points", losing the H and
	 * the word "points". The numbers are what the screen is for and must never
	 * be the part that goes; the game's name is the part you already know,
	 * since you are inside it. */
	{
		TTF_Font *fh = ui_font(UI_F_LABEL);
		int budget = TORTOS_SCREEN_W - menu_row_h() * 3 - 48;
		char counts[64];

		snprintf(counts, sizeof counts, "%d/%d   %d/%d points",
		         chv_earned(), n, chv_points_earned(), chv_points_total());
		snprintf(heading, sizeof heading, "%s   %s", chv_game_title(), counts);
		if (ui_text_width(fh, heading) > budget)
			snprintf(heading, sizeof heading, "%s", counts);
	}

	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);

	while (!done && !want_quit) {
		plat_input_poll(&a->in);

		if (in_repeat(&a->in, IN_UP))   sel = (sel + n - 1) % n;
		if (in_repeat(&a->in, IN_DOWN)) sel = (sel + 1) % n;
		/* A set runs to well over a hundred entries, so the shoulder buttons
		 * page it the same way they page a shelf. */
		if (in_repeat(&a->in, IN_L1))   sel = sel > 8 ? sel - 8 : 0;
		if (in_repeat(&a->in, IN_R1))   sel = sel < n - 9 ? sel + 8 : n - 1;
		if (a->in.pressed[IN_BACK] || a->in.pressed[IN_MENU] ||
		    a->in.pressed[IN_ACCEPT])
			done = 1;
		/* Power still stops the game from in here. Not trapping it would
		 * make this screen the one place in the launcher that ignores it. */
		if (a->in.pressed[IN_POWER]) {
			plat_resident_line("STOP");
			done = 1;
		}

		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 255);
		SDL_RenderClear(a->r);
		draw_paused_frame(a, bg);
		SDL_SetRenderDrawBlendMode(a->r, SDL_BLENDMODE_BLEND);
		SDL_SetRenderDrawColor(a->r, 0, 0, 0, 185);
		SDL_RenderFillRect(a->r, NULL);
		menu_draw(a, heading, rows, n, sel, 0, a->tint);
		SDL_RenderPresent(a->r);
		SDL_Delay(8);
	}

	free(rows);
	free(vals);
	plat_input_flush();
	memset(&a->in, 0, sizeof a->in);
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
			case GM_CHEEVOS:
				cheevos_screen(a, bg);
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
	/* The game's system, not the shelf's. Launching a favorite off the
	 * Favorites shelf otherwise loads whatever core the shelf claims, which
	 * is none, and looks for the ROM under a folder that does not exist. */
	int o = shelf_owner(a, a->sys_cursor, v->cursor);
	const system_cfg *s = &a->sys.systems[o];
	char core[CFG_STR * 2], elf[CFG_STR * 2], rom[LIB_PATH * 2];
	char st[LIB_PATH * 2], pv[LIB_PATH * 2];
	char save[CFG_STR * 2], bios[CFG_STR * 2];
	char active[LIB_PATH * 2] = "";
	char set[LIB_PATH * 2];
	int  console = 0;
	bool first_play = false;
	/* 18 fixed entries plus NULL, then two per core option. Sized off the
	 * loader's own cap so the two cannot drift apart: the previous 20 was
	 * already 18 full, and a silent bound check would have dropped every
	 * option rather than failing loudly. */
	char *argv[20 + 2 * 32];
	bool resident = false, want_menu;
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
	state_path(a, o, &v->list.items[v->cursor], st, sizeof st);
	preview_path(a, a->sys_cursor, &v->list.items[v->cursor], pv, sizeof pv);
	persist_dir_ensure(a, a->sys_cursor);

	/* Achievements, if this game has any. Most of a library does not, and that
	 * is not a failure: chv_load says so by returning false and everything
	 * below carries on with console 0 and no set, which is what Diatom reads
	 * as "this game has none". */
	{
		chv_path(P_ROMS, s->folder, v->list.items[v->cursor].name,
		         set, sizeof set);

		/* Fetch it if this game has never been played here. Only then: a
		 * cached set costs nothing and this is the launch path, so the delay
		 * is paid once per game rather than every time. Failing is ordinary -
		 * offline, not signed in, or a game RetroAchievements has never seen -
		 * and the launch carries on without. */
		/* Never played here, so there is no set to hand over. The game starts
		 * anyway and the set is found behind it - see ra_fetch_begin. Nothing
		 * in front of the launch. */
		first_play = ra_signed_in() && access(set, R_OK) != 0 && ra_online();

		if (chv_load(set)) {
			/* Reconcile with the account BEFORE deciding what to watch.
			 * Without this the launcher filters against what this device
			 * happens to have seen, which is not the same question and does
			 * not look different: measured 2026-08-29 as 3 of 40 for Contra
			 * against the site's own 13. */
			/* The account's answer, without the launch waiting for it. On a
			 * first play there is nothing to ask about yet; that sync starts
			 * once the set arrives. */
			if (!first_play) ra_sync_begin(chv_game());


			/* ra_start_session is deliberately NOT called here. It drives the
			 * "currently playing" indicator on the website and nothing on the
			 * device, and it is another 320ms request - which is the exact
			 * trade this whole path exists to refuse. */

			chv_active_path(active, sizeof active);
			if (chv_write_active(active)) {
				console = chv_console();
			} else {
				/* Everything in the set is already earned. Nothing to watch,
				 * and sending an empty file would have Diatom log a set with
				 * no achievements in it every launch. */
				active[0] = '\0';
			}
		}
	}

	remember_place(a);
	/* Before the launch, not after: a device that loses power during the load
	 * was still playing this game. */
	playing_set(a);

	/* Read and cleared together. Leaving it set would have the NEXT game
	 * launched in this session open with its menu up too, which is the sort
	 * of thing that looks like a haunting rather than a bug. */
	want_menu = a->resume_menu;
	a->resume_menu = false;

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
		if (plat_resident_send(s->tag, core, rom, st, st, pv,
		                       console, active[0] ? active : NULL)) {
			int r;

			/* Straight after RUN and the levels, and for the same reason: the
			 * mode is Diatom's own global and survives from the last game, so
			 * a system that has never been set would otherwise inherit
			 * whatever the previous one chose. Ordered on the same socket, so
			 * it lands before the first frame. */
			plat_resident_line("SETDISPLAY\tmode=%s", DMODES[v->dmode].name);

			/* The first play of this game: nothing was cached, so there is a
			 * set to go and find. Out here and not inside the chv_load branch
			 * above, which is the mistake the first version made - that
			 * branch is precisely the one a first play does not take.
			 *
			 * Hashing is local work and happens after RUN, so it overlaps the
			 * game's own startup rather than delaying it. The two requests
			 * then run behind the game and on_game_tick hands the set over
			 * when they land. */
			if (first_play) {
				char h[33];

				if (ra_hash_rom(rom, s->tag, h)) {
					chv_active_path(g_pending_active, sizeof g_pending_active);
					snprintf(g_pending_set, sizeof g_pending_set, "%s", set);
					ra_fetch_begin(h, set);
				}
			}
			/* Coming back from a shutdown: the game loads and the menu is
			 * already up, so nothing is handed control of a game the player
			 * may not have meant to resume. Ordered on the same socket, so it
			 * lands before the first frame the player could act on. */
			if (want_menu) plat_resident_line("PAUSE");
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

			/* The game is over and the display is ours again, which is the
			 * first moment it is safe to make a request: the wait loop above
			 * is the power button's watchdog, and anything blocking inside it
			 * would stop the device answering. Whatever was earned goes now;
			 * whatever will not send stays queued. */
			/* Collect first, so anything the account already had is settled
			 * before deciding what is owed - otherwise the flush would
			 * cheerfully submit a dozen duplicates. */
			ra_sync_collect_and_merge();
			ra_flush_unlocks(rom, s->tag);

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
	if (access(TORTOS_POWEROFF_FLAG, F_OK) == 0) {
		plat_leds_off();
		a->running = false;
		return;
	}
	/* Power was pressed during the game. The emulator no longer handles that
	 * key itself, so the press arrived here. */
	if (plat_run_power_pressed()) { power_off(a); return; }

	/* Past both power checks, so this is a real return to the shelf rather
	 * than a shutdown. Everything above leaves the marker standing, which is
	 * what makes the next boot able to tell them apart. */
	playing_clear();

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

	/* No systems means the modulo below divides by zero. Reachable the moment
	 * empty systems started being hidden: a card with no ROMs on it, or ROMs
	 * one directory too deep, now leaves nothing on the shelf and the first
	 * press of left or right took the launcher down with SIGFPE. */
	if (n <= 0) return;

	/* The direction is carried through, not inferred from the cursor: on a
	 * shelf of two, moving from either card to the other is one step in BOTH
	 * directions, and only the press says which. */
	int dir = 0;
	if (in_repeat(&a->in, IN_LEFT))  { a->sys_cursor = (a->sys_cursor - 1 + n) % n; dir = -1; }
	if (in_repeat(&a->in, IN_RIGHT)) { a->sys_cursor = (a->sys_cursor + 1) % n; dir = +1; }
	if (a->in.pressed[IN_ACCEPT])    enter_system(a);
	cf_set_cursor_dir(&a->cf_sys, a->sys_cursor, n, dir);
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
	int dir = 0;
	if (in_repeat(&a->in, IN_LEFT))  { v->cursor = (v->cursor - 1 + n) % n; dir = -1; }
	if (in_repeat(&a->in, IN_RIGHT)) { v->cursor = (v->cursor + 1) % n; dir = +1; }
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
	/* Y favorites what is under the cursor. Written through immediately:
	 * there is no confirm step to hang the save off, and the alternative is
	 * losing the choice to a flat battery. */
	if (a->in.pressed[IN_Y] && n > 0) {
		char p[CFG_STR * 2];
		fav_toggle(a->sys.systems[shelf_owner(a, a->sys_cursor, v->cursor)].tag,
		           v->list.items[v->cursor].file);
		fav_path(p, sizeof p);
		fav_save(p);
		/* The shelf follows immediately. Everything below this line may have
		 * moved - the system indices, sys_cursor, and v itself - so nothing
		 * from before it can be reused. */
		refresh_favorites_shelf(a);
		return;
	}
	if (a->in.pressed[IN_ACCEPT]) { launch(a); return; }
	cf_set_cursor_dir(&v->cf, v->cursor, n, dir);
}

/* ---------- main ---------------------------------------------------------- */

/* Drop systems with nothing in them.
 *
 * A card is a promise that opening it leads somewhere, and nine cards where
 * three have games is eight swipes to find the one you wanted. systems.cfg
 * stays the full list of what TortOS knows how to run; this is only what is
 * worth showing today.
 *
 * Compacted in place rather than filtered at draw time because sys_cursor
 * indexes sys.systems[] directly in a dozen places - the tint, the card art,
 * the launch, the resume - and an index layer over all of them would be a lot
 * of surface for a cosmetic rule. Both arrays move together or the shelf
 * shows one system's card over another's games.
 *
 * Textures are not freed here: this runs before prime_sys_window, so there
 * are none yet. */
static void hide_empty_systems(app *a)
{
	int i, n = 0;

	for (i = 0; i < a->sys.count; i++) {
		if (a->view[i].list.count <= 0) continue;
		if (n != i) {
			a->sys.systems[n] = a->sys.systems[i];
			a->view[n] = a->view[i];
			memset(&a->view[i], 0, sizeof a->view[i]);
		}
		n++;
	}
	if (n != a->sys.count)
		fprintf(stderr, "scan: %d of %d systems have games\n", n, a->sys.count);
	a->sys.count = n;
	if (a->sys_cursor >= n) a->sys_cursor = n ? n - 1 : 0;
}

/* Build the Favorites shelf: one shelf whose games come from every other.
 *
 * Resolved against the shelves that actually scanned, so a favorite whose ROM
 * is off the card simply does not appear - which is the same rule as hiding
 * an empty system, and better than a card that opens onto a game that is not
 * there. Sorted by name like every other shelf rather than by the order
 * someone pressed Y, so it reads as a shelf and not as a history.
 *
 * Inserted at the front, which shifts every system index up by one, so the
 * owners recorded during resolution are corrected afterwards. Runs before
 * prime_sys_window, so there are no textures to move with them.
 *
 * It is NOT a system: no core, no folder, no extensions. Nothing may read
 * those from a->sys.systems[] for a game on this shelf - shelf_owner() is how
 * every one of them is found instead. */
static void build_favorites_shelf(app *a)
{
	game_entry *items;
	int *owner;
	int n = 0, i, k, si, real;

	if (fav_count() <= 0 || a->sys.count <= 0) return;
	if (a->sys.count >= CFG_MAX_SYSTEMS) {
		fprintf(stderr, "scan: no room for a Favorites shelf\n");
		return;
	}

	items = calloc(FAV_MAX, sizeof *items);
	owner = calloc(FAV_MAX, sizeof *owner);
	if (!items || !owner) { free(items); free(owner); return; }

	real = a->sys.count;
	for (i = 0; i < fav_count(); i++) {
		const char *tag, *file;
		bool found = false;
		if (!fav_at(i, &tag, &file)) continue;
		for (si = 0; si < real; si++) {
			if (strcmp(a->sys.systems[si].tag, tag)) continue;
			for (k = 0; k < a->view[si].list.count; k++) {
				if (strcmp(a->view[si].list.items[k].file, file)) continue;
				items[n] = a->view[si].list.items[k];
				owner[n] = si;
				n++;
				found = true;
				break;
			}
			break;
		}
		/* Said out loud. A favorite that does not resolve is either a ROM
		 * that has left the card or a key that never matched, and silently
		 * showing one fewer game than the file lists is the kind of thing
		 * that gets noticed months later. */
		if (!found) fprintf(stderr, "fav: unresolved %s\t%s\n", tag, file);
		if (n >= FAV_MAX) break;
	}
	if (n == 0) { free(items); free(owner); return; }

	/* Insertion sort on the shown title, carrying the owner with it. n is at
	 * most FAV_MAX and realistically a dozen. */
	for (i = 1; i < n; i++) {
		game_entry t = items[i];
		int to = owner[i], j = i - 1;
		while (j >= 0 && strcasecmp(items[j].name, t.name) > 0) {
			items[j + 1] = items[j];
			owner[j + 1] = owner[j];
			j--;
		}
		items[j + 1] = t;
		owner[j + 1] = to;
	}

	/* sys_tex/sys_w/sys_h are parallel to systems[] and have to move with it.
	 * At startup they are all NULL and skipping them is harmless, which is
	 * exactly why it would have gone unnoticed until the shelf was rebuilt
	 * live with the cards already loaded - and then one system would have
	 * been wearing the next one's art. */
	for (i = a->sys.count; i > 0; i--) {
		a->sys.systems[i] = a->sys.systems[i - 1];
		a->view[i] = a->view[i - 1];
		a->sys_tex[i] = a->sys_tex[i - 1];
		a->sys_w[i] = a->sys_w[i - 1];
		a->sys_h[i] = a->sys_h[i - 1];
	}
	a->sys_tex[0] = NULL;
	for (i = 0; i < n; i++) owner[i]++;          /* everything moved up one */

	memset(&a->sys.systems[0], 0, sizeof a->sys.systems[0]);
	snprintf(a->sys.systems[0].name, CFG_STR, "%s", "Favorites");
	snprintf(a->sys.systems[0].tag, sizeof a->sys.systems[0].tag, "%s", "FAV");
	snprintf(a->sys.systems[0].card, CFG_STR, "%s", "FAVORITES.png");
	a->sys.systems[0].accent = MENU_ACCENT;      /* TortOS's, not a console's */

	memset(&a->view[0], 0, sizeof a->view[0]);
	a->view[0].list.items = items;
	a->view[0].list.count = n;
	a->view[0].list.scanned = true;
	a->view[0].owner = owner;
	a->view[0].tex = calloc((size_t)n, sizeof *a->view[0].tex);
	a->view[0].tw  = calloc((size_t)n, sizeof *a->view[0].tw);
	a->view[0].th  = calloc((size_t)n, sizeof *a->view[0].th);
	if (!a->view[0].tex || !a->view[0].tw || !a->view[0].th) {
		free(a->view[0].tex); free(a->view[0].tw); free(a->view[0].th);
		free(items); free(owner);
		memset(&a->view[0], 0, sizeof a->view[0]);
		for (i = 0; i < a->sys.count; i++) {
			a->sys.systems[i] = a->sys.systems[i + 1];
			a->view[i] = a->view[i + 1];
			a->sys_tex[i] = a->sys_tex[i + 1];
			a->sys_w[i] = a->sys_w[i + 1];
			a->sys_h[i] = a->sys_h[i + 1];
		}
		return;
	}
	a->sys.count++;
	fprintf(stderr, "scan: %-16s %d games\n", "Favorites", n);
}

static bool fav_shelf_present(app *a)
{
	return a->sys.count > 0 && strcmp(a->sys.systems[0].tag, "FAV") == 0;
}

/* Take the Favorites shelf back off. Its game list and owner map are this
 * file's own allocations rather than lib_scan's, and the game_entry values in
 * it are copies - the originals belong to the shelves they came from and must
 * not be touched. */
static void drop_favorites_shelf(app *a)
{
	int i;

	if (!fav_shelf_present(a)) return;

	for (i = 0; i < a->view[0].list.count; i++)
		if (a->view[0].tex[i]) SDL_DestroyTexture(a->view[0].tex[i]);
	free(a->view[0].tex);
	free(a->view[0].tw);
	free(a->view[0].th);
	free(a->view[0].list.items);
	free(a->view[0].owner);
	if (a->sys_tex[0]) SDL_DestroyTexture(a->sys_tex[0]);

	for (i = 0; i + 1 < a->sys.count; i++) {
		a->sys.systems[i] = a->sys.systems[i + 1];
		a->view[i] = a->view[i + 1];
		a->sys_tex[i] = a->sys_tex[i + 1];
		a->sys_w[i] = a->sys_w[i + 1];
		a->sys_h[i] = a->sys_h[i + 1];
	}
	a->sys.count--;
	memset(&a->view[a->sys.count], 0, sizeof a->view[0]);
	a->sys_tex[a->sys.count] = NULL;
}

/* Rebuild the Favorites shelf in place, for use the moment a favorite
 * changes. Restarting the launcher to see a star take effect is not an
 * answer.
 *
 * Drop and rebuild rather than patch: the shelf is a sorted projection of a
 * set over every other shelf, and the four cases - it appears, it grows, it
 * shrinks, it goes away - are one line each this way and four separate
 * index-juggling routines the other.
 *
 * Everything is re-found by TAG afterwards, never by index. Inserting or
 * removing the shelf moves every system up or down by one, so the index the
 * caller was standing on means something different by the time this returns.
 * The tag does not move. */
static void refresh_favorites_shelf(app *a)
{
	char tag[sizeof a->sys.systems[0].tag];
	int cur, i;

	if (a->sys.count <= 0) return;
	snprintf(tag, sizeof tag, "%s", a->sys.systems[a->sys_cursor].tag);
	cur = a->view[a->sys_cursor].cursor;

	drop_favorites_shelf(a);
	build_favorites_shelf(a);

	for (i = 0; i < a->sys.count; i++)
		if (strcmp(a->sys.systems[i].tag, tag) == 0) break;

	if (i < a->sys.count) {
		a->sys_cursor = i;
		/* The Favorites list can shrink under the cursor - un-favoriting the
		 * game you are looking at is the ordinary way to use this. */
		if (cur >= a->view[i].list.count)
			cur = a->view[i].list.count ? a->view[i].list.count - 1 : 0;
		a->view[i].cursor = cur;
		cf_reset(&a->view[i].cf, cur);
	} else {
		/* The shelf being stood on no longer exists, which happens exactly
		 * once: un-favoriting the last favorite while inside Favorites. There
		 * is no list to stay in, so go back out to the shelves. */
		a->sys_cursor = 0;
		a->screen = SCREEN_SYSTEMS;
	}
	cf_reset(&a->cf_sys, a->sys_cursor);
}

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
	hide_empty_systems(a);
	build_favorites_shelf(a);
}

/* --shot <file.png> [--screen games|systems] draws one frame, writes it out
 * and exits. This is how the shelf gets looked at without a device in hand:
 * the host build renders exactly what the handheld renders. */
static const char *shot_path;
static int shot_screen = -1;
static int shot_menu, shot_menu_sel;
static int shot_kb, shot_kb_layer;
static const char *shot_notice;
static const char *shot_kb_text = "correct horse";
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
	SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, TORTOS_SCREEN_W,
	                                                  TORTOS_SCREEN_H, 32,
	                                                  SDL_PIXELFORMAT_RGBA32);
	draw_shelf(a);
	if (shot_menu) tortos_menu_draw(a, shot_menu_sel);
	if (shot_slots) shot_draw_slots(a);
	if (shot_kb) kb_preview(a->r, "Wi-Fi password", shot_kb_text,
	                        shot_kb_layer, 1, 0, MENU_ACCENT);
	/* The in-game notice is not drawn by this process - Diatom composites it
	 * over the game - so it cannot be screenshotted like the rest. Rendered
	 * to its wire format and read straight back, which is the same pixels
	 * Diatom will show and therefore the thing worth looking at. */
	if (shot_notice) {
		char dt[512];
		FILE *nf;

		snprintf(dt, sizeof dt, "%s.dtov", shot_path);
		if (notice_render("Unlocked  -  5 points", shot_notice, dt) &&
		    (nf = fopen(dt, "rb"))) {
			unsigned char hd[8];
			if (fread(hd, 1, 8, nf) == 8 && !memcmp(hd, "DTOV", 4)) {
				int nw = hd[4] | (hd[5] << 8), nh = hd[6] | (hd[7] << 8);
				SDL_Surface *ns = SDL_CreateRGBSurfaceWithFormat(
					0, nw, nh, 32, SDL_PIXELFORMAT_ARGB8888);
				if (ns && fread(ns->pixels, 4, (size_t)nw * nh, nf)
				          == (size_t)nw * nh) {
					SDL_Rect at = { (TORTOS_SCREEN_W - nw) / 2,
					                TORTOS_SCREEN_H - nh - TORTOS_SCREEN_H / 24,
					                nw, nh };
					SDL_Texture *nt = SDL_CreateTextureFromSurface(a->r, ns);
					if (nt) {
						SDL_SetTextureBlendMode(nt, SDL_BLENDMODE_BLEND);
						SDL_RenderCopy(a->r, nt, NULL, &at);
						SDL_DestroyTexture(nt);
					}
				}
				if (ns) SDL_FreeSurface(ns);
			}
			fclose(nf);
			remove(dt);
		}
	}
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
		/* --menu [row] draws the TortOS menu over whichever screen --screen
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
		/* --keyboard [layer] [text] draws one frame of the text-entry panel,
		 * for the same reason --menu and --slots exist: it is dense, and
		 * laying it out against a screenshot beats a round trip to a device. */
		else if (!strcmp(argv[i], "--notice") && i + 1 < argc) {
			shot_notice = argv[++i];
		}
		else if (!strcmp(argv[i], "--keyboard")) {
			shot_kb = 1;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '2')
				shot_kb_layer = atoi(argv[++i]);
			if (i + 1 < argc && argv[i + 1][0] != '-') shot_kb_text = argv[++i];
		}
		else if (!strcmp(argv[i], "--slots")) {
			shot_slots = 1;
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_slot_sel = atoi(argv[++i]);
			if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
				shot_slot_aspect = (float)atof(argv[++i]);
		}
	}
	/* --wifi prints what the radio can see and exits. The wifi module talks
	 * to firmware that only exists on the device, so it cannot be exercised
	 * from the host at all, and a flag that reports its view is the cheapest
	 * way to tell "the machinery is wrong" from "the network is". */
	if (argc > 1 && !strcmp(argv[1], "--wifi")) {
		wifi_net nets[WIFI_MAX_NETS];
		char ssid[WIFI_SSID_MAX], ip[64];
		int n, i;
		wifi_state st;

		printf("bringing the supplicant up ...\n");
		printf("  wifi_up: %s\n", wifi_up() ? "ok" : "FAILED");
		st = wifi_status(ssid, sizeof ssid, ip, sizeof ip);
		printf("  status: %s  ssid=[%s] ip=[%s]\n",
		       st == WIFI_CONNECTED  ? "connected"  :
		       st == WIFI_CONNECTING ? "connecting" :
		       st == WIFI_IDLE       ? "idle"       : "off", ssid, ip);
		n = wifi_scan(nets, WIFI_MAX_NETS);
		printf("  scan: %d network(s)\n", n);
		for (i = 0; i < n; i++)
			printf("    %-32s %4d dBm  %-9s %s\n", nets[i].ssid,
			       nets[i].signal, nets[i].secured ? "secured" : "open",
			       nets[i].known ? "known" : "");
		return n < 0 ? 1 : 0;
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
	snprintf(path, sizeof path, "%s/tortos.cfg", P_ROOT);
	cfg_load_tortos(path, &a.cfg);

	/* Before the scan, because the scan builds the Favorites shelf out of
	 * them and a shelf cannot be built from a list that has not been read. */
	{	char fp[CFG_STR * 2];
		fav_path(fp, sizeof fp);
		fav_load(fp);
	}

	{	char cp[CFG_STR * 2];
		chv_store_path(cp, sizeof cp);
		chv_earned_load(cp);
		plat_resident_on_unlock(on_cheevo_unlocked);
		plat_resident_on_tick(on_game_tick);

		/* Without this every HTTPS request fails verification, because the
		 * device has no trust store of its own - res/ssl/README.md. */
		snprintf(cp, sizeof cp, "%s/cacert.pem", P_ROOT);
		ra_set_ca_path(cp);
		ra_creds_path(cp, sizeof cp);
		ra_creds_load(cp);
	}

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
	 * afterwards: doing that put tortos.cfg ahead of the level the player last
	 * chose and undid every nudge on the next restart. */
	plat_settings_init(a.cfg.volume, a.cfg.brightness);
	plat_leds_off();
	t_mark("video+input");

	/* Before ui_init, which is where the sizes are decided; it persists across
	 * the ui_quit/ui_init pair the standalone-emulator fallback goes through. */
	/* The size the player chose beats the shipped default, the same way a
	 * saved brightness does. Read before ui_init, which is when the scale is
	 * applied. */
	ui_set_font_scale(text_scale_load(a.cfg.font_scale));
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
	/* After restore_place, so it wins: `.last` says where the shelf was and
	 * this says what was actually being played. Before the card priming
	 * below, because that loads textures for whatever the cursor is on, and
	 * a fallback to the shelf should find the right ones there. */
	a.resume_menu = playing_restore(&a);
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
	 * nobody holding the device. TORTOS_AUTOLAUNCH="TAG<tab>rom-filename";
	 * pair with TORTOS_AUTOSTOP_S to end the game on a clock. */
	{
		const char *auto_spec = getenv("TORTOS_AUTOLAUNCH");
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
	 * bursts input devices emit as they come up -- before honoring anything. */
	{
		Uint32 grace = SDL_GetTicks() + 350;
		while (SDL_GetTicks() < grace) { plat_input_poll(&a.in); SDL_Delay(8); }
		memset(&a.in, 0, sizeof a.in);
	}

	/* Straight back into the game, before the shelf is ever drawn. After the
	 * input grace above, so a wake press replayed by the boot does not land in
	 * the menu that is about to open. */
	if (a.resume_menu && a.running) {
		a.screen = SCREEN_GAMES;
		launch(&a);
	}

	while (a.running) {
		plat_input_poll(&a.in);
		if (a.in.quit_requested || want_quit) break;

		if (a.in.pressed[IN_POWER]) { power_off(&a); break; }

		if (in_repeat(&a.in, IN_VOLUP))    plat_volume_nudge(+1);
		if (in_repeat(&a.in, IN_VOLDN))    plat_volume_nudge(-1);
		if (in_repeat(&a.in, IN_BRIGHTUP)) plat_brightness_nudge(+1);
		if (in_repeat(&a.in, IN_BRIGHTDN)) plat_brightness_nudge(-1);

		/* MENU on the shelf is TortOS's own menu, the counterpart to the one
		 * MENU opens in a game. It draws over the shelf and returns here. */
		if (a.in.pressed[IN_MENU]) { tortos_menu(&a); continue; }

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
