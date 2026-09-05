/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_CONFIG_H
#define TORTOS_CONFIG_H

#include <stdbool.h>

#define CFG_MAX_SYSTEMS 16   /* eleven shipped, plus Favorites; 12 was exact */
#define CFG_STR 256

typedef struct {
	char name[CFG_STR];   /* display name, e.g. "TurboGrafx-16" */
	char folder[CFG_STR]; /* ROM folder under Roms/            */
	char core[CFG_STR];   /* core short name, e.g. "mednafen_pce_fast" */
	char tag[8];          /* saves, states and per-game configs hang off this,
	                       * so it is also the folder name under Saves/. Three
	                       * characters at most: this array is the limit, and a
	                       * longer tag is silently truncated into it. */
	char card[CFG_STR];   /* card art filename under TortOS/cards/ */
	unsigned accent;      /* 0xRRGGBB, tints the focus glow and the rail */
	char exts[CFG_STR];   /* which extensions in that folder are games; empty
	                       * means everything, which also means save files and
	                       * stray text files show up as games */
	/* Firmware a DISC image needs, resolved against Bios/. Empty for a system
	 * that needs none. Per-system and disc-only rather than per-system alone,
	 * because a PC Engine HuCard runs with no System Card and a CD does not -
	 * declaring it for the whole shelf would refuse the cartridges too. */
	char disc_bios[CFG_STR];
} system_cfg;

typedef struct {
	system_cfg systems[CFG_MAX_SYSTEMS];
	int count;
} systems_cfg;

typedef struct {
	/* Defaults, not settings: the settings code prefers the level the player
	 * last chose in levels.cfg and falls back to these. -1 = not configured. */
	int volume;             /* 0..100 */
	int brightness;         /* a rung, 0..PLAT_BRIGHT_MAX */
	float font_scale;       /* multiplies the whole type scale; 1.0 = as designed */
	char startup_system[CFG_STR];
} tortos_cfg;

bool cfg_load_systems(const char *path, systems_cfg *out);
void cfg_load_tortos(const char *path, tortos_cfg *out);

#endif
