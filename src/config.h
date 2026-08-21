/* SPDX-License-Identifier: 0BSD */
#ifndef PLAYOS_CONFIG_H
#define PLAYOS_CONFIG_H

#include <stdbool.h>

#define CFG_MAX_SYSTEMS 8
#define CFG_STR 256

typedef struct {
	char name[CFG_STR];   /* display name, e.g. "TurboGrafx-16" */
	char folder[CFG_STR]; /* ROM folder under Roms/            */
	char core[CFG_STR];   /* core short name, e.g. "mednafen_pce_fast" */
	char tag[8];          /* minarch tag: saves, states and configs hang off
	                       * this. Three characters at most -- struct Core
	                       * declares tag[8] and it is not negotiable. */
	char card[CFG_STR];   /* card art filename under PlayOS/cards/ */
	unsigned accent;      /* 0xRRGGBB, tints the focus glow and the rail */
	char exts[CFG_STR];   /* which extensions in that folder are games; empty
	                       * means everything, which also means save files and
	                       * stray text files show up as games */
} system_cfg;

typedef struct {
	system_cfg systems[CFG_MAX_SYSTEMS];
	int count;
} systems_cfg;

typedef struct {
	int volume;             /* 0..100, -1 = leave alone */
	int brightness;         /* 0..10,  -1 = leave alone */
	char startup_system[CFG_STR];
} playos_cfg;

bool cfg_load_systems(const char *path, systems_cfg *out);
void cfg_load_playos(const char *path, playos_cfg *out);

#endif
