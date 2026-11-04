/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The updater's writing: see write.h. */
#include "write.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#endif

/* Written and read this much at a time, as rootswitch does */
#define CHUNK (1024 * 1024)

static bool fail(up_result *r, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vsnprintf(r->why, sizeof r->why, format, args);
	va_end(args);
	return false;
}

int up_claim(const char *partition, char *why, size_t whyn)
{
	int fd = open(partition, O_RDONLY | O_EXCL);

	if (fd < 0)
		snprintf(why, whyn, "%s is still in use: %s", partition, strerror(errno));
	return fd;
}

int up_open_card(const char *path, char *why, size_t whyn)
{
	int fd = open(path, O_RDWR);

	if (fd < 0)
		snprintf(why, whyn, "can't open %s for writing: %s", path, strerror(errno));
	return fd;
}

/* `n` bytes of the image, or false at its end or an error */
static bool take(const up_image *image, uint8_t *buf, size_t n)
{
	while (n) {
		ssize_t got = image->read(image->ctx, buf, n);

		if (got <= 0)
			return false;
		buf += got;
		n -= got;
	}
	return true;
}

/* The image's next `n` bytes, read past */
static bool pass(const up_image *image, uint64_t n, uint8_t *scratch)
{
	while (n) {
		size_t step = n < CHUNK ? n : CHUNK;

		if (!take(image, scratch, step))
			return false;
		n -= step;
	}
	return true;
}

/* `n` bytes at `at` onto the card where they differ from it */
static bool put(int card, uint64_t at, const uint8_t *data, size_t n, uint8_t *scratch,
                up_result *r)
{
	if (pread(card, scratch, n, at) != (ssize_t)n)
		return fail(r, "reading the card at byte %llu: %s", (unsigned long long)at,
			    strerror(errno));
	if (memcmp(scratch, data, n) == 0)
		return true;
	r->touched = true;
	if (pwrite(card, data, n, at) != (ssize_t)n)
		return fail(r, "writing the card at byte %llu: %s", (unsigned long long)at,
			    strerror(errno));
	r->written += n;
	return true;
}

/* A span held in memory onto the card, chunk by chunk */
static bool put_span(int card, up_span s, const uint8_t *data, uint8_t *scratch,
                     uint64_t *done, uint64_t total, up_progress progress, void *pctx,
                     up_result *r)
{
	uint64_t at;

	for (at = s.from; at < s.to; at += CHUNK) {
		size_t n = s.to - at < CHUNK ? s.to - at : CHUNK;

		if (!put(card, at, data + (at - s.from), n, scratch, r))
			return false;
		*done += n;
		if (progress)
			progress(pctx, UP_WRITING, *done, total);
	}
	return true;
}

/* The image's next bytes, from..to, against the card's */
static bool check_span(int card, const up_image *image, up_span s, uint8_t *buf,
                       uint8_t *back, uint64_t *done, uint64_t total, up_progress progress,
                       void *pctx, up_result *r)
{
	uint64_t at;

	for (at = s.from; at < s.to; at += CHUNK) {
		size_t n = s.to - at < CHUNK ? s.to - at : CHUNK, i;

		if (!take(image, buf, n))
			return fail(r, "the image ended early, at byte %llu", (unsigned long long)at);
		if (pread(card, back, n, at) != (ssize_t)n)
			return fail(r, "reading the card back at byte %llu: %s",
				    (unsigned long long)at, strerror(errno));
		if (memcmp(buf, back, n) != 0) {
			for (i = 0; buf[i] == back[i]; i++)
				;
			return fail(r, "the card didn't read back what was written, at byte %llu",
				    (unsigned long long)(at + i));
		}
		*done += n;
		if (progress)
			progress(pctx, UP_CHECKING, *done, total);
	}
	return true;
}

/* What was written is on the card, and what is read next comes from it,
 * not from memory: a read-back of the cache proves nothing */
static bool settle(int card, up_result *r)
{
	if (fsync(card) != 0)
		return fail(r, "flushing the card: %s", strerror(errno));
#ifdef BLKFLSBUF
	/* A block device's; a file (make check's cards) answers ENOTTY */
	if (ioctl(card, BLKFLSBUF, 0) != 0 && errno != ENOTTY)
		return fail(r, "clearing the card's cache: %s", strerror(errno));
#endif
	return true;
}

bool up_write(int card, const up_image *image, const up_plan *plan,
              up_progress progress, void *pctx, up_result *r)
{
	/* The boot loader and the boot partition, with whatever lies between,
	 * held as one stretch of the image */
	up_span front = { plan->loader.from, plan->boot.to };
	uint64_t total = (plan->root.to - plan->root.from) + (plan->boot.to - plan->boot.from) +
	                 (plan->loader.to - plan->loader.from), done = 0, at;
	uint8_t *held = NULL, *buf = malloc(CHUNK), *scratch = malloc(CHUNK);
	bool ok = false;

	memset(r, 0, sizeof *r);
	if (front.from > front.to || plan->boot.to > plan->root.from ||
	    plan->loader.to > plan->boot.from) {
		fail(r, "the plan's spans are out of order");
		goto out;
	}
	held = malloc(front.to - front.from);
	if (!held || !buf || !scratch) {
		fail(r, "no memory for the image's front (%llu bytes)",
		     (unsigned long long)(front.to - front.from));
		goto out;
	}

	/* Writing: the front into memory, the root onto the card as it streams
	 * past, then the boot partition, then the boot loader */
	if (!image->open(image->ctx) || !pass(image, front.from, scratch) ||
	    !take(image, held, front.to - front.from) ||
	    !pass(image, plan->root.from - front.to, scratch)) {
		fail(r, "the image ended before its root");
		goto out;
	}
	for (at = plan->root.from; at < plan->root.to; at += CHUNK) {
		size_t n = plan->root.to - at < CHUNK ? plan->root.to - at : CHUNK;

		if (!take(image, buf, n)) {
			fail(r, "the image ended early, at byte %llu", (unsigned long long)at);
			goto out;
		}
		if (!put(card, at, buf, n, scratch, r))
			goto out;
		done += n;
		if (progress)
			progress(pctx, UP_WRITING, done, total);
	}
	if (!put_span(card, plan->boot, held + (plan->boot.from - front.from), scratch, &done,
		      total, progress, pctx, r) ||
	    !put_span(card, plan->loader, held, scratch, &done, total, progress, pctx, r) ||
	    !settle(card, r))
		goto out;

	/* Checking: all of it again, in the image's own order */
	done = 0;
	if (!image->open(image->ctx) || !pass(image, plan->loader.from, scratch)) {
		fail(r, "the image couldn't be read a second time");
		goto out;
	}
	if (!check_span(card, image, plan->loader, buf, scratch, &done, total, progress, pctx, r) ||
	    !pass(image, plan->boot.from - plan->loader.to, scratch) ||
	    !check_span(card, image, plan->boot, buf, scratch, &done, total, progress, pctx, r) ||
	    !pass(image, plan->root.from - plan->boot.to, scratch) ||
	    !check_span(card, image, plan->root, buf, scratch, &done, total, progress, pctx, r)) {
		if (!r->why[0])
			fail(r, "the image ended early on its second reading");
		goto out;
	}
	ok = true;
out:
	free(held);
	free(buf);
	free(scratch);
	return ok;
}
