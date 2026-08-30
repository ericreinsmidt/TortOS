/* SPDX-License-Identifier: 0BSD */
/* See notice.h for why the launcher draws this and Diatom shows it. */
#include <SDL.h>
#include <SDL_ttf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "notice.h"
#include "ui.h"

#define NOTICE_MAX_W 1024        /* Diatom refuses anything wider */
#define NOTICE_MAX_H 256

/* One pixel of the destination, straight alpha, BGRA byte order - which is
 * what SDL_PIXELFORMAT_ARGB8888 already is in memory on a little-endian host,
 * and both ends of this are little-endian. */
static void blend(uint8_t *dst, unsigned b, unsigned g, unsigned r, unsigned a)
{
	if (a == 0) return;
	if (a == 255) {
		dst[0] = (uint8_t)b; dst[1] = (uint8_t)g;
		dst[2] = (uint8_t)r; dst[3] = 255;
		return;
	}
	dst[0] = (uint8_t)((b * a + dst[0] * (255 - a) + 127) / 255);
	dst[1] = (uint8_t)((g * a + dst[1] * (255 - a) + 127) / 255);
	dst[2] = (uint8_t)((r * a + dst[2] * (255 - a) + 127) / 255);
	dst[3] = (uint8_t)(a + dst[3] * (255 - a) / 255);
}

static void draw_text(uint8_t *out, int ow, int oh, TTF_Font *f,
                      const char *s, int x, int y, SDL_Color col)
{
	SDL_Surface *t;
	int sx, sy;

	if (!f || !s || !*s) return;
	t = TTF_RenderUTF8_Blended(f, s, col);
	if (!t) return;

	/* Blended gives ARGB8888 with per-pixel alpha - the antialiasing is IN
	 * the alpha, so ignoring it would give the text hard jagged edges over
	 * the game. */
	for (sy = 0; sy < t->h; sy++) {
		const uint8_t *row = (const uint8_t *)t->pixels + (size_t)sy * t->pitch;
		int dy = y + sy;

		if (dy < 0 || dy >= oh) continue;
		for (sx = 0; sx < t->w; sx++) {
			int dx = x + sx;

			if (dx < 0 || dx >= ow) continue;
			blend(out + ((size_t)dy * ow + dx) * 4,
			      row[sx * 4 + 0], row[sx * 4 + 1],
			      row[sx * 4 + 2], row[sx * 4 + 3]);
		}
	}
	SDL_FreeSurface(t);
}

bool notice_render(const char *heading, const char *body, const char *path)
{
	/* The NAME is the line worth reading, so it gets the larger face and the
	 * brighter ink; "Achievement unlocked" is context and sits above it,
	 * smaller and dimmer. These were the other way round in the first
	 * version, which put the least interesting words in the biggest type -
	 * obvious the moment it was rendered and looked at, and invisible while
	 * it was only being reasoned about. UI_F_LABEL is 48 and UI_F_MENU 43. */
	TTF_Font *fh = ui_font(UI_F_MENU), *fb = ui_font(UI_F_LABEL);
	int pad, w, h, x, y, corner;
	uint8_t *px;
	FILE *f;
	unsigned char hdr[8];
	const SDL_Color WHITE = { 235, 235, 240, 255 };
	const SDL_Color DIM   = { 128, 168, 190, 255 };

	if (!fb) return false;
	if (!fh) fh = fb;

	pad = ui_font_line(UI_F_MENU) / 2;
	w = ui_text_width(fb, body ? body : "");
	x = ui_text_width(fh, heading ? heading : "");
	if (x > w) w = x;
	w += pad * 2;
	h = ui_font_line(UI_F_MENU) + ui_font_line(UI_F_LABEL) + pad * 2;
	if (w > NOTICE_MAX_W) w = NOTICE_MAX_W;
	if (h > NOTICE_MAX_H) h = NOTICE_MAX_H;
	if (w <= 0 || h <= 0) return false;

	px = calloc((size_t)w * (size_t)h, 4);
	if (!px) return false;

	/* A dark slab at 85%, corners knocked off. Not a rounded rectangle with
	 * antialiased arcs - this sits over a moving picture for four seconds and
	 * a diagonal reads as a corner from a metre away. */
	corner = pad;
	for (y = 0; y < h; y++) {
		for (x = 0; x < w; x++) {
			int dx = x < corner ? corner - x : (x >= w - corner ? x - (w - corner - 1) : 0);
			int dy = y < corner ? corner - y : (y >= h - corner ? y - (h - corner - 1) : 0);

			if (dx + dy > corner) continue;          /* the clipped corner */
			blend(px + ((size_t)y * w + x) * 4, 26, 22, 18, 217);
		}
	}

	draw_text(px, w, h, fh, heading, pad, pad - pad / 4, DIM);
	draw_text(px, w, h, fb, body, pad, pad + ui_font_line(UI_F_MENU) - pad / 4, WHITE);

	f = fopen(path, "wb");
	if (!f) { free(px); return false; }
	memcpy(hdr, "DTOV", 4);
	hdr[4] = (unsigned char)w;  hdr[5] = (unsigned char)(w >> 8);
	hdr[6] = (unsigned char)h;  hdr[7] = (unsigned char)(h >> 8);
	if (fwrite(hdr, 1, sizeof hdr, f) != sizeof hdr ||
	    fwrite(px, 4, (size_t)w * (size_t)h, f) != (size_t)w * (size_t)h) {
		fclose(f);
		free(px);
		return false;
	}
	fclose(f);
	free(px);
	return true;
}
