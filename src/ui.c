/* SPDX-License-Identifier: 0BSD */
#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GLOW_SIZE 192      /* the glow is drawn stretched; small is enough */
#define TEXT_CACHE 12

/* The type scale: one base size and a multiplier per role. The panel is
 * 1024x768 across about three inches, so the base is set for reading at arm's
 * length on a handheld rather than for a screen at desk distance, and the
 * ratios between the roles are what keeps a count from competing with a title.
 * UI_F_CARD is in card pixels, not screen pixels -- the card face is drawn at
 * 512 wide and shown at roughly two thirds of that. */
#define FONT_BASE 32.0f
static const float font_mul[UI_F_COUNT] = {
	[UI_F_TITLE] = 1.62f,   /* 52 */
	[UI_F_MENU]  = 1.34f,   /* 43 */
	[UI_F_LABEL] = 1.50f,   /* 48 */
	[UI_F_META]  = 1.00f,   /* 32 */
	[UI_F_CARD]  = 1.94f,   /* 62, in card pixels - the title IS the card */
};

static TTF_Font *fonts[UI_F_COUNT];
static float font_scale = 1.0f;
static TTF_Font *f_mark;
static char font_path_kept[512];
static SDL_Texture *glow_tex;

/* One title is redrawn on every frame while the rest of the screen changes
 * around it. Rendering TTF text is milliseconds; blitting a texture is not.
 * A tiny most-recently-used cache turns the first into the second. */
struct text_entry {
	TTF_Font *font;
	char str[256];
	unsigned rgb;
	SDL_Texture *tex;
	int w, h;
	unsigned used;
};
static struct text_entry cache[TEXT_CACHE];
static unsigned cache_clock;

static SDL_Texture *make_glow(SDL_Renderer *r)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, GLOW_SIZE, GLOW_SIZE, 32,
	                                                SDL_PIXELFORMAT_ARGB8888);
	SDL_Texture *t;
	int x, y;
	if (!s) return NULL;
	for (y = 0; y < GLOW_SIZE; y++) {
		Uint32 *row = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
		for (x = 0; x < GLOW_SIZE; x++) {
			float dx = (x - GLOW_SIZE / 2.0f) / (GLOW_SIZE / 2.0f);
			float dy = (y - GLOW_SIZE / 2.0f) / (GLOW_SIZE / 2.0f);
			float d = sqrtf(dx * dx + dy * dy);
			float a = 1.0f - d;
			if (a < 0) a = 0;
			a = a * a * a;   /* a soft shoulder, no visible edge */
			row[x] = SDL_MapRGBA(s->format, 255, 255, 255, (Uint8)(a * 255.0f));
		}
	}
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_ADD);
	return t;
}

void ui_set_font_scale(float scale)
{
	/* Matched to the three steps the launcher offers - see TEXT_SCALES. The
	 * range used to run to 1.50, which the menu panel survived and the
	 * keyboard panel did not. A hand-edited config should not be able to
	 * reach a size nothing was checked at. */
	if (scale < 0.85f) scale = 0.85f;
	if (scale > 1.15f) scale = 1.15f;
	font_scale = scale;
}

float ui_get_font_scale(void) { return font_scale; }

bool ui_init(SDL_Renderer *r, const char *font_path)
{
	int i;

	if (TTF_Init() != 0) {
		fprintf(stderr, "ttf: %s\n", TTF_GetError());
		return false;
	}
	snprintf(font_path_kept, sizeof font_path_kept, "%s", font_path);
	for (i = 0; i < UI_F_COUNT; i++) {
		int pt = (int)(FONT_BASE * font_mul[i] * font_scale + 0.5f);
		fonts[i] = TTF_OpenFont(font_path, pt);
		if (!fonts[i])
			fprintf(stderr, "font %s @%d: %s\n", font_path, pt, TTF_GetError());
	}
	glow_tex = make_glow(r);
	return fonts[UI_F_TITLE] != NULL;
}

