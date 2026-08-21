/* SPDX-License-Identifier: 0BSD */
#ifndef PLAYOS_COVERFLOW_H
#define PLAYOS_COVERFLOW_H

#include <SDL.h>
#include <stdbool.h>

/* Single-row Cover Flow: perspective-tilted cards with reflections,
 * rendered with SDL_RenderGeometry. Never draws more cards than there
 * are items: the row wraps around only when count >= CF_WINDOW; below
 * that it clamps, and a 1-item list is a single centered card. */

#define CF_HALF_WINDOW 3
#define CF_WINDOW (2 * CF_HALF_WINDOW + 1)

typedef struct {
	float size;       /* card height as a fraction of screen height */
	float aspect;     /* card width / card height */
	float step;       /* neighbor spacing as a fraction of card width */
	float side_scale; /* scale of fully off-center cards */
	float center_y;   /* card center y as a fraction of screen height */
	float tilt;       /* max yaw in radians */
	float reflect;    /* reflection height as a fraction of card height */
	int side_alpha;   /* alpha of fully off-center cards (center is 255) */
	int strips;       /* vertical subdivisions per card */
} cf_layout;

extern const cf_layout CF_LAYOUT_SYSTEMS;
extern const cf_layout CF_LAYOUT_GAMES;

/* Where the focused card sits on screen, so the caller can put a glow behind
 * it and lay text out against it without duplicating the geometry. */
void cf_focus_rect(const cf_layout *lay, int screen_w, int screen_h, SDL_Rect *out);

typedef struct {
	float pos;      /* continuous position, eases toward target */
	float target;
	bool active;    /* animation in flight */
	Uint32 last_ms;
	int last_cursor;
	bool primed;
} coverflow;

/* Texture for item index; w/h receive its pixel size. May return NULL. */
typedef SDL_Texture *(*cf_tex_fn)(void *ctx, int index, int *w, int *h);

void cf_reset(coverflow *cf, int cursor);
/* Move toward cursor (shortest path when wrapping). */
void cf_set_cursor(coverflow *cf, int cursor, int count);
/* Step animation and draw. Returns true while still animating. */
bool cf_draw(coverflow *cf, SDL_Renderer *r, int screen_w, int screen_h,
             int count, cf_tex_fn get_tex, void *ctx, const cf_layout *lay);

#endif
