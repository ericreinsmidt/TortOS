/* What a menu row is. No SDL, no renderer, nothing device-specific.
 *
 * Separated from main.c on purpose. ADR-0001 says a screen's build function
 * must be callable with no renderer and no device, so that "given this state,
 * what does this menu contain" is answerable by `make check` - and that is only
 * true if the types it needs can be included without dragging SDL in with them.
 *
 * The conventions, and the five defects that produced them: docs/menus.md.
 */
#ifndef TORTOS_MENU_H
#define TORTOS_MENU_H

#include <stdbool.h>

typedef struct {
	const char *label;
	const char *value;  /* the right column, or NULL for a row that is only a label */
	bool live;          /* false: a placeholder, drawn quiet and doing nothing */
} menu_row;

/* A row that is only a rule, separating a list from a footer that is not part
 * of it. Drawn with the same bar the heading uses, so the two cannot drift
 * apart - which is why it belongs to the menu code and not to a screen painting
 * its own line.
 *
 * Not live, so any screen that steps over dead rows skips it for free. */
#define MENU_RULE ((menu_row){ NULL, NULL, false })

/* A note under the rule: a key legend, a count, a caption. Centered, because it
 * describes the list rather than being an item in it - left-aligned it read as
 * one more row you had failed to be able to select.
 *
 * Marked by a sentinel in `value` rather than a new struct field, so every
 * existing { label, value, live } initializer stays valid. */
#define MENU_NOTE_MARK ((const char *)1)
#define MENU_NOTE(s)   ((menu_row){ (s), MENU_NOTE_MARK, false })
#define ROW_IS_NOTE(r) ((r).value == MENU_NOTE_MARK)

/* Move to the next live row in `dir` (+1 or -1), or stay put if none is.
 *
 * Bounded, deliberately. A menu can legitimately be all dead rows - "No
 * networks found" under a rule is exactly that - and the do/while this
 * replaced would spin forever on one. Pure, so tools/menu-check.c holds it
 * to that. */
int menu_step_sel(const menu_row *rows, int n, int sel, int dir);

#endif