void ui_quit(void)
{
	for (int i = 0; i < TEXT_CACHE; i++)
		if (cache[i].tex) { SDL_DestroyTexture(cache[i].tex); cache[i].tex = NULL; }
	if (glow_tex) { SDL_DestroyTexture(glow_tex); glow_tex = NULL; }
	for (int i = 0; i < UI_F_COUNT; i++)
		if (fonts[i]) { TTF_CloseFont(fonts[i]); fonts[i] = NULL; }
	if (f_mark) { TTF_CloseFont(f_mark); f_mark = NULL; }
	TTF_Quit();
}

TTF_Font *ui_font(ui_font_role role)
{
	if (role < 0 || role >= UI_F_COUNT) return NULL;
	return fonts[role];
}

int ui_font_line(ui_font_role role)
{
	TTF_Font *f = ui_font(role);
	return f ? TTF_FontLineSkip(f) : 0;
}

static struct text_entry *text_get(SDL_Renderer *r, TTF_Font *f, const char *s,
                                  SDL_Color col)
{
	unsigned rgb = ((unsigned)col.r << 16) | ((unsigned)col.g << 8) | col.b;
	int i, oldest = 0;

	if (!f || !s || !*s) return NULL;
	for (i = 0; i < TEXT_CACHE; i++) {
		if (cache[i].tex && cache[i].font == f && cache[i].rgb == rgb &&
		    strcmp(cache[i].str, s) == 0) {
			cache[i].used = ++cache_clock;
			return &cache[i];
		}
		if (!cache[i].tex) { oldest = i; goto fill; }
		if (cache[i].used < cache[oldest].used) oldest = i;
	}
fill:
	if (cache[oldest].tex) SDL_DestroyTexture(cache[oldest].tex);
	memset(&cache[oldest], 0, sizeof cache[oldest]);
	{
		SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, col);
		if (!surf) return NULL;
		cache[oldest].tex = SDL_CreateTextureFromSurface(r, surf);
		cache[oldest].w = surf->w;
		cache[oldest].h = surf->h;
		SDL_FreeSurface(surf);
	}
	if (!cache[oldest].tex) return NULL;
	cache[oldest].font = f;
	cache[oldest].rgb = rgb;
	snprintf(cache[oldest].str, sizeof cache[oldest].str, "%s", s);
	cache[oldest].used = ++cache_clock;
	return &cache[oldest];
}

int ui_text(SDL_Renderer *r, TTF_Font *f, const char *s, int x, int y,
            int anchor, SDL_Color col)
{
	struct text_entry *e = text_get(r, f, s, col);
	SDL_Rect dst;
	if (!e) return 0;
	dst.w = e->w;
	dst.h = e->h;
	dst.x = anchor == 0 ? x - e->w / 2 : (anchor > 0 ? x - e->w : x);
	dst.y = y;
	SDL_SetTextureAlphaMod(e->tex, col.a);
	SDL_RenderCopy(r, e->tex, NULL, &dst);
	return e->w;
}

int ui_text_width(TTF_Font *f, const char *s)
{
	int w = 0;
	if (f && s) TTF_SizeUTF8(f, s, &w, NULL);
	return w;
}

/* Tuned by eye, and the first is the one that matters - see ui.h.
 *
 * 70 px/s is slow enough to read at arm's length on a three-inch panel and
 * fast enough that a 300px overrun is done in four seconds. The pause at the
 * far end stops the reversal reading as jitter: without it the text arrives
 * and instantly leaves, which looks like a glitch rather than an end. */
#define MQ_FADE_PX     36          /* how far the edges dissolve */
#define MQ_FADE_STEP    3
#define MQ_HOLD_MS   1400          /* stillness before it starts */
#define MQ_END_MS     900          /* stillness at the far end */
#define MQ_SPEED_PXPS  70

