/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The updater's plan: see plan.h. */
#include "plan.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static uint32_t u32_at(const uint8_t *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t u64_at(const uint8_t *p)
{
	return u32_at(p) | (uint64_t)u32_at(p + 4) << 32;
}

/* An MBR's four primary entries. A GPT's protective MBR reads as one entry
 * of type 0xEE, which is how a GPT is told apart (as the installer does). */
static bool read_mbr(const uint8_t *head, size_t len, up_table *out, bool *protective)
{
	int i;

	if (len < UP_SECTOR || head[510] != 0x55 || head[511] != 0xAA)
		return false;
	memset(out, 0, sizeof *out);
	*protective = false;
	out->count = 4;
	for (i = 0; i < 4; i++) {
		const uint8_t *e = head + 446 + 16 * i;
		uint64_t sectors = u32_at(e + 12);

		if (e[4] == 0 || sectors == 0)
			continue;
		if (e[4] == 0xEE)
			*protective = true;
		out->part[i].used = true;
		out->part[i].start = u32_at(e + 8);
		out->part[i].sectors = sectors;
	}
	return true;
}

/* A GPT's entries, from its header in sector 1. Its CRCs are not checked,
 * as the installer doesn't for an update: what matters is where the
 * partitions are, and a card whose table is damaged is refused by that
 * comparison anyway. */
static bool read_gpt(const uint8_t *head, size_t len, up_table *out)
{
	const uint8_t *header = head + UP_SECTOR;
	uint64_t lba;
	uint32_t count, size, i;

	if (len < 2 * UP_SECTOR || memcmp(header, "EFI PART", 8) != 0)
		return false;
	lba = u64_at(header + 72);
	count = u32_at(header + 80);
	size = u32_at(header + 84);
	if (size < 128 || count > UP_PARTS || lba > len / UP_SECTOR)
		return false;
	memset(out, 0, sizeof *out);
	out->gpt = true;
	for (i = 0; i < count; i++) {
		uint64_t at = lba * UP_SECTOR + (uint64_t)i * size, first, last;
		const uint8_t *e = head + at;
		static const uint8_t unused[16];

		if (at + 128 > len)
			return false;
		first = u64_at(e + 32);
		last = u64_at(e + 40);
		if (memcmp(e, unused, 16) != 0 && last >= first) {
			out->part[i].used = true;
			out->part[i].start = first;
			out->part[i].sectors = last - first + 1;
		}
		if (out->part[i].used)
			out->count = i + 1;
	}
	return true;
}

bool up_read_table(const uint8_t *head, size_t len, up_table *out)
{
	bool protective;

	if (!read_mbr(head, len, out, &protective))
		return false;
	return protective ? read_gpt(head, len, out) : true;
}

static bool refuse(char *why, size_t whyn, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	vsnprintf(why, whyn, format, args);
	va_end(args);
	return false;
}

bool up_plan_update(const uint8_t *image_head, uint64_t image_len,
                    const uint8_t *card_head, const uint8_t *root_super,
                    uint64_t skip, up_plan *out, char *why, size_t whyn)
{
	up_table image, card;
	uint64_t first = UINT64_MAX, end = 0, table, used;
	int i;

	if (!up_read_table(image_head, UP_HEAD, &image))
		return refuse(why, whyn, "the image has no partition table");
	if (!up_read_table(card_head, UP_HEAD, &card))
		return refuse(why, whyn, "the card has no partition table");
	if (image.gpt != card.gpt)
		return refuse(why, whyn, "the card's partition table is another kind than the image's");
	if (!image.part[0].used || !image.part[1].used)
		return refuse(why, whyn, "the image has no boot and root partitions");

	for (i = 0; i < image.count; i++) {
		const up_part *p = &image.part[i];

		if (!p->used)
			continue;
		if (p->start < first)
			first = p->start;
		if (p->start + p->sectors > end)
			end = p->start + p->sectors;
	}
	table = image.gpt ? UP_HEAD : UP_SECTOR;
	if (skip < table || skip % UP_SECTOR != 0 || skip > first * UP_SECTOR)
		return refuse(why, whyn, "an update can't start at byte %llu of this image",
			      (unsigned long long)skip);
	if (image_len < (image.part[1].start + image.part[1].sectors) * UP_SECTOR)
		return refuse(why, whyn, "the image (%llu bytes) is shorter than its partitions",
			      (unsigned long long)image_len);

	/* The image's partitions exactly where the card has them, the ones the
	 * update leaves too (the Brick's state partition)... */
	for (i = 0; i < image.count; i++) {
		const up_part *p = &image.part[i], *c = &card.part[i];

		if (!p->used)
			continue;
		if (!c->used)
			return refuse(why, whyn, "the card has no partition %d", i + 1);
		if (c->start != p->start || c->sectors != p->sectors)
			return refuse(why, whyn,
				      "partition %d: sector %llu for %llu on the card, %llu for %llu in the image",
				      i + 1, (unsigned long long)c->start, (unsigned long long)c->sectors,
				      (unsigned long long)p->start, (unsigned long long)p->sectors);
	}
	/* ...and the card's others, the games partition, wholly past them */
	for (i = 0; i < card.count; i++)
		if (card.part[i].used && !(i < image.count && image.part[i].used) &&
		    card.part[i].start < end)
			return refuse(why, whyn, "the card's partition %d starts at sector %llu, inside the system",
				      i + 1, (unsigned long long)card.part[i].start);

	/* The root as far as its squashfs runs: bytes_used, little-endian at 40
	 * in the superblock, padded to 4 KiB as mksquashfs pads it. Read by the
	 * kernel to there and no further, so what lies past it in the partition
	 * is never looked at. */
	if (memcmp(root_super, "hsqs", 4) != 0)
		return refuse(why, whyn, "the image's root partition doesn't hold a squashfs");
	used = (u64_at(root_super + 40) + 4095) / 4096 * 4096;
	if (used > image.part[1].sectors * UP_SECTOR)
		return refuse(why, whyn, "the image's root (%llu bytes) doesn't fit its partition",
			      (unsigned long long)used);

	out->loader.from = skip;
	out->loader.to = first * UP_SECTOR;
	out->boot.from = image.part[0].start * UP_SECTOR;
	out->boot.to = (image.part[0].start + image.part[0].sectors) * UP_SECTOR;
	out->root.from = image.part[1].start * UP_SECTOR;
	out->root.to = out->root.from + used;
	return true;
}
