/* SPDX-License-Identifier: 0BSD */
/* See xfer.h for why every path the browser sends comes through here. */
#include <stdio.h>
#include <string.h>

#include "xfer.h"

static xfer_root g_roots[3];
static int       g_nroots;

static void add_root(const char *name, const char *label, const char *fmt,
                     const char *base)
{
	xfer_root *r;

	if (g_nroots >= (int)(sizeof g_roots / sizeof g_roots[0])) return;
	r = &g_roots[g_nroots];
	snprintf(r->name,  sizeof r->name,  "%s", name);
	snprintf(r->label, sizeof r->label, "%s", label);
	if (snprintf(r->path, sizeof r->path, fmt, base) >= (int)sizeof r->path)
		return;                       /* a root that does not fit is no root */
	g_nroots++;
}

void xfer_init(const char *roms_dir, const char *card_dir)
{
	g_nroots = 0;
	/* Three, and cores/ is deliberately not among them. A .so uploaded there
	 * is dlopen'd into the launcher's own address space on the next launch,
	 * which makes an upload form a way to run code as root and a way to break
	 * the device past the point where the launcher can fix it. The configs
	 * are out for the milder version of the same reason: a bad systems.cfg is
	 * a launcher that does not start, and the way to recover it is the card
	 * reader this feature exists to avoid needing. */
	add_root("roms",  "ROMs",  "%s", roms_dir);
	add_root("bios",  "BIOS",  "%s/Bios",  card_dir);
	add_root("saves", "Saves", "%s/Saves", card_dir);
}

int xfer_root_count(void) { return g_nroots; }

const xfer_root *xfer_root_at(int i)
{
	return (i >= 0 && i < g_nroots) ? &g_roots[i] : NULL;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* Percent-decode into `out`. False on a malformed escape, on a decoded NUL,
 * or on anything that will not fit.
 *
 * A malformed escape is refused rather than passed through as a literal '%'.
 * Passing it through is what most decoders do and it is how "%2%65" becomes
 * "%2e" becomes "." one layer later; refusing means there is exactly one
 * reading of any string that gets this far.
 *
 * Public, so a rename's destination goes through the same decoder as a path
 * rather than through a second one written to look like it. */
bool xfer_decode(const char *in, char *out, size_t outn)
{
	size_t o = 0;

	for (; *in; in++) {
		int c = (unsigned char)*in;

		if (c == '%') {
			int hi = hexval((unsigned char)in[1]);
			int lo = hi < 0 ? -1 : hexval((unsigned char)in[2]);

			if (lo < 0) return false;
			c = hi * 16 + lo;
			in += 2;
		}
		/* Control bytes never name a file anyone meant. NUL would truncate
		 * the string somewhere after this check and turn one path into
		 * another; the rest are refused with it rather than reasoned about
		 * one at a time. */
		if (c < 0x20 || c == 0x7f) return false;
		if (o + 1 >= outn) return false;
		out[o++] = (char)c;
	}
	out[o] = '\0';
	return true;
}

bool xfer_name_ok(const char *name)
{
	size_t n;

	if (!name || !*name) return false;
	if (!strcmp(name, ".") || !strcmp(name, "..")) return false;
	if (strchr(name, '/')) return false;
	n = strlen(name);
	if (n >= XFER_NAME_MAX) return false;
	/* Trailing dots and spaces are legal to ASK for and not legal to HAVE on
	 * vfat, which silently stores something else - so a rename to "x " would
	 * report success and produce a file the next listing cannot find by the
	 * name it was given. */
	if (name[n - 1] == ' ' || name[n - 1] == '.') return false;
	for (; *name; name++)
		if ((unsigned char)*name < 0x20 || (unsigned char)*name == 0x7f)
			return false;
	return true;
}

bool xfer_resolve(const char *url_path, char *out, size_t outn)
{
	char dec[XFER_PATH_MAX];
	const xfer_root *root = NULL;
	char *p, *seg;
	size_t used;
	int i;

	if (!url_path || !out || outn == 0) return false;
	if (!xfer_decode(url_path, dec, sizeof dec)) return false;

	/* An absolute path is not a request for a root-relative one that happens
	 * to start with a slash; it is a request for somewhere else. */
	if (dec[0] == '/') return false;

	p = dec;
	seg = p;
	while (*p && *p != '/') p++;
	if (*p == '/') *p++ = '\0';
	for (i = 0; i < g_nroots; i++)
		if (!strcmp(seg, g_roots[i].name)) { root = &g_roots[i]; break; }
	if (!root) return false;

	used = strlen(root->path);
	if (used >= outn) return false;
	memcpy(out, root->path, used + 1);

	/* Component by component, so "..", "." and "" are decided one at a time
	 * rather than by pattern-matching the whole string - which is the check
	 * that misses "a/../../b" because it only looked at the front. */
	while (*p) {
		size_t n;

		seg = p;
		while (*p && *p != '/') p++;
		if (*p == '/') *p++ = '\0';

		if (!*seg || !strcmp(seg, ".")) continue;   /* "a//b" and "a/./b" */
		if (!strcmp(seg, "..")) return false;
		n = strlen(seg);
		if (n >= XFER_NAME_MAX) return false;
		if (used + 1 + n >= outn) return false;
		out[used++] = '/';
		memcpy(out + used, seg, n + 1);
		used += n;
	}
	return true;
}