void ui_text_marquee(SDL_Renderer *r, TTF_Font *f, const char *s,
                     int x, int y, int w, unsigned phase, SDL_Color col)
{
	int tw = ui_text_width(f, s);
	int over, travel, off, i;
	unsigned cycle, p;
	SDL_Rect clip, was;
	SDL_bool had;

	if (!f || !s || w <= 0) return;
	if (tw <= w) { ui_text(r, f, s, x, y, -1, col); return; }

	over   = tw - w;
	travel = over * 1000 / MQ_SPEED_PXPS;
	if (travel < 1) travel = 1;
	cycle  = (unsigned)(MQ_HOLD_MS + travel + MQ_END_MS + travel);
	p      = phase % cycle;

	if      (p < MQ_HOLD_MS)                    off = 0;
	else if (p < (unsigned)(MQ_HOLD_MS + travel))
		off = (int)((p - MQ_HOLD_MS) * (unsigned)over / (unsigned)travel);
	else if (p < (unsigned)(MQ_HOLD_MS + travel + MQ_END_MS))
		off = over;
	else
		off = over - (int)((p - MQ_HOLD_MS - travel - MQ_END_MS)
		                   * (unsigned)over / (unsigned)travel);

	/* Nested clips: the caller may already have one, and dropping it would
	 * let this draw outside whatever panel it sits in. */
	had = SDL_RenderIsClipEnabled(r);
	if (had) SDL_RenderGetClipRect(r, &was);
	clip.x = x; clip.y = y;
	clip.w = w; clip.h = TTF_FontHeight(f);
	SDL_RenderSetClipRect(r, &clip);

	/* Faded at both edges, and faded in the TEXT rather than by laying a
	 * gradient of the background over it. The background here is a coverflow
	 * with a vignette, not a flat color, so anything painted on top would
	 * show as a band. Fading the glyphs works over whatever is behind them.
	 *
	 * Without it the text is chopped mid-stroke - and on the shelf the left
	 * chop lands right beside the heart, where it reads as damage rather than
	 * as more text.
	 *
	 * EACH SLICE IS DRAWN EXACTLY ONCE, at its own alpha, with the clip
	 * deciding which part of the text lands. The first attempt drew the whole
	 * string at full alpha and then re-drew the edges dimmer on top, which
	 * cannot subtract - blending only adds - and used BLENDMODE_NONE to try to
	 * force it, which replaces the destination wholesale and painted two solid
	 * white blocks where the fades should have been. */
	{
		struct text_entry *e = text_get(r, f, s, col);
		SDL_Rect dst;

		if (!e) { SDL_RenderSetClipRect(r, had ? &was : NULL); return; }
		dst.x = x - off; dst.y = y; dst.w = e->w; dst.h = e->h;

		/* A FADE MEANS "MORE TEXT THIS WAY", so each side only fades when
		 * there is something hidden on it. Both faded unconditionally at
		 * first, which put a soft left edge on a title resting at its
		 * beginning - nothing was hidden there, and it read as the name
		 * starting halfway through a word.
		 *
		 * The width is the amount hidden, capped. So it grows from nothing as
		 * the text pulls away and shrinks back to nothing as it returns,
		 * which also means neither end pops. */
		int lf = off < MQ_FADE_PX ? off : MQ_FADE_PX;
		int rf = (over - off) < MQ_FADE_PX ? (over - off) : MQ_FADE_PX;

		clip.x = x + lf;
		clip.w = w - lf - rf;
		if (clip.w > 0) {
			SDL_RenderSetClipRect(r, &clip);
			SDL_SetTextureAlphaMod(e->tex, col.a);
			SDL_RenderCopy(r, e->tex, NULL, &dst);
		}

		for (i = 0; i < lf; i += MQ_FADE_STEP) {
			SDL_SetTextureAlphaMod(e->tex, (Uint8)(col.a * i / lf));
			clip.x = x + i;
			clip.w = lf - i < MQ_FADE_STEP ? lf - i : MQ_FADE_STEP;
			SDL_RenderSetClipRect(r, &clip);
			SDL_RenderCopy(r, e->tex, NULL, &dst);
		}
		for (i = 0; i < rf; i += MQ_FADE_STEP) {
			int sw = rf - i < MQ_FADE_STEP ? rf - i : MQ_FADE_STEP;

			SDL_SetTextureAlphaMod(e->tex, (Uint8)(col.a * i / rf));
			clip.x = x + w - sw - i;
			clip.w = sw;
			SDL_RenderSetClipRect(r, &clip);
			SDL_RenderCopy(r, e->tex, NULL, &dst);
		}
		SDL_SetTextureAlphaMod(e->tex, 255);
	}
	SDL_RenderSetClipRect(r, had ? &was : NULL);
}

