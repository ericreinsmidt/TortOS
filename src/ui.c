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
	[UI_F_META]  = 0.88f,   /* 28 */
	[UI_F_CARD]  = 1.38f,   /* 44, in card pixels */
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
	/* The ceiling is where the longest menu label still fits across the panel;
	 * past it the two columns start colliding rather than merely being large. */
	if (scale < 0.75f) scale = 0.75f;
	if (scale > 1.50f) scale = 1.50f;
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

void ui_panel(SDL_Renderer *r, const SDL_Rect *q, int radius, unsigned border)
{
	SDL_Rect in = { q->x + 2, q->y + 2, q->w - 4, q->h - 4 };

	ui_round_rect(r, q, radius, (SDL_Color){
		(Uint8)(border >> 16), (Uint8)(border >> 8), (Uint8)border, 110 });
	/* Opaque enough that a card title behind it does not ghost through the
	 * list, which at 95% it did. Lifted off the background's own near-black:
	 * at 10,11,16 the panel was a hole in the screen rather than a surface on
	 * it, and every row drawn on it inherited that as looking unlit. */
	ui_round_rect(r, &in, radius - 2, (SDL_Color){ 22, 24, 32, 252 });
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

static void blit_line(SDL_Surface *dst, TTF_Font *f, const char *line, int *y)
{
	SDL_Surface *t = TTF_RenderUTF8_Blended(f, line, UI_TEXT);
	if (!t) return;
	SDL_BlitSurface(t, NULL, dst, &(SDL_Rect){ (dst->w - t->w) / 2, *y, 0, 0 });
	*y += t->h + 4;
	SDL_FreeSurface(t);
}

/* Break a title across lines that fit the card and draw them centred. The
 * line is built by appending in place and undoing the append when it no
 * longer fits, so there is no second buffer that could truncate the first. */
static void draw_wrapped(SDL_Surface *dst, TTF_Font *f, const char *title,
                         int box_w, int top_y)
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
			if (len) { blit_line(dst, f, line, &y); lines++; }
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
			blit_line(dst, f, line, &y);
			lines++;
			snprintf(line, sizeof line, "%s", word);
			len = n;
		}
	}
	if (line[0] && lines < 4) blit_line(dst, f, line, &y);
}

/* The first letter of the title, enormous and barely there. It gives a card
 * with no art a silhouette of its own, so a row of generated cards is still
 * telling you something as it slides past rather than being a row of identical
 * slabs. */
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
		f_mark = TTF_OpenFont(font_path_kept, 340);
	if (!f_mark) return;

	t = TTF_RenderUTF8_Blended(f_mark, ch, (SDL_Color){
		(Uint8)(rgb >> 16), (Uint8)(rgb >> 8), (Uint8)rgb, 46 });
	if (!t) return;
	SDL_SetSurfaceBlendMode(t, SDL_BLENDMODE_BLEND);
	SDL_BlitSurface(t, NULL, dst, &(SDL_Rect){
		(dst->w - t->w) / 2, (int)(CARD_H * 0.30f) - t->h / 2, 0, 0 });
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

	/* The system's colour as a band rather than a wash: a generated card
	 * should read as "this system, no art" at a glance in the row. */
	SDL_FillRect(s, &(SDL_Rect){ 0, 0, CARD_W, 6 },
	             SDL_MapRGBA(s->format, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8),
	                         (Uint8)rgb, 255));
	SDL_FillRect(s, &(SDL_Rect){ 52, (int)(CARD_H * 0.615f), CARD_W - 104, 2 },
	             SDL_MapRGBA(s->format, (Uint8)(rgb >> 16), (Uint8)(rgb >> 8),
	                         (Uint8)rgb, 170));

	if (fonts[UI_F_CARD])
		draw_wrapped(s, fonts[UI_F_CARD], title, CARD_W - 76, (int)(CARD_H * 0.655f));

	round_corners(s, CARD_RADIUS);
	t = SDL_CreateTextureFromSurface(r, s);
	SDL_FreeSurface(s);
	if (t) { *w = CARD_W; *h = CARD_H; SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); }
	return t;
}
