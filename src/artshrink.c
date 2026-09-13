/* SPDX-License-Identifier: MIT */
/* Shrinking fetched box art to the size it is actually drawn at.
 *
 * Its own file rather than part of artscrape.c, for two reasons. artscrape is
 * about naming and fetching and has no other business with pixels; and it is
 * compiled standalone by check-artscrape, which is an offline check that must
 * not need SDL to build. */
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <SDL.h>
#include <SDL_image.h>
#include "artshrink.h"

#define ARTSHRINK_PATH_MAX 2048


/* The longest edge a cover is ever drawn at, plus nothing.
 *
 * The shelf card is 332x461, and equal-area sizing can take a landscape cover
 * to 464 wide, so 512 covers the largest draw with a little spare. The launch
 * zoom passes that, but it is a 200ms accelerating fade and softness there is
 * not visible. */
#define ART_MAX_PX 512

/* Shrink a freshly fetched cover to the size it is actually drawn at.
 *
 * libretro ships art at its own resolution and TortOS never had an opinion
 * about it. Measured on this card 2026-09-12: NES covers arrive 512x731 and
 * Genesis 479x680, against a card that draws at most 332x461 - two to three
 * times the pixels that can ever reach the screen. That is not free, because
 * game_get_tex decodes on the render path: 7-27ms per card on this device
 * against a 16.7ms frame, every time one scrolls past.
 *
 * Never upscales. Art already inside the box is left exactly as it arrived.
 *
 * Area-average rather than a 2x2 bilinear. At these ratios bilinear samples
 * too few source pixels and softens exactly what a box cover is made of -
 * text, logos and hard edges. Averaging straight RGBA is only correct while
 * alpha is constant, which for box art it is: of 314 SNES covers sampled,
 * none carried a meaningful alpha channel.
 *
 * Written to a temporary and renamed, so a power cut during the encode leaves
 * the original rather than half a file. */
static void shrink_now(const char *path)
{
	SDL_Surface *raw, *src, *dst;
	char tmp[ARTSHRINK_PATH_MAX];
	Uint32 t0 = SDL_GetTicks();
	int tw, th, x, y;
	double k, sx, sy;

	if (!(raw = IMG_Load(path))) return;
	if (raw->w <= ART_MAX_PX && raw->h <= ART_MAX_PX) {
		SDL_FreeSurface(raw);
		return;
	}
	k  = (double)ART_MAX_PX / (raw->w > raw->h ? raw->w : raw->h);
	tw = (int)(raw->w * k + 0.5); if (tw < 1) tw = 1;
	th = (int)(raw->h * k + 0.5); if (th < 1) th = 1;

	src = SDL_ConvertSurfaceFormat(raw, SDL_PIXELFORMAT_ARGB8888, 0);
	SDL_FreeSurface(raw);
	if (!src) return;
	dst = SDL_CreateRGBSurfaceWithFormat(0, tw, th, 32, SDL_PIXELFORMAT_ARGB8888);
	if (!dst) { SDL_FreeSurface(src); return; }

	sx = (double)src->w / tw;
	sy = (double)src->h / th;
	for (y = 0; y < th; y++) {
		Uint32 *drow = (Uint32 *)((Uint8 *)dst->pixels + y * dst->pitch);
		int y0 = (int)(y * sy), y1 = (int)((y + 1) * sy);

		if (y1 <= y0) y1 = y0 + 1;
		if (y1 > src->h) y1 = src->h;
		for (x = 0; x < tw; x++) {
			unsigned a = 0, rr = 0, gg = 0, bb = 0, n = 0;
			int x0 = (int)(x * sx), x1 = (int)((x + 1) * sx), xx, yy;

			if (x1 <= x0) x1 = x0 + 1;
			if (x1 > src->w) x1 = src->w;
			for (yy = y0; yy < y1; yy++) {
				const Uint32 *srow = (const Uint32 *)
					((Uint8 *)src->pixels + yy * src->pitch);
				for (xx = x0; xx < x1; xx++) {
					Uint32 px = srow[xx];
					a  += (px >> 24) & 0xFF;
					rr += (px >> 16) & 0xFF;
					gg += (px >>  8) & 0xFF;
					bb +=  px        & 0xFF;
					n++;
				}
			}
			drow[x] = n ? ((a / n) << 24 | (rr / n) << 16 |
			               (gg / n) << 8 | (bb / n)) : 0;
		}
	}

	/* THE OUTPUT IS RGBA AND CANNOT BE MADE RGB HERE. Every cover libretro
	 * ships as RGB gains an alpha plane of constant 255 on the way through,
	 * and that is worth about 20% of every later decode: measured on the
	 * device 2026-09-12, the same cover is 14ms as RGBA and 11ms as RGB, plus
	 * a millisecond of conversion the RGB one does not pay.
	 *
	 * It is IMG_SavePNG, not this function. It converts whatever it is handed
	 * to RGBA32 before writing, with no flag to stop it; handing it an RGB24
	 * surface and the original RGB24 image produced byte-identical RGBA files.
	 * Getting RGB out means writing the PNG here instead - chunk framing, CRCs
	 * and adaptive row filtering, against a zlib the sysroot does not carry
	 * yet. Deliberately not done: a hand-rolled encoder that is subtly wrong
	 * corrupts art silently, and decoding is about to move off the render
	 * thread, after which 3ms of decode stops being what anyone feels. */
	if (snprintf(tmp, sizeof tmp, "%s.tmp", path) < (int)sizeof tmp &&
	    IMG_SavePNG(dst, tmp) == 0) {
		fprintf(stderr, "art: %dx%d -> %dx%d %ums %s\n",
		        src->w, src->h, tw, th, SDL_GetTicks() - t0,
		        strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
		if (rename(tmp, path) != 0) unlink(tmp);
	} else {
		unlink(tmp);
	}
	SDL_FreeSurface(dst);
	SDL_FreeSurface(src);
}


/* Fork and return. The caller is the Box Art screen's frame loop, which is
 * where the power button is read, so nothing there may wait: net_get_async
 * forks curl for exactly this reason and this is the same bargain. A resize
 * that blocked would also stall the next download, because there is ONE async
 * slot - the link would sit idle through every resize and a full run would
 * cost downloads PLUS resizes rather than the larger of the two.
 *
 * Double fork, so the grandchild is reparented to init and reaped there. The
 * middle child exits at once, so the waitpid below returns immediately and
 * leaves no zombie; tracking the real worker would mean a table and a reaping
 * pass for work whose only failure mode is a cover staying large.
 *
 * The grandchild touches surfaces, libpng and the card - never the renderer,
 * never the display - and leaves by _exit so it runs no atexit handler and
 * cannot tear down state the parent is still using. Everything it writes goes
 * through a temporary and a rename, so the parent reading the same cover mid
 * resize sees the old file or the new one and never a partial one.
 *
 * If the fork fails, do it inline rather than skip it. That is slow, but a
 * launcher that cannot fork is one where curl cannot start either, so the
 * scrape has larger problems than a stalled frame. */
void art_shrink(const char *path)
{
	pid_t mid = fork();

	if (mid < 0) { shrink_now(path); return; }
	if (mid == 0) {
		if (fork() == 0) { shrink_now(path); _exit(0); }
		_exit(0);
	}
	waitpid(mid, NULL, 0);
}