void ui_fit_text(TTF_Font *f, const char *src, char *dst, size_t dstn,
                 int maxw)
{
	size_t n;

	snprintf(dst, dstn, "%s", src ? src : "");
	if (!f || ui_text_width(f, dst) <= maxw) return;

	n = strlen(dst);
	while (n > 0) {
		n--;
		while (n > 0 && ((unsigned char)dst[n] & 0xC0) == 0x80) n--;
		if (n + 3 >= dstn) continue;
		memcpy(dst + n, "...", 4);
		if (ui_text_width(f, dst) <= maxw) return;
		dst[n] = '\0';
	}
}

void ui_glow(SDL_Renderer *r, const SDL_Rect *rect, unsigned rgb, int alpha,
             float spread)
{
	SDL_Rect dst;
	int gw, gh;
	if (!glow_tex) return;
	gw = (int)(rect->w * spread);
	gh = (int)(rect->h * spread);
	dst.x = rect->x + rect->w / 2 - gw / 2;
	dst.y = rect->y + rect->h / 2 - gh / 2;
	dst.w = gw;
	dst.h = gh;
	SDL_SetTextureColorMod(glow_tex, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb);
	SDL_SetTextureAlphaMod(glow_tex, (Uint8)alpha);
	SDL_RenderCopy(r, glow_tex, NULL, &dst);
}

void ui_rail(SDL_Renderer *r, int screen_w, int screen_h, int index, int count,
             unsigned rgb)
{
	/* Grown upward from where the old 3px bar's bottom edge sat, so matching
	 * the settings line's weight did not also move the rail. */
	int h = UI_BAR_H, y = screen_h - 23 - h;
	int track_x = 90, track_w = screen_w - track_x * 2;
	int seg_w, seg_x;

	if (count <= 1) return;
	seg_w = track_w / count;
	if (seg_w < 18) seg_w = 18;
	seg_x = track_x + (int)((float)index / (float)(count - 1) * (track_w - seg_w));

	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	/* The track carries twice the weight it used to, so it takes less alpha to
	 * say the same thing; brighter than this and an empty rail reads as a
	 * drawn element rather than as the absence of one. */
	SDL_SetRenderDrawColor(r, 255, 255, 255, 16);
	SDL_RenderFillRect(r, &(SDL_Rect){ track_x, y, track_w, h });
	SDL_SetRenderDrawColor(r, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 235);
	SDL_RenderFillRect(r, &(SDL_Rect){ seg_x, y, seg_w, h });
}

void ui_round_rect(SDL_Renderer *r, const SDL_Rect *q, int radius, SDL_Color col)
{
	int y;

	if (q->w <= 0 || q->h <= 0) return;
	if (radius * 2 > q->w) radius = q->w / 2;
	if (radius * 2 > q->h) radius = q->h / 2;
	if (radius < 0) radius = 0;

	SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, col.r, col.g, col.b, col.a);
	SDL_RenderFillRect(r, &(SDL_Rect){ q->x, q->y + radius, q->w, q->h - radius * 2 });
	for (y = 0; y < radius; y++) {
		int dy = radius - y;
		int dx = radius - (int)(sqrt((double)(radius * radius - dy * dy)) + 0.5);
		SDL_RenderFillRect(r, &(SDL_Rect){ q->x + dx, q->y + y, q->w - dx * 2, 1 });
		SDL_RenderFillRect(r, &(SDL_Rect){ q->x + dx, q->y + q->h - 1 - y,
		                                   q->w - dx * 2, 1 });
	}
}

/* The border is the system's color at full strength, not a wash of it. At
 * alpha 110 over a near-black background it read as a darker version of the
 * accent rather than the accent, so a system's identity barely reached the one
 * piece of chrome that frames everything it does. Twelve pixels rather than two
 * for the same reason: at two it was a hairline that the eye resolved as gray.
 *
 * Twelve specifically, matching the save/load frame's `bw`, so the two pieces
 * of accent chrome a player sees are the same weight rather than nearly so. */

