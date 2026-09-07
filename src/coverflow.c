/* SPDX-License-Identifier: MIT */
#include "coverflow.h"

#include <math.h>
#include <string.h>

/* The systems row: three cards, so they are large and the neighbors sit
 * close. center_y is high because the reflection owns the bottom of the
 * screen and the shelf rail sits under it.
 *
 * reflect is the reflection baseline as a multiple of the card half-height,
 * measured below the card center (see draw_card). */
const cf_layout CF_LAYOUT_SYSTEMS = {
	.size = 0.62f, .aspect = 0.78f, .step = 0.86f, .side_scale = 0.66f,
	.center_y = 0.44f, .tilt = 0.72f, .reflect = 1.34f,
	.side_alpha = 140, .strips = 16,
};

/* One system filling the screen, flat, sliding in from off the edge.
 *
 * Every difference from the row above is a number here: no yaw, no shrinking
 * or fading of the neighbors, and a step wide enough to put them past the
 * bezel. Nothing in cf_draw knows this mode exists.
 *
 * side_scale and side_alpha are 1.0 and 255 because they are NOT unused just
 * because the neighbors are off screen: during a move the incoming card is
 * partly on screen at a fractional distance, and any other values would have
 * it slide in shrunken and translucent and grow into place.
 *
 * aspect is square because this mode is paired with the photographs. Art of
 * another shape still fits - draw_card contains it - it just leaves the frame
 * unfilled on two sides.
 *
 * step 1.8 puts a neighbor's near edge about 130px past the screen at this
 * size, so nothing peeks in at rest, and the extra distance is what makes the
 * slide read as coming from outside rather than from just off the edge. */
const cf_layout CF_LAYOUT_SINGLE = {
	.size = 0.70f, .aspect = 1.00f, .step = 1.80f, .side_scale = 1.00f,
	.center_y = 0.40f, .tilt = 0.0f, .reflect = 1.15f,
	.side_alpha = 255, .strips = 16,
};

/* One game filling a cube face. The GAMES proportions rather than the SINGLE
 * ones, because the games shelf writes its title at y=40 and that clearance
 * was measured against a card of this size and position. Borrowing the
 * systems' face layout put the art's top edge at y=38 and the title landed on
 * the box art. Only `step` differs from the row: wide enough that the
 * neighbours a face must not show are off the screen entirely. */
const cf_layout CF_LAYOUT_GAME_FACE = {
	.size = 0.60f, .aspect = 0.72f, .step = 3.20f, .side_scale = 1.00f,
	.center_y = 0.47f, .tilt = 0.0f, .reflect = 1.52f,
	.side_alpha = 255, .strips = 16,
};

/* The games row: box art, so the cards are taller and there are more of them
 * in view. The title is drawn above the row, which is why it sits slightly
 * lower than the systems row. */
const cf_layout CF_LAYOUT_GAMES = {
	.size = 0.60f, .aspect = 0.72f, .step = 0.74f, .side_scale = 0.62f,
	.center_y = 0.47f, .tilt = 0.82f, .reflect = 1.52f,
	.side_alpha = 150, .strips = 16,
};

void cf_focus_rect(const cf_layout *lay, int screen_w, int screen_h, SDL_Rect *out)
{
	float ch = screen_h * lay->size;
	float cw = ch * lay->aspect;
	out->w = (int)cw;
	out->h = (int)ch;
	out->x = (int)(screen_w * 0.5f - cw * 0.5f);
	out->y = (int)(screen_h * lay->center_y - ch * 0.5f);
}

/* Every move takes this long, whatever its distance. Crossing the shelf is not
 * a longer journey than stepping one card, it is a faster one.
 *
 * This replaced an exponential ease toward the target with a speed cap over
 * it, and a teleport past six cards. The ease alone converged in time
 * proportional to log(distance), which was nearly constant already; the cap
 * made anything past a card and a half take time proportional to DISTANCE, so
 * a letter jump across a big shelf became a wait, and the teleport existed to
 * hide the worst of it. One duration removes the need for both. */
#define ANIM_MS 240.0f

/* Ease out, cubic: about seven eighths of the distance is covered in the first
 * half of the time, so a single-card step still reads as immediate even though
 * it formally takes as long as a forty-card one. */
