/* SPDX-License-Identifier: GPL-3.0-or-later */
/* What the updater shows while it writes (screen.h). */
#include "screen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __linux__
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "art.h"
#include "xz.h"

#define PICTURE_BYTES ((size_t)ART_W * ART_H * 4)

static uint8_t *picture;      /* what is composed, RGBA */
static uint8_t *base;         /* the screen under it, RGBA */
static uint8_t *pieces;       /* the glyphs and the bar's fill */
static int base_is = -1;      /* which screen base holds: 0 writing, 1 checking, 2 failed */
/* What changed since the last show, in the picture: all of it, or a box */
static struct { bool whole; int x0, y0, x1, y1; } dirty = { true, 0, 0, 0, 0 };
static int shown_phase = -1, shown_pct = -1;
static int redraws;
static double redraw_s;

#ifdef __linux__
static int fb = -1;
static uint8_t *fb_mem;
static size_t fb_len;
static struct fb_var_screeninfo var;
static struct fb_fix_screeninfo fix;
#endif

/* Unpack one of art.h's blobs whole, as it was made: anything else is a
 * build gone wrong, and the screen is left alone. */
static bool unxz(const uint8_t *in, size_t n, uint8_t *out, size_t want)
{
	struct xz_buf b = { in, 0, n, out, 0, want };
	struct xz_dec *s = xz_dec_init(XZ_SINGLE, 0);
	enum xz_ret ret;

	if (!s) return false;
	ret = xz_dec_run(s, &b);
	xz_dec_end(s);
	return ret == XZ_STREAM_END && b.out_pos == want;
}

static bool ready(void)
{
	static int tried;

	if (tried) return picture && base && pieces;
	tried = 1;
	xz_crc32_init();
	picture = malloc(PICTURE_BYTES);
	base = malloc(PICTURE_BYTES);
	pieces = malloc(ART_PIECES_BYTES);
	if (!picture || !base || !pieces ||
	    !unxz(art_pieces_xz, sizeof art_pieces_xz, pieces, ART_PIECES_BYTES)) {
		free(picture); free(base); free(pieces);
		picture = base = pieces = NULL;
		return false;
	}
	return true;
}

static bool use_base(int which)
{
	static const struct { const uint8_t *xz; size_t n; } screens[] = {
		{ art_writing_xz, sizeof art_writing_xz },
		{ art_checking_xz, sizeof art_checking_xz },
		{ art_failed_xz, sizeof art_failed_xz },
	};

	if (base_is == which) return true;
	base_is = -1;
	if (!unxz(screens[which].xz, screens[which].n, base, PICTURE_BYTES)) return false;
	base_is = which;
	return true;
}

/* A piece over the picture at (x, y): its color is already times its
 * coverage, so what shows through is the picture times what is left */
static void put(int id, int x, int y)
{
	const art_piece *p = &art_pieces[id];

	for (int j = 0; j < p->h; j++) {
		const uint8_t *s = pieces + p->offset + (size_t)j * p->w * 4;

		if (y + j < 0 || y + j >= ART_H) continue;
		for (int i = 0; i < p->w; i++, s += 4) {
			uint8_t *d;
			int k = 255 - s[3];

			if (x + i < 0 || x + i >= ART_W || !s[3]) continue;
			d = picture + ((size_t)(y + j) * ART_W + x + i) * 4;
			d[0] = (uint8_t)(s[0] + d[0] * k / 255);
			d[1] = (uint8_t)(s[1] + d[1] * k / 255);
			d[2] = (uint8_t)(s[2] + d[2] * k / 255);
		}
	}
}

/* Part of the bar's fill: columns from..to of the piece, at x */
static void put_fill(int from, int to, int x)
{
	const art_piece *p = &art_pieces[ART_BAR_FILL];

	for (int j = 0; j < p->h; j++)
		for (int i = from; i < to; i++) {
			const uint8_t *s = pieces + p->offset + ((size_t)j * p->w + i) * 4;
			uint8_t *d = picture + ((size_t)(ART_BAR_Y + j) * ART_W + x + i - from) * 4;
			int k = 255 - s[3];

			d[0] = (uint8_t)(s[0] + d[0] * k / 255);
			d[1] = (uint8_t)(s[1] + d[1] * k / 255);
			d[2] = (uint8_t)(s[2] + d[2] * k / 255);
		}
}

