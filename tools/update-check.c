/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Does the updater's plan write what the installer's Update would, and refuse
 * what it refuses?
 *
 *     make check-update
 *
 * The cards are made up here, in each device's real shape (plastron's
 * genimage.cfg for both): the GKD Pixel 2's MBR, the boot partition at 16 MiB
 * and the root at 80 MiB, and the Brick's GPT, boot at 4 MiB, root at 68 MiB
 * and the state partition after it. Then the games partition a first boot
 * adds, and every way a card can be the wrong one. The installer's own tests
 * (TortOS-Installer, writer.rs) are the model.
 *
 * No device. The plan's disks are their first sectors, in memory; the
 * writing checks put a small card in a file under /tmp, and remove it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>

#include "tortos-update/plan.h"
#include "tortos-update/sha256.h"
#include "tortos-update/write.h"

static int failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

#define MIB (2048ULL)		/* sectors */
#define PIXEL_SKIP (32 * 1024)
#define BRICK_SKIP (128 * 1024)

typedef struct { uint64_t start, sectors; } part;

static void put32(uint8_t *p, uint32_t v)
{
	p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void put64(uint8_t *p, uint64_t v)
{
	put32(p, (uint32_t)v);
	put32(p + 4, (uint32_t)(v >> 32));
}

/* An MBR with these partitions in slots 1 to n, a zero start for an empty slot */
static void mbr(uint8_t *head, const part *parts, int n)
{
	int i;

	memset(head, 0, UP_HEAD);
	for (i = 0; i < n; i++) {
		uint8_t *e = head + 446 + 16 * i;

		if (!parts[i].start)
			continue;
		e[4] = i == 0 ? 0x0C : 0x83;
		put32(e + 8, (uint32_t)parts[i].start);
		put32(e + 12, (uint32_t)parts[i].sectors);
	}
	head[510] = 0x55;
	head[511] = 0xAA;
}

/* A GPT as genimage writes it: the protective MBR, the header in sector 1,
 * 128 entries of 128 bytes from sector 2 */
static void gpt(uint8_t *head, const part *parts, int n)
{
	int i;

	memset(head, 0, UP_HEAD);
	head[446 + 4] = 0xEE;
	put32(head + 446 + 8, 1);
	put32(head + 446 + 12, 0xFFFFFFFF);
	head[510] = 0x55;
	head[511] = 0xAA;
	memcpy(head + 512, "EFI PART", 8);
	put64(head + 512 + 72, 2);
	put32(head + 512 + 80, 128);
	put32(head + 512 + 84, 128);
	for (i = 0; i < n; i++) {
		uint8_t *e = head + 2 * UP_SECTOR + 128 * i;

		if (!parts[i].start)
			continue;
		memset(e, 0xA0 + i, 16);	/* any type that isn't all zeros */
		put64(e + 32, parts[i].start);
		put64(e + 40, parts[i].start + parts[i].sectors - 1);
	}
}

/* A squashfs superblock saying it runs `used` bytes */
static void squashfs(uint8_t *super, uint64_t used)
{
	memset(super, 0, UP_SUPER);
	memcpy(super, "hsqs", 4);
	put64(super + 40, used);
}

static const part PIXEL[] = {
	{ 16 * MIB, 64 * MIB },		/* PX2BOOT */
	{ 80 * MIB, 256 * MIB },	/* root */
};
static const part PIXEL_CARD[] = {
	{ 16 * MIB, 64 * MIB },
	{ 80 * MIB, 256 * MIB },
	{ 336 * MIB, 58000 * MIB },	/* games, made by the first boot */
};
static const part BRICK[] = {
	{ 4 * MIB, 64 * MIB },		/* BRICKBOOT */
	{ 68 * MIB, 256 * MIB },	/* plastron-root */
	{ 324 * MIB, 16 * MIB },	/* plastron-state */
};
static const part BRICK_CARD[] = {
	{ 4 * MIB, 64 * MIB },
	{ 68 * MIB, 256 * MIB },
	{ 324 * MIB, 16 * MIB },
	{ 340 * MIB, 58000 * MIB },	/* tortos */
};
#define PIXEL_LEN (336 * MIB * UP_SECTOR)
#define BRICK_LEN (340 * MIB * UP_SECTOR)

static uint8_t image[UP_HEAD], card[UP_HEAD], super[UP_SUPER];
static up_plan plan;
static char why[256];

static int run(uint64_t len, uint64_t skip)
{
	why[0] = 0;
	return up_plan_update(image, len, card, super, skip, &plan, why, sizeof why);
}

/* Refused, saying `says` */
static void refused(uint64_t len, uint64_t skip, const char *says, const char *what)
{
	CHECK(!run(len, skip), "%s is refused", what);
	CHECK(strstr(why, says) != NULL, "%s says \"%s\", not \"%s\"", what, says, why);
}

/* ---- write.c, on small cards in files ---- */

/* A small card in the Brick's shape, in sectors: the GPT, the boot loader at
 * 64 KiB, boot, root, state, then games. The root's squashfs ends partway
 * through its partition, as a real one does. */
#define W_SKIP (64 * 1024)
static const part W_IMAGE[] = { { 256, 2048 }, { 2304, 8192 }, { 10496, 512 } };
static const part W_CARD[] = { { 256, 2048 }, { 2304, 8192 }, { 10496, 512 }, { 11008, 4096 } };
#define W_IMAGE_LEN (11008ULL * UP_SECTOR)
#define W_CARD_LEN (15104ULL * UP_SECTOR)

typedef struct { const uint8_t *data; size_t len, at; int opens, flip_on; } mem_image;

static bool mem_open(void *ctx)
{
	mem_image *m = ctx;

	m->at = 0;
	m->opens++;
	return true;
}

static ssize_t mem_read(void *ctx, uint8_t *buf, size_t n)
{
	mem_image *m = ctx;

	if (n > m->len - m->at)
		n = m->len - m->at;
	memcpy(buf, m->data + m->at, n);
	/* A byte that reads differently on the given pass: what a card that
	 * doesn't hold what was written looks like from here */
	if (m->opens == m->flip_on && m->at <= 3000000 && 3000000 < m->at + n)
		buf[3000000 - m->at] ^= 0xFF;
	m->at += n;
	return n;
}

/* A disk image filled with `seed`'s pattern, its table and root superblock in place */
static uint8_t *disk(size_t len, const part *parts, int n, int seed, uint64_t used)
{
	uint8_t *d = malloc(len);
	size_t i;

	for (i = 0; i < len; i++)
		d[i] = (uint8_t)(i * 7 + seed * 131 + (i >> 12));
	gpt(d, parts, n);
	squashfs(d + parts[1].start * UP_SECTOR, used);
	return d;
}

static int card_file(const uint8_t *d, size_t len, char *path)
{
	int fd;

	strcpy(path, "/tmp/update-check-XXXXXX");
	fd = mkstemp(path);
	if (fd >= 0 && write(fd, d, len) != (ssize_t)len) {
		close(fd);
		fd = -1;
	}
	if (fd >= 0)
		close(fd);
	return fd;
}

static void write_checks(void)
{
	uint8_t *old = disk(W_CARD_LEN, W_CARD, 4, 1, 3500000);
	uint8_t *new = disk(W_IMAGE_LEN, W_IMAGE, 3, 2, 3800000);
	uint8_t *after = malloc(W_CARD_LEN);
	mem_image m = { new, W_IMAGE_LEN, 0, 0, 0 };
	up_image source = { &m, mem_open, mem_read };
	char path[64], err[256];
	up_result r;
	int fd;
	uint64_t root_end;

	printf("writing\n");
	CHECK(card_file(old, W_CARD_LEN, path) >= 0, "a card file");
	fd = up_open_card(path, err, sizeof err);
	CHECK(fd >= 0, "the card opens (%s)", err);
	memcpy(card, old, UP_HEAD);
	memcpy(image, new, UP_HEAD);
	memcpy(super, new + W_IMAGE[1].start * UP_SECTOR, UP_SUPER);
	CHECK(run(W_IMAGE_LEN, W_SKIP), "the small card plans (%s)", why);
	root_end = plan.root.to;
	CHECK(up_write(fd, &source, &plan, NULL, NULL, &r), "it writes (%s)", r.why);
	CHECK(r.touched && r.written > 0, "and says it wrote");
	CHECK(pread(fd, after, W_CARD_LEN, 0) == (ssize_t)W_CARD_LEN, "the card reads");
	CHECK(memcmp(after + W_SKIP, new + W_SKIP, root_end - W_SKIP) == 0,
	      "the boot loader, boot and root are the image's");
	CHECK(memcmp(after, old, W_SKIP) == 0, "the table and the bytes before the boot loader are untouched");
	CHECK(memcmp(after + root_end, old + root_end, W_CARD_LEN - root_end) == 0,
	      "past the root's squashfs, state and games are untouched");

	CHECK(up_write(fd, &source, &plan, NULL, NULL, &r) && r.written == 0 && !r.touched,
	      "written again, nothing differs and nothing is written (%llu bytes)",
	      (unsigned long long)r.written);

	/* The boot loader the same as the card's: only the rest goes on */
	close(fd);
	memcpy(old + W_SKIP, new + W_SKIP, plan.loader.to - W_SKIP);
	card_file(old, W_CARD_LEN, path);
	fd = up_open_card(path, err, sizeof err);
	CHECK(up_write(fd, &source, &plan, NULL, NULL, &r), "an unchanged boot loader writes (%s)", r.why);
	CHECK(r.written <= root_end - plan.loader.to, "and the boot loader isn't among what was written");

	printf("failures\n");
	close(fd);
	card_file(old, W_CARD_LEN, path);
	fd = up_open_card(path, err, sizeof err);
	m.opens = 0;
	m.flip_on = 2;
	CHECK(!up_write(fd, &source, &plan, NULL, NULL, &r), "a card that reads back wrong fails");
	CHECK(strstr(r.why, "at byte 3000000") != NULL, "and says where (%s)", r.why);
	m.flip_on = 0;

	close(fd);
	card_file(old, W_CARD_LEN, path);
	fd = up_open_card(path, err, sizeof err);
	m.len = plan.boot.to - 100;
	CHECK(!up_write(fd, &source, &plan, NULL, NULL, &r), "an image ending before its root fails");
	CHECK(!r.touched, "with nothing written (%s)", r.why);
	m.len = plan.root.from + 2 * 1024 * 1024 + 5;
	CHECK(!up_write(fd, &source, &plan, NULL, NULL, &r), "an image ending inside its root fails");
	CHECK(r.touched && strstr(r.why, "ended early"), "having written, and says so (%s)", r.why);
	m.len = W_IMAGE_LEN;

	close(fd);
	unlink(path);
	free(old);
	free(new);
	free(after);
}

/* ---- sha256.c ---- */

static int digest_is(const void *data, size_t n, const char *want)
{
	sha256 h;
	uint8_t d[32];
	char hex[65];

	sha256_start(&h);
	sha256_add(&h, data, n);
	sha256_finish(&h, d, hex);
	return strcmp(hex, want) == 0;
}

static void sha256_checks(void)
{
	/* FIPS 180-4's examples, from NIST's published test vectors */
	static const char abc448[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	static const char abc896[] = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
	                             "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
	uint8_t *million = malloc(1000000), one[32], split[32], data[300];
	size_t n, i;
	sha256 h;

	printf("sha-256\n");
	CHECK(digest_is("", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
	      "the empty message");
	CHECK(digest_is("abc", 3, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
	      "\"abc\"");
	CHECK(digest_is(abc448, 56, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
	      "the 448-bit message, padded into a second block");
	CHECK(digest_is(abc896, 112, "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"),
	      "the 896-bit message");
	memset(million, 'a', 1000000);
	CHECK(digest_is(million, 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
	      "a million a's");
	free(million);

	/* However it is fed: every length to past a few blocks, all at once
	 * against a byte at a time and in uneven pieces, so a slip at a block's
	 * edge (55, 56, 63, 64 bytes) shows */
	for (i = 0; i < sizeof data; i++)
		data[i] = (uint8_t)(i * 31 + 7);
	for (n = 0; n <= sizeof data; n++) {
		sha256_start(&h);
		sha256_add(&h, data, n);
		sha256_finish(&h, one, NULL);
		sha256_start(&h);
		for (i = 0; i < n; i++)
			sha256_add(&h, data + i, 1);
		sha256_finish(&h, split, NULL);
		CHECK(memcmp(one, split, 32) == 0, "%zu bytes a byte at a time", n);
		sha256_start(&h);
		for (i = 0; i < n; i += 13)
			sha256_add(&h, data + i, n - i < 13 ? n - i : 13);
		sha256_finish(&h, split, NULL);
		CHECK(memcmp(one, split, 32) == 0, "%zu bytes 13 at a time", n);
	}
}

int main(void)
{
	up_table t;
	part moved[4];

	printf("tables\n");
	mbr(image, PIXEL, 2);
	CHECK(up_read_table(image, UP_HEAD, &t) && !t.gpt, "the Pixel's MBR reads as an MBR");
	CHECK(t.part[1].used && t.part[1].start == 80 * MIB, "its root is at 80 MiB");
	gpt(image, BRICK, 3);
	CHECK(up_read_table(image, UP_HEAD, &t) && t.gpt, "the Brick's GPT reads as a GPT");
	CHECK(t.count == 3 && t.part[2].sectors == 16 * MIB, "its state partition is 16 MiB");
	memset(image, 0, UP_HEAD);
	CHECK(!up_read_table(image, UP_HEAD, &t), "zeros are no table");

	printf("the Pixel 2\n");
	mbr(image, PIXEL, 2);
	mbr(card, PIXEL_CARD, 3);
	squashfs(super, 42214000);
	CHECK(run(PIXEL_LEN, PIXEL_SKIP), "the Pixel's card updates (%s)", why);
	CHECK(plan.loader.from == PIXEL_SKIP && plan.loader.to == 16 * MIB * UP_SECTOR,
	      "the boot loader runs from 32 KiB to the boot partition");
	CHECK(plan.boot.from == 16 * MIB * UP_SECTOR && plan.boot.to == 80 * MIB * UP_SECTOR,
	      "the boot partition is 16 to 80 MiB");
	CHECK(plan.root.from == 80 * MIB * UP_SECTOR &&
	      plan.root.to == 80 * MIB * UP_SECTOR + 42217472,
	      "the root runs to its squashfs's end, padded to 4 KiB");
	CHECK(plan.root.to < (80 + 256) * MIB * UP_SECTOR, "and stops short of the games partition");
	mbr(card, PIXEL, 2);
	CHECK(run(PIXEL_LEN, PIXEL_SKIP), "a card never booted (no games partition) updates too");

	printf("the Brick\n");
	gpt(image, BRICK, 3);
	gpt(card, BRICK_CARD, 4);
	squashfs(super, 46775000);
	CHECK(run(BRICK_LEN, BRICK_SKIP), "the Brick's card updates (%s)", why);
	CHECK(plan.loader.from == BRICK_SKIP && plan.loader.to == 4 * MIB * UP_SECTOR,
	      "the boot loader runs from 128 KiB to the boot partition");
	CHECK(plan.root.to <= 324 * MIB * UP_SECTOR, "nothing reaches the state partition");
	gpt(card, BRICK, 3);
	CHECK(run(BRICK_LEN, BRICK_SKIP), "a Brick card never booted updates too");

	printf("refusals\n");
	gpt(card, BRICK_CARD, 4);
	memcpy(moved, BRICK_CARD, sizeof moved);
	moved[2].start += MIB;
	gpt(card, moved, 4);
	refused(BRICK_LEN, BRICK_SKIP, "partition 3", "a moved state partition");
	memcpy(moved, BRICK_CARD, sizeof moved);
	moved[1].sectors = 300 * MIB;
	gpt(card, moved, 4);
	refused(BRICK_LEN, BRICK_SKIP, "partition 2", "a root partition of another size");
	memcpy(moved, BRICK_CARD, sizeof moved);
	moved[3].start = 300 * MIB;
	gpt(card, moved, 4);
	refused(BRICK_LEN, BRICK_SKIP, "inside the system", "a games partition inside the system");
	gpt(card, BRICK_CARD, 2);
	refused(BRICK_LEN, BRICK_SKIP, "no partition 3", "a card missing the state partition");

	mbr(image, PIXEL, 2);
	gpt(card, BRICK_CARD, 4);
	squashfs(super, 42214000);
	refused(PIXEL_LEN, PIXEL_SKIP, "another kind", "the Pixel's image on the Brick's card");
	gpt(image, BRICK, 3);
	mbr(card, PIXEL_CARD, 3);
	refused(BRICK_LEN, BRICK_SKIP, "another kind", "the Brick's image on the Pixel's card");
	memset(card, 0, UP_HEAD);
	refused(BRICK_LEN, BRICK_SKIP, "no partition table", "a card with no table");

	mbr(image, PIXEL, 2);
	mbr(card, PIXEL_CARD, 3);
	refused(PIXEL_LEN - 1, PIXEL_SKIP, "shorter", "an image cut short");
	refused(PIXEL_LEN, 256, "can't start", "a boot loader inside the table");
	refused(PIXEL_LEN, PIXEL_SKIP + 1, "can't start", "a boot loader off a sector");
	refused(PIXEL_LEN, 17 * MIB * UP_SECTOR, "can't start", "a boot loader past the first partition");
	memset(super, 0, UP_SUPER);
	refused(PIXEL_LEN, PIXEL_SKIP, "squashfs", "a root that isn't a squashfs");
	squashfs(super, 256 * MIB * UP_SECTOR + 1);
	refused(PIXEL_LEN, PIXEL_SKIP, "doesn't fit", "a squashfs bigger than its partition");
	mbr(image, (part[]){ { 16 * MIB, 64 * MIB } }, 1);
	squashfs(super, 42214000);
	refused(PIXEL_LEN, PIXEL_SKIP, "no boot and root", "an image with no root partition");

	write_checks();
	sha256_checks();

	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
