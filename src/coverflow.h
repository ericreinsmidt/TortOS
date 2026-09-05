/* SPDX-License-Identifier: MIT */
#ifndef TORTOS_COVERFLOW_H
#define TORTOS_COVERFLOW_H

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

/* A move is a fixed-duration tween from `from` to `target` starting at `t0`,
 * rather than an ease toward a moving target. Distance changes the speed, not
 * the time: crossing the whole shelf takes exactly as long as stepping one
 * card, so the row never feels further away than it is. */
typedef struct {
	float pos;      /* continuous position, interpolated from -> target */
	float from;     /* where the move in flight started */
	float target;
	Uint32 t0;      /* when it started */
	bool active;    /* animation in flight */
	int last_cursor;
	bool primed;
	/* Which way the row last traveled, -1 or +1. Only a two-item ring needs
	 * it: there, both representatives of the other card sit exactly one step
	 * away, so which side it rests on is a genuine tie and the direction of
	 * travel is the only thing that can settle it sensibly. */
	int last_dir;
} coverflow;

/* Texture for item index; w/h receive its pixel size. May return NULL. */
typedef SDL_Texture *(*cf_tex_fn)(void *ctx, int index, int *w, int *h);

void cf_reset(coverflow *cf, int cursor);
/* Move toward cursor (shortest path when wrapping). */
void cf_set_cursor(coverflow *cf, int cursor, int count);
/* Same, with the direction the user actually pressed: -1, 0 or +1. A ring of
 * two has both neighbors one step away, so it is the only size where the
 * shortest-path arithmetic cannot work out which way to turn. */
void cf_set_cursor_dir(coverflow *cf, int cursor, int count, int dir);
/* Step animation and draw. Returns true while still animating. */
bool cf_draw(coverflow *cf, SDL_Renderer *r, int screen_w, int screen_h,
             int count, cf_tex_fn get_tex, void *ctx, const cf_layout *lay);

#endif