static int glyph(char c, bool head)
{
	if (c >= '0' && c <= '9') return (head ? ART_HEAD_0 : ART_VALUE_0) + (c - '0');
	if (head && c == '.') return ART_HEAD_DOT;
	if (!head && c == '%') return ART_VALUE_PERCENT;
	return -1;
}

static int width(const char *s, bool head)
{
	int w = 0;

	for (; *s; s++)
		if (glyph(*s, head) >= 0) w += art_pieces[glyph(*s, head)].w;
	return w;
}

static void text(const char *s, int x, int y, bool head)
{
	for (; *s; s++) {
		int g = glyph(*s, head);

		if (g < 0) continue;
		put(g, x, y);
		x += art_pieces[g].w;
	}
}

/* The framebuffer pixels from (x0, y0) up to (x1, y1), of a w x h one */
static void fit_area(uint32_t *out, int w, int h, int x0, int y0, int x1, int y1)
{
	/* The Pixel's panel is portrait and mounted turned: the picture goes
	 * onto it a quarter turn counter-clockwise, as TortOS's own does
	 * (src/device/pixel2.c), at the panel's long side by its short one.
	 * Sampled between the picture's pixels, with where each column and row
	 * falls worked out once, so a redraw is integer sums only. */
	static int *at_x, *at_y, tw, th;
	bool turned = h > w;
	int pw = turned ? h : w, ph = turned ? w : h;

	if (tw != pw || th != ph) {
		free(at_x);
		free(at_y);
		at_x = malloc(sizeof *at_x * (size_t)pw);
		at_y = malloc(sizeof *at_y * (size_t)ph);
		if (!at_x || !at_y) { tw = th = 0; return; }
		/* Each as the first source pixel times 256 plus the weight of the next */
		for (int i = 0; i < pw; i++) {
			int f = (int)(((i + 0.5) * ART_W / pw - 0.5) * 256);

			at_x[i] = f < 0 ? 0 : f >= (ART_W - 1) * 256 ? (ART_W - 1) * 256 : f;
		}
		for (int i = 0; i < ph; i++) {
			int f = (int)(((i + 0.5) * ART_H / ph - 0.5) * 256);

			at_y[i] = f < 0 ? 0 : f >= (ART_H - 1) * 256 ? (ART_H - 1) * 256 : f;
		}
		tw = pw;
		th = ph;
	}
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++) {
			/* Where this framebuffer pixel is in the upright picture */
			int ux = turned ? pw - 1 - y : x, uy = turned ? x : y;
			int sx = at_x[ux] >> 8, ax = at_x[ux] & 255, sy = at_y[uy] >> 8, ay = at_y[uy] & 255;
			int nx = sx + 1 < ART_W ? 4 : 0, ny = sy + 1 < ART_H ? ART_W * 4 : 0;
			const uint8_t *p = picture + ((size_t)sy * ART_W + sx) * 4;
			uint32_t px = 0xff000000u;

			for (int c = 0; c < 3; c++) {
				int top = p[c] * (256 - ax) + p[c + nx] * ax;
				int bot = p[c + ny] * (256 - ax) + p[c + ny + nx] * ax;

				px |= (uint32_t)((top * (256 - ay) + bot * ay + 32768) >> 16) << (16 - 8 * c);
			}
			out[(size_t)y * w + x] = px;
		}
}

void screen_fit(uint32_t *out, int w, int h)
{
	fit_area(out, w, h, 0, 0, w, h);
}

/* Everything that touches the device: on a computer (make updater-shots) the
 * picture is only composed, and looked at through screen_fit */
#ifdef __linux__
/* Opaque: the Brick's display engine reads the top byte as alpha, and a
 * picture written with it at 0 showed as a lit black screen */
