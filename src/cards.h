/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_CARDS_H
#define TORTOS_CARDS_H

#include <stdbool.h>
#include <string.h>

/* Which art the shelf wears.
 *
 * A set is a directory under res/cards/ holding files named by column 6 of
 * systems.cfg, so adding one is dropping in a folder - no config change and no
 * code change. The art does NOT have to match the default set's shape: cf_draw
 * contain-fits every texture and lengthens the reflection for short art, which
 * is why a set of square photographs sits on the same shelf as portrait cards
 * without touching the layout.
 *
 * Accents stay in systems.cfg and do not vary by set. They were sampled from
 * the classic art, and the obvious idea of resampling them per set does not
 * survive contact with the photographs: measured over their opaque pixels the
 * hardware runs 0-13% saturation, Genesis at 0, because consoles are gray and
 * black plastic. There is no color in them to sample. */
#define CARDS_DEFAULT "classic"

typedef struct {
	/* What the setting stores. Separate from `dir` because a preset is art
	 * AND how it is presented, and two presets can draw the same directory a
	 * different way. Keying the setting on the directory would make those two
	 * indistinguishable once written down. */
	const char *id;
	const char *dir;   /* directory under res/cards/ */
	const char *name;  /* what the menu row calls it */
	/* Whether the art says which system it is. The classic cards have the
	 * name drawn into the image, so the shelf must not print it again; a set
	 * of bare photographs does not, and without this the shelf is unlabeled
	 * and you are asked to tell a Master System from a Genesis by silhouette.
	 * A property of the set, so a new one declares which kind it is rather
	 * than the shelf special-casing a directory by name. */
	bool labeled;
	/* One image per screen, flat, sliding in from off the edge, instead of the
	 * angled row. The whole difference is a cf_layout, which lives in
	 * coverflow.c - a flag rather than a pointer to one so this header stays
	 * free of SDL and a check can read it without linking the renderer. */
	bool single;
} card_set;

static const card_set CARD_SETS[] = {
	{ "classic", "classic", "Classic",     true,  false },
	{ "slide",   "fancy",   "Slide",       false, true  },
	{ "fancy",   "fancy",   "Fancy Pants", false, false },
};
#define CARD_SET_COUNT ((int)(sizeof CARD_SETS / sizeof CARD_SETS[0]))

/* The set a stored name selects, by index. An unknown name is the default
 * rather than an error: the setting outlives the directory it points at, and a
 * set that has been removed should leave a working shelf behind. */
static inline int cards_index(const char *id)
{
	int i;

	if (id && *id)
		for (i = 0; i < CARD_SET_COUNT; i++)
			if (!strcmp(CARD_SETS[i].id, id)) return i;
	return 0;
}

/* Stepping wraps, because two entries with a stop at each end would make the
 * row feel broken in one direction half the time. */
static inline int cards_step(int i, int d)
{
	int n = CARD_SET_COUNT;

	if (n <= 0) return 0;
	i = (i + d) % n;
	return i < 0 ? i + n : i;
}

#endif
