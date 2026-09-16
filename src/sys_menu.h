/* The two menus behind the MENU button, as rows rather than as pixels.
 *
 * MENU has two menus behind it, chosen by where it was pressed. From the
 * systems row it is about the firmware. From inside a system it is about THAT
 * system, because a menu that repeated the firmware's settings while a shelf
 * of NES games sat behind it would be answering a question nobody asked.
 *
 * Split out of main.c under ADR-0001 so that what these menus CONTAIN can be
 * checked on a build machine. Nothing here may include SDL: tools/menu-check.c
 * links this file without it, and that is the whole point.
 *
 * The caller reads the device and fills a sys_ui; this file turns that into
 * rows. Wi-Fi state in particular is asked for by the caller because asking
 * costs a fork - see menu_wifi in main.c - and this file must not care. */
#ifndef SYS_MENU_H
#define SYS_MENU_H

#include <stdbool.h>
#include <stddef.h>

#include "menu.h"
#include "config.h"
#include "wifi.h"
#include "audioout.h"

/* The TortOS menu, in the order it is read.
 *
 * Play Time leads because it is the only row here anyone opens twice. Wi-Fi,
 * Bluetooth and Audio Output are setup: you use them when something is wrong
 * or new, and then never again. A menu ordered by what a device needs on its
 * first day puts the thing you actually come back to five rows down. */
typedef enum {
	PM_STATS,
	PM_WIFI, PM_BT, PM_AUDIO, PM_XFER,
	PM_SLEEP, PM_TEXT, PM_THEME, PM_DIR, PM_SCRAPE, PM_ACHIEVEMENTS, PM_ABOUT, PM_ROWS
} pm_row;

/* The system menu. Games and Core carry real values rather than invented ones,
 * because a placeholder that lies about the machine it is describing is worse
 * than no row. The rest name capabilities that already exist on the emulator
 * side - Diatom has per-system display modes and core-supplied button labels -
 * so these are hooks waiting to be wired, not wishes. */
typedef enum {
	SM_GAMES, SM_CORE, SM_SHOW, SM_SORT,
	SM_DISPLAY, /* SM_BUTTONS, */ SM_BOXART, SM_RESCAN, SM_ROWS
} sm_row;

#define MENU_MAX_ROWS 12

/* Where the built rows' text lives. A row holds pointers, not copies, so the
 * strings a build formats have to outlive the build; the caller owns this and
 * keeps it alive as long as it keeps the rows. */
typedef struct { char a[24], b[CFG_STR], c[16], d[40]; } menu_bufs;

/* Everything either menu needs to know about the device, gathered by the
 * caller. A struct rather than a dozen arguments so that adding a fact to a
 * row does not change every call site, and so a check can state a device's
 * whole situation in one initializer. */
typedef struct {
	bool games;              /* the system menu, rather than TortOS's own */

	wifi_state  wifi;        /* already cached by the caller; see menu_wifi */
	const char *ssid;        /* the network's name when connected, else NULL */

	/* The TortOS menu */
	bool        ra_in;
	const char *ra_name;     /* only read when ra_in */
	const char *text_size;   /* "100%" - the caller owns the scale table */
	const char *cards;       /* the showing card set's name, from CARD_SETS */
	const char *cards_dir;   /* which way the shelves run, from CARD_DIRS */
	int         auto_off;    /* seconds, 0 for off */
	/* Where sound goes: the policy the player set, and where it actually ends
	 * up under that policy. Both, because the row has to name a place - "Auto"
	 * on its own is a rule, not somewhere you can hear. */
	aout_policy audio_policy;
	aout_dest   audio_dest;

	/* Bluetooth: the name of the connected headset, or NULL. The row used to
	 * read "not yet" and be dead, which was true of the pairing screen and
	 * false of the feature - game audio has gone to a headset since
	 * 2026-09-03. */
	const char *bt_name;

	/* The system menu */
	const char *sys_name;
	const char *sys_core;
	int         game_count;
	const char *dmode;
	/* The label of the shelf's sort order. A label rather than an index for
	 * the same reason dmode is one: this file must not know the table, or a
	 * check that links it would have to link the table too. */
	const char *sort;
} sys_ui;

/* Seconds to the label a row shows. Pure, and here rather than in main.c so
 * the check can hold it to "never", "30s" and "2m" without a device. */
void sys_menu_auto_off_label(int seconds, char *out, size_t n);

/* Build whichever menu u->games calls for. Returns the row count, so the input
 * loop never needs to know which of the two it is driving. */
int sys_menu_build(const sys_ui *u, menu_row *out, menu_bufs *b,
                   const char **heading);

#endif