void ui_panel(SDL_Renderer *r, const SDL_Rect *q, int radius, unsigned border)
{
	const int bw = UI_PANEL_BORDER;
	SDL_Rect in = { q->x + bw, q->y + bw, q->w - bw * 2, q->h - bw * 2 };

	ui_round_rect(r, q, radius, (SDL_Color){
		(Uint8)(border >> 16), (Uint8)(border >> 8), (Uint8)border, 255 });
	/* Opaque enough that a card title behind it does not ghost through the
	 * list, which at 95% it did. Lifted off the background's own near-black:
	 * at 10,11,16 the panel was a hole in the screen rather than a surface on
	 * it, and every row drawn on it inherited that as looking unlit. */
	ui_round_rect(r, &in, radius - bw, (SDL_Color){ 22, 24, 32, 252 });
}

unsigned ui_mix(unsigned a, unsigned b, float t)
{
	int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
	int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
	int rr = (int)(ar + (br - ar) * t);
	int rg = (int)(ag + (bg - ag) * t);
	int rb = (int)(ab + (bb - ab) * t);
	return ((unsigned)rr << 16) | ((unsigned)rg << 8) | (unsigned)rb;
}

/* ---- the generated card ------------------------------------------------- */

#define CARD_W 512
#define CARD_H 656
#define CARD_RADIUS 22

static void round_corners(SDL_Surface *s, int radius)
{
	int x, y;
	for (y = 0; y < radius; y++) {
		Uint32 *top = (Uint32 *)((Uint8 *)s->pixels + (size_t)y * s->pitch);
		Uint32 *bot = (Uint32 *)((Uint8 *)s->pixels + (size_t)(s->h - 1 - y) * s->pitch);
		for (x = 0; x < radius; x++) {
			int dx = radius - x, dy = radius - y;
			if (dx * dx + dy * dy > radius * radius) {
				top[x] = 0; top[s->w - 1 - x] = 0;
				bot[x] = 0; bot[s->w - 1 - x] = 0;
			}
		}
	}
}

static void blit_line(SDL_Surface *dst, TTF_Font *f, const char *line, int *y,
                      int x)
{
	SDL_Surface *t = TTF_RenderUTF8_Blended(f, line, UI_TEXT);
	if (!t) return;
	SDL_BlitSurface(t, NULL, dst,
	                &(SDL_Rect){ x < 0 ? (dst->w - t->w) / 2 : x, *y, 0, 0 });
	*y += t->h + 4;
	SDL_FreeSurface(t);
}

/* Break a title across lines that fit the card. `x` is where each line starts,
 * or -1 to centre them. The line is built by appending in place and undoing the
 * append when it no longer fits, so there is no second buffer that could
 * truncate the first. Returns the y below the last line, which is where a rule
 * under the title goes. */
static int draw_wrapped(SDL_Surface *dst, TTF_Font *f, const char *title,
                        int box_w, int top_y, int x)
{
	char line[256];
	char word[96];
	const char *p = title;
	size_t len = 0;
	int y = top_y;
	int lines = 0;

	line[0] = '\0';
	while (*p && lines < 4) {
		size_t n = 0, keep;
		int w = 0;

		while (*p == ' ') p++;
		while (*p && *p != ' ' && n + 1 < sizeof word) word[n++] = *p++;
		while (*p && *p != ' ') p++;          /* drop the tail of a huge word */
		word[n] = '\0';
		if (!n) break;

		keep = len;
		if (len + (len ? 1 : 0) + n + 1 > sizeof line) {
			/* the line cannot hold another word at all */
			if (len) { blit_line(dst, f, line, &y, x); lines++; }
			snprintf(line, sizeof line, "%s", word);
			len = n;
			continue;
		}
		if (len) line[len++] = ' ';
		memcpy(line + len, word, n);
		len += n;
		line[len] = '\0';

		TTF_SizeUTF8(f, line, &w, NULL);
		if (w > box_w && keep) {
			line[keep] = '\0';                /* undo the append */
			blit_line(dst, f, line, &y, x);
			lines++;
			snprintf(line, sizeof line, "%s", word);
			len = n;
		}
	}
	if (line[0] && lines < 4) blit_line(dst, f, line, &y, x);
	return y;
}

