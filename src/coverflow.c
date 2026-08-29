/* SPDX-License-Identifier: 0BSD */
#include "coverflow.h"

#include <math.h>
#include <string.h>

/* The systems row: three cards, so they are large and the neighbours sit
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

void cf_set_cursor(coverflow *cf, int cursor, int count)
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
	}
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
		float u = (float)(SDL_GetTicks() - cf->t0) / ANIM_MS;
		if (u >= 1.0f) {
			cf->pos = cf->target;
			cf->active = false;
			return;
		}
		cf->pos = cf->from + (cf->target - cf->from) * ease_out(u);
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
				if (fabsf(d) < fabsf(slots[dup].d)) slots[dup].d = d;
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