static void show(void)
{
	if (fb < 0) return;
	if (var.xres == ART_W && var.yres == ART_H) {
		for (int y = 0; y < ART_H; y++) {
			uint32_t *row = (uint32_t *)(fb_mem + (size_t)y * fix.line_length);
			const uint8_t *s = picture + (size_t)y * ART_W * 4;

			for (int x = 0; x < ART_W; x++, s += 4)
				row[x] = 0xff000000u | (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
		}
	} else {
		/* Scaled and turned (the Pixel): the whole picture when the screen
		 * under it changes, about 180 ms on its CPU, and otherwise only the
		 * parts that did, mapped onto the framebuffer with a pixel to spare
		 * for the sampling between pixels */
		static uint32_t *fitted;
		int w = (int)var.xres, h = (int)var.yres, fx0 = 0, fy0 = 0, fx1 = w, fy1 = h;

		if (!fitted && !(fitted = malloc((size_t)w * h * 4))) return;
		if (dirty.whole) {
			screen_fit(fitted, w, h);
		} else {
			bool turned = h > w;
			int pw = turned ? h : w, ph = turned ? w : h;
			int ux0 = dirty.x0 * pw / ART_W - 2, ux1 = dirty.x1 * pw / ART_W + 3;
			int uy0 = dirty.y0 * ph / ART_H - 2, uy1 = dirty.y1 * ph / ART_H + 3;

			fx0 = turned ? uy0 : ux0;
			fx1 = turned ? uy1 : ux1;
			fy0 = turned ? pw - ux1 : uy0;
			fy1 = turned ? pw - ux0 : uy1;
			fx0 = fx0 < 0 ? 0 : fx0;
			fy0 = fy0 < 0 ? 0 : fy0;
			fx1 = fx1 > w ? w : fx1;
			fy1 = fy1 > h ? h : fy1;
			fit_area(fitted, w, h, fx0, fy0, fx1, fy1);
		}
		for (int y = fy0; y < fy1; y++)
			memcpy(fb_mem + (size_t)y * fix.line_length + (size_t)fx0 * 4,
			       fitted + (size_t)y * w + fx0, (size_t)(fx1 - fx0) * 4);
	}
}

/* The backlight: TortOS turns it off as it stops. The Brick's through its
 * display driver, as plastron's backlight tool does; the Pixel's in sysfs */
static void light(void)
{
#if defined(UPDATE_BRICK)
	unsigned long args[4] = { 0 };
	int fd = open("/dev/disp", O_RDWR);

	if (fd >= 0) {
		ioctl(fd, 0x104, args);      /* DISP_LCD_BACKLIGHT_ENABLE */
		close(fd);
	}
#else
	char level[16] = "", max[16] = "";
	int fd;

	if ((fd = open("/sys/class/backlight/backlight/bl_power", O_WRONLY)) >= 0) {
		write(fd, "0", 1);
		close(fd);
	}
	if ((fd = open("/sys/class/backlight/backlight/brightness", O_RDWR)) >= 0) {
		if (read(fd, level, sizeof level - 1) > 0 && atoi(level) == 0) {
			int mfd = open("/sys/class/backlight/backlight/max_brightness", O_RDONLY);

			if (mfd >= 0 && read(mfd, max, sizeof max - 1) > 0) {
				lseek(fd, 0, SEEK_SET);
				write(fd, max, strlen(max));
			}
			if (mfd >= 0) close(mfd);
		}
		close(fd);
	}
#endif
}

bool screen_open(void)
{
	if (!ready()) return false;
	if (fb >= 0) return true;
	fb = open("/dev/fb0", O_RDWR);
	if (fb < 0) return false;
	if (ioctl(fb, FBIOGET_VSCREENINFO, &var) != 0 || ioctl(fb, FBIOGET_FSCREENINFO, &fix) != 0 ||
	    var.bits_per_pixel != 32 || var.xres < 2 || var.yres < 2) {
		close(fb);
		fb = -1;
		return false;
	}
	fb_len = (size_t)fix.line_length * var.yres_virtual;
	fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (fb_mem == MAP_FAILED) {
		close(fb);
		fb = -1;
		return false;
	}
	/* Shown from the first buffer: TortOS may have left another showing */
	ioctl(fb, FBIOBLANK, FB_BLANK_UNBLANK);
	var.xoffset = var.yoffset = 0;
	ioctl(fb, FBIOPAN_DISPLAY, &var);
	light();
	return true;
}

void screen_wait_power(int seconds)
{
	struct pollfd keys[16];
	int n = 0;
	DIR *d = opendir("/dev/input");
	struct dirent *e;

	while (d && (e = readdir(d)) && n < 16) {
		unsigned long bits[KEY_CNT / (8 * sizeof(unsigned long)) + 1] = { 0 };
		char path[300];
		int fd;

		if (strncmp(e->d_name, "event", 5)) continue;
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0) continue;
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) >= 0 &&
		    (bits[KEY_POWER / (8 * sizeof(unsigned long))] >> (KEY_POWER % (8 * sizeof(unsigned long)))) & 1) {
			keys[n].fd = fd;
			keys[n++].events = POLLIN;
		} else {
			close(fd);
		}
	}
	if (d) closedir(d);
	if (!n) {
		sleep((unsigned)seconds);
		return;
	}
	for (int left = seconds * 1000; left > 0; left -= 250) {
		if (poll(keys, (nfds_t)n, 250) <= 0) continue;
		for (int i = 0; i < n; i++) {
			struct input_event ev;

			while (read(keys[i].fd, &ev, sizeof ev) == (ssize_t)sizeof ev)
				if (ev.type == EV_KEY && ev.code == KEY_POWER && ev.value == 1)
					goto pressed;
		}
	}
