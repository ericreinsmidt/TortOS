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

/* How many cards either side of the cursor the CALLER guarantees are already
 * decoded. This is a contract, not a preference: a move that crosses cold
 * cards decodes them on the render path, and one card is 7-27ms on this
 * device against a 16.7ms frame - measured 2026-09-12 on Game Boy art, which
 * averages 327KB. Seven of those in one frame is why a long jump showed two
 * still pictures and no motion. main.c's TEX_KEEP_NEAR is the guarantee and
 * asserts it is not less than this. */
#define CF_WARM_CARDS 8

typedef struct {
	float size;       /* card height as a fraction of screen height */
	float aspect;     /* card width / card height */
	float step;       /* neighbor spacing as a fraction of card width */
	float side_scale; /* scale of fully off-center cards */
	float center_y;   /* card center y as a fraction of screen height */
	float tilt;       /* max yaw in radians */
	float reflect;    /* reflection height as a fraction of card height */
	/* Clear air between the art and its reflection, as a fraction of the
	 * card's half height - about 25px on a focused card, and it shrinks with
	 * the side cards because it is in the card's own units rather than the
	 * screen's. A reflection that touches reads as the object continuing;
	 * a small gap reads as a surface it is standing on. */
	float reflect_gap;
	/* Lay the row down the screen instead of across it. The cards, the
	 * scaling and the reflections are unchanged - only which axis the
	 * neighbors are offset along, and `step` then counts card HEIGHTS rather
	 * than widths, because that is the direction they are spaced in. */
	bool vertical;
	int side_alpha;   /* alpha of fully off-center cards (center is 255) */
	int strips;       /* vertical subdivisions per card */
} cf_layout;

extern const cf_layout CF_LAYOUT_SYSTEMS;
/* The same two rows stood on end, for the Vertical direction. Separate tables
 * rather than a flag applied to the others: the screen is 1024x768, so a card
 * sized to fill the width leaves no room above and below, and every number has
 * to be chosen again rather than reused. */
extern const cf_layout CF_LAYOUT_SYSTEMS_V;
extern const cf_layout CF_LAYOUT_GAMES_V;
extern const cf_layout CF_LAYOUT_GAMES;
/* The systems row again, one at a time and flat. Systems only: box art keeps
 * the angled row. */
extern const cf_layout CF_LAYOUT_SINGLE;
/* The same idea for the games shelf, whose text sits differently. */
extern const cf_layout CF_LAYOUT_GAME_FACE;

/* Turn two full-screen faces of a cube about a horizontal axis.
 *
 * `frac` is how far between them, 0 to 1: at 0 the near face is square on and
 * the far one is edge-on and invisible, at 1 they have swapped. The faces are
 * whole screens rendered offscreen, so whatever is on them - art, name, count,
 * reflection - turns together without any of it needing to know.
 *
 * Faces darken as they turn away. A cube whose sides stay evenly lit reads as
 * two flat pictures sliding past each other rather than as one solid. */
void cf_draw_cube(SDL_Renderer *r, SDL_Texture *near_face, SDL_Texture *far_face,
                  float frac, int screen_w, int screen_h, bool yaw,
                  unsigned near_rgb, unsigned far_rgb);

/* Where the focused card sits on screen, so the caller can put a glow behind
 * it and lay text out against it without duplicating the geometry. */
void cf_focus_rect(const cf_layout *lay, int screen_w, int screen_h, SDL_Rect *out);

/* A move is a fixed-duration tween from `from` to `target` starting at `t0`,
 * rather than an ease toward a moving target. Distance changes the speed, not
 * the time: crossing the whole shelf takes exactly as long as stepping one
 * card, so the row never feels further away than it is. */
/* How a move is spread over its time. OUT_CUBIC peaks on the first frame and
 * decelerates, which is what makes a card step feel immediate. SMOOTH starts
 * and ends at rest and peaks at half again the average - slower to leave, but
 * a solid object that reaches full speed between two frames does not read as
 * turning, it reads as having cut. */
typedef enum {
	CF_EASE_OUT_CUBIC = 0,
	CF_EASE_SMOOTH
} cf_ease;

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
	/* Per-shelf timing. Zero means the default, which is what a card row
	 * wants; the cube sets its own because it turns a whole screen through a
	 * right angle rather than sliding a card a few hundred pixels, and the
	 * same duration spent on the two is not the same thing to look at. */
	float anim_ms;
	cf_ease ease;
	/* Whether to skip ahead rather than queue when told to move again while
	 * already moving. See cf_set_cursor_dir. */
	bool chase;
	/* How far a move too long to draw may travel before it cuts, in cards.
	 * Per-shelf because a card means different things on each: the row shows
	 * seven at once, so eight of them read as travel, while Vertical shows
	 * ONE filling the screen and eight would be a full-screen blur. Capped by
	 * CF_WARM_CARDS however it is set - a glide is only free while the cards
	 * it crosses are already decoded. Zero disables it. */
	float glide;
	/* A move longer than the warm window: the tween runs out through the warm
	 * cards behind the cursor and then cuts to `land`. */
	float land;
	bool  cutting;
} coverflow;

/* The index a pending cut will land on, or -1 when none is pending. The caller
 * uses it to decode that window while the departure is still being drawn, so
 * the move lands on a card that is already there. */
int cf_landing(const coverflow *cf, int count);

/* Whether a cut is in flight. The caller holds off evicting while it is, or it
 * would free the very cards the departure is still drawing: the cursor is
 * already at the destination and eviction is measured from the cursor. */
bool cf_cutting(const coverflow *cf);

/* Texture for item index; w/h receive its pixel size. May return NULL.
 *
 * `content_bottom` receives how far down the texture its opaque pixels reach,
 * as a fraction of height: 1.0 for art that fills its canvas, less for art
 * padded with transparency below the subject. The reflection is drawn from
 * there rather than from the card's edge - see draw_card. */
typedef SDL_Texture *(*cf_tex_fn)(void *ctx, int index, int *w, int *h,
                                  float *content_bottom);

void cf_reset(coverflow *cf, int cursor);
/* Move toward cursor (shortest path when wrapping). */
void cf_set_cursor(coverflow *cf, int cursor, int count);
/* Same, with the direction the user actually pressed: -1, 0 or +1. A ring of
 * two has both neighbors one step away, so it is the only size where the
 * shortest-path arithmetic cannot work out which way to turn. */
void cf_set_cursor_dir(coverflow *cf, int cursor, int count, int dir);
/* Advance the animation without drawing anything.
 *
 * The vertical shelves turn a cube instead of laying out a row, so they never
 * call cf_draw and would otherwise leave `pos` frozen. Same clock, same
 * easing, same wrap handling - only the drawing differs. */
void cf_tick(coverflow *cf, int count);

/* Step animation and draw. Returns true while still animating. */
bool cf_draw(coverflow *cf, SDL_Renderer *r, int screen_w, int screen_h,
             int count, cf_tex_fn get_tex, void *ctx, const cf_layout *lay);

#endif