static float ease_out(float u)
{
	float k = 1.0f - u;
	return 1.0f - k * k * k;
}

/* Smoothstep: zero velocity at both ends, peak 1.5x average in the middle. */
static float ease_smooth(float u)
{
	return u * u * (3.0f - 2.0f * u);
}

static float ease_apply(cf_ease e, float u)
{
	return e == CF_EASE_SMOOTH ? ease_smooth(u) : ease_out(u);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

/* Wrap smoothly like an infinite carousel, but never draw the same item
 * twice. A 1-item list is a single static card.
 *
 * The half-window used to be (count-1)/2, which reads as "keep the span
 * 2*half+1 within the item count" and is right for odd counts and wrong for
 * every even one. Two items gave half = 0, the draw loop ran from 0 to 0, and
 * a shelf holding two games drew exactly one card while the counter under it
 * said 1/2. Reported 2026-08-29 and reproduced on the host immediately.
 *
 * The span cannot be both odd and equal to an even count, so the duplicate is
 * dropped where it actually happens - in the slot collection - rather than
 * prevented by shrinking the window until it cannot occur. */
static bool cf_loops(int count) { return count >= 2; }

static int cf_half(int count)
{
	int h = count - 1;
	return h < CF_HALF_WINDOW ? h : CF_HALF_WINDOW;
}

void cf_reset(coverflow *cf, int cursor)
{
	memset(cf, 0, sizeof *cf);
	cf->pos = cf->from = cf->target = (float)cursor;
	cf->last_cursor = cursor;
	cf->primed = true;
}

void cf_set_cursor_dir(coverflow *cf, int cursor, int count, int dir)
{
	if (!cf->primed) { cf_reset(cf, cursor); return; }
	if (cursor == cf->last_cursor) return;

	bool loops = cf_loops(count);
	int raw = cursor - cf->last_cursor;
	if (loops) {
		/* take the shortest way around the ring so an end<->beginning move
		 * is a single smooth step, not a slide across the whole list */
		while (raw > count / 2) raw -= count;
		while (raw < -count / 2) raw += count;
		/* A ring of TWO has both neighbors one step away, so "shortest way
		 * around" has no answer and the rule above returns the literal
		 * difference every time: +1, -1, +1, -1. The row then rocks right and
		 * left rather than turning, however the cards are drawn - which is
		 * why this looked like a drawing bug and is not one. When the caller
		 * knows which way the press was, believe it over the arithmetic. */
		if (count == 2 && dir) raw = dir;
	}
	if (raw) cf->last_dir = raw > 0 ? 1 : -1;
	cf->last_cursor = cursor;

	cf->target += (float)raw;
	/* Start the tween from where the cards ARE, not from where the last one
	 * was headed. Holding a direction retargets every key repeat, and taking
	 * the live position each time is what makes that one continuous glide
	 * rather than a stutter back to the old start. */
	cf->from = cf->pos;
	cf->t0 = SDL_GetTicks();
	cf->active = true;
}

static void step_anim(coverflow *cf, int count)
{
	bool loops = cf_loops(count);
	if (loops) {
		/* `from` rides along with pos and target through a wrap, or the
		 * interpolation below would be measuring against a point that is now
		 * a whole revolution away. */
		while (cf->pos >= (float)count) {
			cf->pos -= count; cf->target -= count; cf->from -= count;
		}
		while (cf->pos < 0.0f) {
			cf->pos += count; cf->target += count; cf->from += count;
		}
	}
	if (!cf->active) return;
	{
		float ms = cf->anim_ms > 0.0f ? cf->anim_ms : ANIM_MS;
		float u = (float)(SDL_GetTicks() - cf->t0) / ms;
		if (u >= 1.0f) {
			cf->pos = cf->target;
			cf->active = false;
			return;
		}
		cf->pos = cf->from + (cf->target - cf->from) * ease_apply(cf->ease, u);
	}
}

/* Weak-perspective projection of a card-local point. The card is yawed
 * about its vertical axis; x rotates into depth and everything is scaled
 * by F/(F+z), which squashes the far edge horizontally and vertically. */
typedef struct {
	float ox, oy;   /* card center on screen */
	float cosa, sina;
	float focal;
} cf_proj;

static void proj_point(const cf_proj *p, float lx, float ly, float *sx, float *sy)
{
	float xr = lx * p->cosa;
	float zr = lx * p->sina;
	float s = p->focal / (p->focal + zr);
	*sx = p->ox + xr * s;
	*sy = p->oy + ly * s;
}

static void render_quad(SDL_Renderer *r, SDL_Texture *tex,
                        const float xy[8], const float uv[8],
                        const SDL_Color col[4])
{
	SDL_Vertex v[4];
	for (int i = 0; i < 4; i++) {
		v[i].position.x = xy[i * 2];
		v[i].position.y = xy[i * 2 + 1];
		v[i].tex_coord.x = uv[i * 2];
		v[i].tex_coord.y = uv[i * 2 + 1];
		v[i].color = col[i];
	}
	static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
	SDL_RenderGeometry(r, tex, v, 4, idx, 6);
}

static void draw_card(SDL_Renderer *r, SDL_Texture *tex, int tw, int th,
                      float ox, float oy, float hw, float hh,
                      float ang, Uint8 alpha, const cf_layout *lay)
{
	/* CONTAIN-fit the texture inside the card frame */
	float ahw = hw, ahh = hh;
	if (tw > 0 && th > 0) {
		float tex_ar = (float)tw / (float)th;
		float frame_ar = hw / hh;
		if (tex_ar > frame_ar) ahh = hw / tex_ar;
		else ahw = hh * tex_ar;
	}

	cf_proj p = {
		.ox = ox, .oy = oy,
		.cosa = cosf(ang), .sina = sinf(ang),
		.focal = hw * 6.0f + 1.0f,
	};

	SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

	int n = lay->strips > 0 ? lay->strips : 16;
	/* The reflection reaches a common baseline (lay->reflect * hh below the
	 * card center) no matter how tall the fitted art is: short art gets a
	 * longer reflection so it reflects to the same depth as tall art. The
	 * art itself stays vertically centered. */
	float f = lay->reflect;
	if (ahh > 0.001f) {
		f = (lay->reflect * hh - ahh) / (2.0f * ahh);
		if (f < 0.02f) f = 0.02f;
		/* Cap at a full mirror: f>1 makes the reflection's far texcoord
		 * negative, which the Mali GLES driver drops (no reflection at all).
		 * A full mirror of very short/wide art still reaches nearly the same
		 * depth. */
		if (f > 1.0f) f = 1.0f;
	}
	Uint8 ra = (Uint8)(alpha * 90 / 255);
	SDL_Color body[4] = {
		{ 255, 255, 255, alpha }, { 255, 255, 255, alpha },
		{ 255, 255, 255, alpha }, { 255, 255, 255, alpha },
	};

	for (int i = 0; i < n; i++) {
		float u0 = (float)i / n, u1 = (float)(i + 1) / n;
		float lx0 = -ahw + u0 * 2.0f * ahw;
		float lx1 = -ahw + u1 * 2.0f * ahw;

		float tlx, tly, trx, try_, brx, bry, blx, bly;
		proj_point(&p, lx0, -ahh, &tlx, &tly);
		proj_point(&p, lx1, -ahh, &trx, &try_);
		proj_point(&p, lx1, +ahh, &brx, &bry);
		proj_point(&p, lx0, +ahh, &blx, &bly);

		float xy[8] = { tlx, tly, trx, try_, brx, bry, blx, bly };
		float uv[8] = { u0, 0, u1, 0, u1, 1, u0, 1 };
		render_quad(r, tex, xy, uv, body);

		if (f > 0.0f) {
			/* mirror the strip below its bottom edge, extended along the
			 * card's own (already foreshortened) vertical direction */
			float rblx = blx + f * (blx - tlx);
			float rbly = bly + f * (bly - tly);
			float rbrx = brx + f * (brx - trx);
			float rbry = bry + f * (bry - try_);
			float vb = 1.0f - f;
			float rxy[8] = { blx, bly, brx, bry, rbrx, rbry, rblx, rbly };
			float ruv[8] = { u0, 1, u1, 1, u1, vb, u0, vb };
			SDL_Color rcol[4] = {
				{ 255, 255, 255, ra }, { 255, 255, 255, ra },
				{ 255, 255, 255, 0 }, { 255, 255, 255, 0 },
			};
			render_quad(r, tex, rxy, ruv, rcol);
		}
	}
}

bool cf_draw(coverflow *cf, SDL_Renderer *r, int screen_w, int screen_h,
             int count, cf_tex_fn get_tex, void *ctx, const cf_layout *lay)
{
	if (count <= 0) return false;
	step_anim(cf, count);

	bool loops = cf_loops(count);
	int half = cf_half(count);
	float ch = screen_h * lay->size;
	float cw = ch * lay->aspect;
	float step = cw * lay->step;
	float cx = screen_w * 0.5f;
	float cy = screen_h * lay->center_y;

	int base = (int)floorf(cf->pos + 0.5f);

	/* collect visible slots, then draw far-to-near so the center card wins */
	struct slot { int item; float d; } slots[CF_WINDOW];
	int ns = 0;
	for (int k = -half; k <= half; k++) {
		int i = base + k;
		float d = (float)i - cf->pos;
		if (fabsf(d) > half + 0.5f) continue;
		int item = i;
		if (loops) {
			item = i % count;
			if (item < 0) item += count;
		} else if (i < 0 || i >= count) {
			continue;
		}
		/* One slot per item, nearest wins. With the window now allowed to be
		 * wider than the list, wrapping offers the same item on both sides -
		 * two items put the other one at -1 and +1 - and drawing it twice
		 * would be worse than the bug this replaced. */
		{
			int dup = -1, q;
			for (q = 0; q < ns; q++)
				if (slots[q].item == item) { dup = q; break; }
			if (dup >= 0) {
				float held = slots[dup].d;
				/* Nearest wins. On an exact tie - which is every resting frame
				 * of a two-item ring, where both copies sit one step out - put
				 * the card BEHIND the direction of travel, so the one you just
				 * moved past is the one you see. Choosing arbitrarily instead
				 * meant it sat left whichever way you went, and so had to jump
				 * across at the end of every leftward move. */
				if (fabsf(d) < fabsf(held) ||
				    (fabsf(d) == fabsf(held) && cf->last_dir &&
				     (d < 0) == (cf->last_dir > 0)))
					slots[dup].d = d;
				continue;
			}
		}
		slots[ns].item = item;
		slots[ns].d = d;
		ns++;
	}
	/* insertion sort by |d| descending */
	for (int a = 1; a < ns; a++) {
		struct slot s = slots[a];
		int b = a - 1;
		while (b >= 0 && fabsf(slots[b].d) < fabsf(s.d)) {
			slots[b + 1] = slots[b];
			b--;
		}
		slots[b + 1] = s;
	}

	for (int a = 0; a < ns; a++) {
		float d = slots[a].d;
		float ad = fabsf(d);
		float c = 1.0f - clampf(ad, 0.0f, 1.0f);
		float scale = lay->side_scale + (1.0f - lay->side_scale) * c;
		float ang = -lay->tilt * clampf(d, -1.0f, 1.0f);
		Uint8 alpha = (Uint8)(lay->side_alpha + (255 - lay->side_alpha) * c);
		int tw = 0, th = 0;
		SDL_Texture *tex = get_tex(ctx, slots[a].item, &tw, &th);
		if (!tex) continue;
		draw_card(r, tex, tw, th, cx + d * step, cy,
		          cw * 0.5f * scale, ch * 0.5f * scale, ang, alpha, lay);
	}
	return cf->active;
}

/* Direction unknown - the cursor moved by something other than a press, such
 * as a letter jump or a list rebuild. Only a two-item ring cares. */
void cf_set_cursor(coverflow *cf, int cursor, int count)
{
	cf_set_cursor_dir(cf, cursor, count, 0);
}

void cf_tick(coverflow *cf, int count)
{
	step_anim(cf, count);
}

/* One face of the cube. The face is a plane at radius R from a horizontal axis
 * sitting R behind the screen, turned by `phi`; every point on it is rotated
 * about that axis and then divided through by depth. Strips run down the face
 * because that is the direction depth varies once it turns. */
static void cube_face(SDL_Renderer *r, SDL_Texture *tex, float phi,
                      int screen_w, int screen_h)
{
	const int N = 14;
	float R = screen_h * 0.5f;
	/* Distance from the eye to the screen plane, and the single number that
	 * decides whether this reads as a turn or as a slide.
	 *
	 * It was 1.7x the screen height, which is nearly orthographic: the faces
	 * translated without visibly foreshortening, so the eye saw two pictures
	 * sliding past each other. 0.85x puts the eye about where it is for a
	 * screen this size and doubles the effect - at 45 degrees through the
	 * turn the leading edge magnifies 32% rather than 14%, and that growth is
	 * the cue that says the edge is coming toward you. */
	float F = screen_h * 0.85f;
	float cx = screen_w * 0.5f, cy = screen_h * 0.5f;
	float cs = cosf(phi), sn = sinf(phi);
	float lit = 0.45f + 0.55f * cosf(phi);
	Uint8 k;
	int i;

	if (cosf(phi) <= 0.0f) return;   /* turned past edge-on: facing away */
	if (lit < 0.0f) lit = 0.0f;
	k = (Uint8)(255.0f * lit);

	for (i = 0; i < N; i++) {
		float t0 = (float)i / N, t1 = (float)(i + 1) / N;
		float y0 = -R + t0 * 2.0f * R, y1 = -R + t1 * 2.0f * R;
		/* The face lies at z = -R from the axis, i.e. toward the viewer. */
		float ry0 = y0 * cs + R * sn, rz0 = y0 * sn - R * cs;
		float ry1 = y1 * cs + R * sn, rz1 = y1 * sn - R * cs;
		float s0 = F / (F + R + rz0), s1 = F / (F + R + rz1);
		float sy0 = cy + ry0 * s0, sy1 = cy + ry1 * s1;
		float hw0 = screen_w * 0.5f * s0, hw1 = screen_w * 0.5f * s1;
		float xy[8] = { cx - hw0, sy0, cx + hw0, sy0,
		                cx + hw1, sy1, cx - hw1, sy1 };
		float uv[8] = { 0, t0, 1, t0, 1, t1, 0, t1 };
		SDL_Color col[4] = { { k, k, k, 255 }, { k, k, k, 255 },
		                     { k, k, k, 255 }, { k, k, k, 255 } };

		render_quad(r, tex, xy, uv, col);
	}
}

void cf_draw_cube(SDL_Renderer *r, SDL_Texture *near_face, SDL_Texture *far_face,
                  float frac, int screen_w, int screen_h)
{
	float q = 1.5707963f;   /* a quarter turn: the faces are at right angles */
	/* Positive, so the face being left rolls DOWN and out: its top edge comes
	 * toward the viewer and pulls the rest after it. The other sign is the
	 * list convention - press down, the selection moves down, the content
	 * scrolls up past it - and it reads as backwards on something solid,
	 * because the button stops being a cursor key and becomes a push. */
	float phi = frac * q;
	/* MINUS a quarter, not plus. The face arriving is the one that was on TOP
	 * of the cube, hinged along the shared top-front edge; plus a quarter is
	 * the BOTTOM face, which puts both faces leaving by the same edge and
	 * stops the pair reading as one solid at all. */
	float far_phi = phi - q;
	/* Centre depth of each, as a fraction of the cube's half-width: the face
	 * more nearly square-on is the nearer one, and it has to be drawn last.
	 * Fixed order is only right for half the turn. */
	float dn = 1.0f - cosf(phi), df = 1.0f - cosf(far_phi);

	/* Opaque. These are whole screens, one genuinely in front of the other,
	 * and blending them lets the far face show through the near one wherever
	 * the near one is dark - which is most of a face. */
	if (near_face) SDL_SetTextureBlendMode(near_face, SDL_BLENDMODE_NONE);
	if (far_face)  SDL_SetTextureBlendMode(far_face, SDL_BLENDMODE_NONE);

	if (dn >= df) {
		if (near_face) cube_face(r, near_face, phi, screen_w, screen_h);
		if (far_face)  cube_face(r, far_face, far_phi, screen_w, screen_h);
	} else {
		if (far_face)  cube_face(r, far_face, far_phi, screen_w, screen_h);
		if (near_face) cube_face(r, near_face, phi, screen_w, screen_h);
	}
}
