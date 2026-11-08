/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The updater's screens as files, to look at beside their mockups:
 *
 *     make updater-shots
 *
 * Composed by tools/tortos-update/screen.c exactly as on a device, written
 * as BMPs into build-native/updater-shots/: Writing at 0, 35 and 100%,
 * Checking at 60%, the stopped-while-writing screen, and Writing at 35% as
 * the GKD Pixel 2's portrait framebuffer gets it, turned. */
#include <stdio.h>
#include <stdlib.h>

#include "screen.h"
#include "art.h"

static void bmp(const char *path, const uint32_t *px, int w, int h)
{
	int row = w * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * h;
	unsigned char head[54] = { 'B', 'M' };
	FILE *f = fopen(path, "wb");

	if (!f) { perror(path); exit(1); }
	head[2] = size; head[3] = size >> 8; head[4] = size >> 16; head[5] = size >> 24;
	head[10] = 54; head[14] = 40;
	head[18] = w; head[19] = w >> 8; head[22] = h; head[23] = h >> 8;
	head[26] = 1; head[28] = 24;
	fwrite(head, 1, 54, f);
	for (int y = h - 1; y >= 0; y--) {
		for (int x = 0; x < w; x++) {
			uint32_t p = px[(size_t)y * w + x];
			unsigned char c[3] = { p & 255, (p >> 8) & 255, (p >> 16) & 255 };

			fwrite(c, 1, 3, f);
		}
		fwrite("\0\0\0", 1, pad, f);
	}
	fclose(f);
}

/* The composed picture as it is, upright */
static void upright(const char *path)
{
	static uint32_t px[ART_W * ART_H];
	const uint8_t *p = screen_picture();

	for (int i = 0; i < ART_W * ART_H; i++, p += 4)
		px[i] = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
	bmp(path, px, ART_W, ART_H);
}

int main(int argc, char **argv)
{
	static uint32_t portrait[480 * 640];
	const char *dir = argc > 1 ? argv[1] : "build-native/updater-shots";
	char path[512];

	screen_open();     /* no framebuffer here: composes all the same */
#define SHOT(name) (snprintf(path, sizeof path, "%s/%s.bmp", dir, name), upright(path))
	/* A long version first: the heading is drawn when a stage starts, and
	 * the updater never changes its version partway through one */
	screen_progress("12.10.3", 1, 7); SHOT("checking-7-long-version");
	screen_progress("2.2.0", 0, 0);   SHOT("writing-0");
	screen_progress("2.2.0", 0, 35);  SHOT("writing-35");
	screen_progress("2.2.0", 0, 100); SHOT("writing-100");
	screen_progress("2.2.0", 1, 60);  SHOT("checking-60");
	screen_progress("2.2.0", 0, 35);
	screen_fit(portrait, 480, 640);
	snprintf(path, sizeof path, "%s/writing-35-pixel2-framebuffer.bmp", dir);
	bmp(path, portrait, 480, 640);
	screen_failed();                  SHOT("failed");
	printf("wrote %s\n", dir);
	return 0;
}