pressed:
	for (int i = 0; i < n; i++) close(keys[i].fd);
}

#else
static void show(void) { }
bool screen_open(void) { return false; }
void screen_wait_power(int seconds) { (void)seconds; }
#endif

static double clock_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

void screen_progress(const char *version, int phase, int pct)
{
	char value[8], v[24];
	int w, fill;
	double t0 = clock_s();

	if (pct < 0) pct = 0;
	if (pct > 100) pct = 100;
	if (!ready() || (phase == shown_phase && pct == shown_pct) || !use_base(phase ? 1 : 0))
		return;
	/* The same screen with a new percentage: only the value and the bar */
	dirty.whole = phase != shown_phase;
	dirty.x0 = ART_BAR_X;
	dirty.x1 = ART_VALUE_X;
	dirty.y0 = ART_VALUE_Y;
	dirty.y1 = ART_BAR_Y + ART_BAR_H;
	shown_phase = phase;
	shown_pct = pct;
	if (dirty.whole) {
		memcpy(picture, base, PICTURE_BYTES);
	} else {
		for (int y = dirty.y0; y < dirty.y1; y++)
			memcpy(picture + ((size_t)y * ART_W + dirty.x0) * 4,
			       base + ((size_t)y * ART_W + dirty.x0) * 4, (size_t)(dirty.x1 - dirty.x0) * 4);
	}

	/* "Update to " and the version, centered as the heading is: outside the
	 * strip a new percentage changes, so only on a whole redraw */
	if (dirty.whole) {
		snprintf(v, sizeof v, "%s", version ? version : "");
		w = art_pieces[ART_HEAD_PREFIX].w + width(v, true);
		put(ART_HEAD_PREFIX, ART_HEAD_X - w / 2, ART_HEAD_Y);
		text(v, ART_HEAD_X - w / 2 + art_pieces[ART_HEAD_PREFIX].w, ART_HEAD_Y, true);
	}

	snprintf(value, sizeof value, "%d%%", pct);
	text(value, ART_VALUE_X - width(value, false), ART_VALUE_Y, false);

	/* The fill as far as pct, keeping the piece's round right end; a sliver
	 * is drawn as just the round end, as TortOS's bar does */
	fill = ART_BAR_W * pct / 100;
	if (fill > 0 && fill < ART_BAR_H) fill = ART_BAR_H;
	if (fill > 0) {
		int cap = ART_BAR_H / 2;

		put_fill(0, fill - cap, ART_BAR_X);
		put_fill(ART_BAR_W - cap, ART_BAR_W, ART_BAR_X + fill - cap);
	}
	show();
	redraws++;
	redraw_s += clock_s() - t0;
}

void screen_cost(int *n, double *seconds)
{
	*n = redraws;
	*seconds = redraw_s;
}

void screen_failed(void)
{
	if (!ready() || !use_base(2)) return;
	memcpy(picture, base, PICTURE_BYTES);
	shown_phase = shown_pct = -1;
	dirty.whole = true;
	show();
}

const uint8_t *screen_picture(void)
{
	return picture;
}