/* The first letter of the title, enormous and barely there, running off the
 * bottom-right corner.
 *
 * It used to sit centred and upright at a third of the way down, which made it
 * the largest thing on the card - and on an alphabetised shelf it is the least
 * distinguishing: Castlevania, Contra and Crystalis sit next to each other and
 * were three identical Cs with the titles that tell them apart set small
 * underneath. Bled off the corner it is what it always was, a texture in the
 * system's colour, and the title can have the space. */
static void draw_watermark(SDL_Surface *dst, const char *title, unsigned rgb)
{
	char ch[2] = { 0, 0 };
	const char *p = title;
	SDL_Surface *t;

	while (*p && (unsigned char)*p <= ' ') p++;
	if (!*p) return;
	ch[0] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
	if ((unsigned char)ch[0] > 127) return;   /* one glyph, and an ASCII one */

	if (!f_mark && font_path_kept[0])
		f_mark = TTF_OpenFont(font_path_kept, 560);
	if (!f_mark) return;

	t = TTF_RenderUTF8_Blended(f_mark, ch, (SDL_Color){
		(Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 55 });
	if (!t) return;
	SDL_SetSurfaceBlendMode(t, SDL_BLENDMODE_BLEND);
	/* Deliberately past both edges: what is wanted is the shoulder of the
	 * letter, not the letter. */
	SDL_BlitSurface(t, NULL, dst, &(SDL_Rect){
		dst->w - (int)(t->w * 0.62f), CARD_H - (int)(t->h * 0.80f), 0, 0 });
	SDL_FreeSurface(t);
}

SDL_Texture *ui_make_card(SDL_Renderer *r, const char *title, unsigned rgb,
                          int *w, int *h)
{
	SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, CARD_W, CARD_H, 32,
	                                                SDL_PIXELFORMAT_ARGB8888);
	SDL_Texture *t;
	int y;
	if (!s) return NULL;

	/* A vertical gradient, darker at the foot, so a wall of generated cards
	 * still has some depth to it. */
	for (y = 0; y < CARD_H; y++) {
		float k = (float)y / CARD_H;
		Uint8 v = (Uint8)(38 - 18 * k);
		SDL_FillRect(s, &(SDL_Rect){ 0, y, CARD_W, 1 },
		             SDL_MapRGBA(s->format, (Uint8)(v * 0.86f), (Uint8)(v * 0.92f),
		                         (Uint8)(v * 1.20f), 255));
	}

	draw_watermark(s, title, rgb);

	/* The system's color as a band rather than a wash: a generated card
	 * should read as "this system, no art" at a glance in the row. */
	SDL_FillRect(s, &(SDL_Rect){ 0, 0, CARD_W, 6 },
	             SDL_MapRGBA(s->format, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8),
	                         (Uint8)rgb, 255));

	/* The title at the top and hard left, because that is the edge the eye
	 * runs down when the row is moving. A rule under it rather than across
	 * the card: it is the end of the name, not a divider between halves. */
	if (fonts[UI_F_CARD]) {
		/* Not at the top edge. Started at 76 it left the bottom half of the
		 * card empty for the one-line titles that are most of a shelf, and the
		 * bleed does not fill it - the face darkens toward the foot and takes
		 * the letter's tail with it. Four lines still fit below this. */
		int y = draw_wrapped(s, fonts[UI_F_CARD], title, CARD_W - 96,
		                     (int)(CARD_H * 0.38f), 48);

		SDL_FillRect(s, &(SDL_Rect){ 48, y + 12, 90, 3 },
		             SDL_MapRGBA(s->format, (Uint8)(rgb >> 16),
		                         (Uint8)(rgb >> 8), (Uint8)rgb, 220));
	}

	round_corners(s, CARD_RADIUS);
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	if (t) { *w = CARD_W; *h = CARD_H; SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
	return t;
}
