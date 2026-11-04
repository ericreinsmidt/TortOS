/* SPDX-License-Identifier: GPL-3.0-or-later */
/* A release's card image, unpacked as it is read: see image.h. */
#include "image.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xz.h"

#define DICT_MAX (64u << 20)
#define IN_SIZE (64 * 1024)

struct xz_image {
	int fd;
	struct xz_dec *dec;
	struct xz_buf buf;
	uint8_t in[IN_SIZE];
	bool ended;		/* the stream's end reached */
	bool broken;		/* a read or the stream failed: everything after is -1 */
};

xz_image *xz_image_open(const char *path, char *why, size_t whyn)
{
	static bool tables;
	xz_image *x = calloc(1, sizeof *x);

	if (!tables) {
		xz_crc32_init();
		xz_crc64_init();
		tables = true;
	}
	if (!x) {
		snprintf(why, whyn, "no memory for the image's reader");
		return NULL;
	}
	x->fd = open(path, O_RDONLY);
	if (x->fd < 0) {
		snprintf(why, whyn, "can't open %s: %s", path, strerror(errno));
		free(x);
		return NULL;
	}
	x->dec = xz_dec_init(XZ_DYNALLOC, DICT_MAX);
	if (!x->dec) {
		snprintf(why, whyn, "no memory for the image's decoder");
		close(x->fd);
		free(x);
		return NULL;
	}
	return x;
}

void xz_image_close(xz_image *x)
{
	if (!x)
		return;
	xz_dec_end(x->dec);
	close(x->fd);
	free(x);
}

static bool restart(void *ctx)
{
	xz_image *x = ctx;

	if (lseek(x->fd, 0, SEEK_SET) != 0)
		return false;
	xz_dec_reset(x->dec);
	memset(&x->buf, 0, sizeof x->buf);
	x->buf.in = x->in;
	x->ended = x->broken = false;
	return true;
}

/* Fills `out` with the next unpacked bytes: short only at the stream's end,
 * -1 if the file can't be read or isn't a whole, correct .xz. XZ Embedded
 * checks each block's CRC64 as it ends, so damage shows as XZ_DATA_ERROR
 * here, not as wrong bytes on the card. */
static ssize_t unpack(void *ctx, uint8_t *out, size_t n)
{
	xz_image *x = ctx;
	enum xz_ret ret;

	if (x->broken)
		return -1;
	x->buf.out = out;
	x->buf.out_pos = 0;
	x->buf.out_size = n;
	while (x->buf.out_pos < n && !x->ended) {
		if (x->buf.in_pos == x->buf.in_size) {
			ssize_t got = read(x->fd, x->in, IN_SIZE);

			if (got < 0) {
				x->broken = true;
				return -1;
			}
			x->buf.in_pos = 0;
			x->buf.in_size = got;
		}
		ret = xz_dec_run(x->dec, &x->buf);
		if (ret == XZ_STREAM_END) {
			x->ended = true;
		} else if (ret != XZ_OK) {
			x->broken = true;
			return -1;
		} else if (x->buf.in_size == 0) {
			/* The file ended before the stream did */
			x->broken = true;
			return -1;
		}
	}
	return x->buf.out_pos;
}

up_image xz_image_source(xz_image *x)
{
	up_image source = { x, restart, unpack };

	return source;
}

bool xz_image_peek(xz_image *x, uint64_t at, uint8_t *out, size_t n)
{
	static uint8_t skip[64 * 1024];

	if (!restart(x))
		return false;
	while (at) {
		size_t step = at < sizeof skip ? at : sizeof skip;

		if (unpack(x, skip, step) != (ssize_t)step)
			return false;
		at -= step;
	}
	return unpack(x, out, n) == (ssize_t)n;
}
