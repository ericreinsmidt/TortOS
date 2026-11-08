/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Is there a newer TortOS, and which file is it (src/update.h). */
#include "update.h"

#include "atomic.h"
#include "net.h"
#include "rajson.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A reply for the latest release is about 11 KB with five files; room for a
 * release with many more, and for the long notes some releases carry. */
#define REPLY_MAX (256 * 1024)
/* An image is about 40 MB; anything past this is not one of ours. */
#define IMAGE_MAX ((long long)1 << 30)

static void say(char *why, size_t whyn, const char *text)
{
	if (why && whyn) snprintf(why, whyn, "%s", text);
}

/* One to four numbers of up to five digits, joined by dots. */
static bool version_parts(const char *v, long part[4], int *count)
{
	int n = 0;

	if (!v || !*v) return false;
	for (;;) {
		long x = 0;
		int  digits = 0;

		while (isdigit((unsigned char)*v)) {
			if (++digits > 5) return false;
			x = x * 10 + (*v++ - '0');
		}
		if (!digits || n == 4) return false;
		part[n++] = x;
		if (*v == '\0') break;
		if (*v++ != '.') return false;
	}
	*count = n;
	return true;
}

bool update_is_newer(const char *candidate, const char *installed)
{
	long a[4], b[4];
	int  na, nb;

	if (!version_parts(candidate, a, &na) || !version_parts(installed, b, &nb))
		return false;
	for (int i = 0; i < 4; i++) {
		long x = i < na ? a[i] : 0, y = i < nb ? b[i] : 0;

		if (x != y) return x > y;
	}
	return false;
}

static bool is_sha256(const char *s)
{
	int n = 0;

	for (; s[n]; n++)
		if (!isxdigit((unsigned char)s[n]) || isupper((unsigned char)s[n])) return false;
	return n == 64;
}

/* A download address: https, or plain http from a development feed. Nothing
 * that would break a key=value line or curl's config file. */
static bool url_ok(const char *u)
{
	if (strncmp(u, "https://", 8) && !(UPDATE_FEED_IS_DEV && !strncmp(u, "http://", 7)))
		return false;
	for (; *u; u++)
		if ((unsigned char)*u <= ' ' || *u == '"' || *u == '\\' || *u == 0x7f) return false;
	return true;
}

/* A device's name goes into a file name: lower case letters and digits. */
static bool device_ok(const char *d)
{
	if (!d || !*d || strlen(d) > 16) return false;
	for (; *d; d++)
		if (!islower((unsigned char)*d) && !isdigit((unsigned char)*d)) return false;
	return true;
}

/* A string member that fits whole: js_str truncates without saying, and a
 * shortened address or name must not pass for the real one. */
static bool member_str(jsv obj, const char *name, char *out, size_t n)
{
	jsv v;

	if (!js_member(obj, name, &v) || !js_str(v, out, n)) return false;
	return strlen(out) < n - 1;
}

/* One file of the release: where it is, how big, and its SHA-256 from
 * GitHub's "sha256:..." digest. */
static bool asset(jsv a, char *url, size_t urln, long long *size, char *sha256,
                  char *why, size_t whyn, const char *what)
{
	char digest[80], msg[160];
	jsv  v;

	if (!member_str(a, "browser_download_url", url, urln) || !url_ok(url)) {
		snprintf(msg, sizeof msg, "%s has no usable download address", what);
		say(why, whyn, msg);
		return false;
	}
	if (!js_member(a, "size", &v) || js_int(v) <= 0 || js_int(v) > IMAGE_MAX) {
		snprintf(msg, sizeof msg, "%s has no usable size", what);
		say(why, whyn, msg);
		return false;
	}
	*size = js_int(v);
	if (!member_str(a, "digest", digest, sizeof digest) ||
	    strncmp(digest, "sha256:", 7) || !is_sha256(digest + 7)) {
		snprintf(msg, sizeof msg, "%s has no SHA-256", what);
		say(why, whyn, msg);
		return false;
	}
	memcpy(sha256, digest + 7, 64);     /* exactly 64, as is_sha256 said */
	sha256[64] = '\0';
	return true;
}

bool update_parse(const char *json, size_t len, const char *device,
                  update_release *out, char *why, size_t whyn)
{
	update_release r;
	char tag[32], want[96], name[160];
	bool image = false, sums = false;
	long long sums_size;
	long part[4];
	int  parts;
	jsv  root, v, assets, it = { 0 }, a;

	memset(&r, 0, sizeof r);
	if (!device_ok(device)) { say(why, whyn, "no device name"); return false; }
	if (!json || !len) { say(why, whyn, "an empty reply"); return false; }
	root = js_root(json, len);
	if (!member_str(root, "tag_name", tag, sizeof tag) || tag[0] != 'v' ||
	    !version_parts(tag + 1, part, &parts)) {
		say(why, whyn, "no release version in the reply");
		return false;
	}
	if ((js_member(root, "draft", &v) && js_is_true(v)) ||
	    (js_member(root, "prerelease", &v) && js_is_true(v))) {
		say(why, whyn, "the latest release is a draft or a prerelease");
		return false;
	}
	snprintf(r.version, sizeof r.version, "%s", tag + 1);
	snprintf(want, sizeof want, "TortOS-v%s-%s.img.xz", r.version, device);
	if (!js_member(root, "assets", &assets)) {
		say(why, whyn, "the release lists no files");
		return false;
	}
	while (js_next(assets, &it, &a)) {
		if (!member_str(a, "name", name, sizeof name)) continue;
		if (!image && !strcmp(name, want)) {
			if (!asset(a, r.url, sizeof r.url, &r.size, r.sha256, why, whyn, want))
				return false;
			snprintf(r.file, sizeof r.file, "%s", want);
			image = true;
		} else if (!sums && !strcmp(name, "SHA256SUMS")) {
			if (!asset(a, r.sums_url, sizeof r.sums_url, &sums_size, r.sums_sha256,
			           why, whyn, "SHA256SUMS"))
				return false;
			sums = true;
		}
	}
	if (!image) {
		snprintf(name, sizeof name, "the release has no %s", want);
		say(why, whyn, name);
		return false;
	}
	if (!sums) { say(why, whyn, "the release has no SHA256SUMS"); return false; }
	*out = r;
	return true;
}

bool update_cache_write(const char *path, time_t when, const char *installed,
                        update_state state, const update_release *r)
{
	FILE *f;

	if (state == UPDATE_UNKNOWN || !(f = atomic_open(path, 0644))) return false;
	fprintf(f, "checked=%lld\ninstalled=%s\nstate=%s\n", (long long)when, installed,
	        state == UPDATE_WAITING ? "waiting" : "current");
	if (state == UPDATE_WAITING)
		fprintf(f, "version=%s\nfile=%s\nurl=%s\nsize=%lld\nsha256=%s\n"
		           "sums_url=%s\nsums_sha256=%s\n",
		        r->version, r->file, r->url, r->size, r->sha256, r->sums_url,
		        r->sums_sha256);
	return atomic_commit(f, path);
}

static void copy_value(char *out, size_t n, const char *v, bool *fits)
{
	size_t k = strlen(v);

	if (k >= n) {
		*fits = false;
		k = n - 1;
	}
	memcpy(out, v, k);
	out[k] = '\0';
}

static bool cache_read(const char *path, bool aged, time_t now, const char *installed,
                       update_state *state, update_release *r);

bool update_cache_read(const char *path, time_t now, const char *installed,
                       update_state *state, update_release *r)
{
	return cache_read(path, true, now, installed, state, r);
}

bool update_cache_peek(const char *path, const char *installed,
                       update_state *state, update_release *r)
{
	return cache_read(path, false, 0, installed, state, r);
}

static bool cache_read(const char *path, bool aged, time_t now, const char *installed,
                       update_state *state, update_release *r)
{
	update_release c;
	char line[1024], have[24] = "", st[16] = "", want[96];
	long long checked = -1;
	bool fits = true;
	FILE *f = fopen(path, "r");

	if (!f) return false;
	memset(&c, 0, sizeof c);
	while (fgets(line, sizeof line, f)) {
		char *eq = strchr(line, '='), *v;

		line[strcspn(line, "\r\n")] = '\0';
		if (!eq) continue;
		*eq = '\0';
		v = eq + 1;
		if (!strcmp(line, "checked")) checked = strtoll(v, NULL, 10);
		else if (!strcmp(line, "installed")) copy_value(have, sizeof have, v, &fits);
		else if (!strcmp(line, "state")) copy_value(st, sizeof st, v, &fits);
		else if (!strcmp(line, "version")) copy_value(c.version, sizeof c.version, v, &fits);
		else if (!strcmp(line, "file")) copy_value(c.file, sizeof c.file, v, &fits);
		else if (!strcmp(line, "url")) copy_value(c.url, sizeof c.url, v, &fits);
		else if (!strcmp(line, "size")) c.size = strtoll(v, NULL, 10);
		else if (!strcmp(line, "sha256")) copy_value(c.sha256, sizeof c.sha256, v, &fits);
		else if (!strcmp(line, "sums_url")) copy_value(c.sums_url, sizeof c.sums_url, v, &fits);
		else if (!strcmp(line, "sums_sha256"))
			copy_value(c.sums_sha256, sizeof c.sums_sha256, v, &fits);
	}
	fclose(f);
	if (!fits || checked < 0 || strcmp(have, installed) ||
	    (aged && (now < checked || now - checked >= UPDATE_CHECK_EVERY_S)))
		return false;
	if (!strcmp(st, "current")) {
		*state = UPDATE_CURRENT;
		return true;
	}
	if (strcmp(st, "waiting") || !update_is_newer(c.version, installed)) return false;
	/* Kept by this file, but on a card anyone can write: held to the same
	 * rules as a fresh reply before it can name a download. */
	snprintf(want, sizeof want, "TortOS-v%s-", c.version);
	if (strncmp(c.file, want, strlen(want)) || strstr(c.file, "/") ||
	    strlen(c.file) < 8 || strcmp(c.file + strlen(c.file) - 7, ".img.xz") ||
	    !url_ok(c.url) || !url_ok(c.sums_url) || c.size <= 0 || c.size > IMAGE_MAX ||
	    !is_sha256(c.sha256) || !is_sha256(c.sums_sha256))
		return false;
	*state = UPDATE_WAITING;
	*r = c;
	return true;
}

update_state update_answer(const char *json, size_t len, const char *device,
                           const char *installed, const char *cache_path, time_t now,
                           update_release *out, char *why, size_t whyn)
{
	update_release r;
	update_state state;

	if (!update_parse(json, len, device, &r, why, whyn)) return UPDATE_UNKNOWN;
	state = update_is_newer(r.version, installed) ? UPDATE_WAITING : UPDATE_CURRENT;
	if (state == UPDATE_WAITING) *out = r;
	say(why, whyn, state == UPDATE_WAITING ? "a newer release" : "up to date");
	update_cache_write(cache_path, now, installed, state, &r);
	return state;
}

update_state update_check(const char *device, const char *installed,
                          const char *cache_path, time_t now, int timeout_s,
                          update_release *out, char *why, size_t whyn)
{
	update_state state;
	char *reply = malloc(REPLY_MAX);
	long len;

	if (!reply) { say(why, whyn, "out of memory"); return UPDATE_UNKNOWN; }
	len = net_get_buf(UPDATE_FEED, NULL, 0, reply, REPLY_MAX, timeout_s);
	if (len <= 0) {
		free(reply);
		say(why, whyn, "no answer from the release feed");
		return UPDATE_UNKNOWN;
	}
	if (len >= REPLY_MAX - 1) {
		free(reply);
		say(why, whyn, "the release feed's reply was too long");
		return UPDATE_UNKNOWN;
	}
	state = update_answer(reply, (size_t)len, device, installed, cache_path, now,
	                      out, why, whyn);
	free(reply);
	return state;
}

bool update_sums_list(const char *sums, size_t len, const char *file, const char *sha256)
{
	const char *p = sums, *end = sums + len;
	size_t flen = strlen(file);
	bool found = false;

	if (!is_sha256(sha256) || !flen) return false;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p)), *name;
		size_t n;
		char hex[65];

		if (!eol) eol = end;
		n = (size_t)(eol - p);
		if (n > 0 && p[n - 1] == '\r') n--;
		/* 64 hex, a space, then a space or '*' (binary mode), then the name */
		if (n > 66 && (p[64] == ' ') && (p[65] == ' ' || p[65] == '*')) {
			name = p + 66;
			if ((size_t)(n - 66) == flen && !memcmp(name, file, flen)) {
				memcpy(hex, p, 64);
				hex[64] = '\0';
				if (!is_sha256(hex) || strcmp(hex, sha256)) return false;
				found = true;
			}
		}
		p = eol + 1;
	}
	return found;
}

bool update_request_write(const char *path, const char *device, const update_release *r)
{
	FILE *f;

	if (!device_ok(device) || !is_sha256(r->sha256) || r->size <= 0 ||
	    !r->file[0] || strchr(r->file, '/') || !(f = atomic_open(path, 0644)))
		return false;
	fprintf(f, "file=%s\nversion=%s\ndevice=%s\nsize=%lld\nsha256=%s\n",
	        r->file, r->version, device, r->size, r->sha256);
	return atomic_commit(f, path);
}

update_result update_result_parse(const char *line, char *detail, size_t n)
{
	static const struct { const char *start; update_result result; } said[] = {
		{ "updated to TortOS ", UPDATE_RESULT_DONE },
		{ "not updated, nothing written: ", UPDATE_RESULT_UNTOUCHED },
		{ "FAILED after writing: ", UPDATE_RESULT_FAILED },
	};

	if (detail && n) detail[0] = '\0';
	if (!line) return UPDATE_RESULT_NONE;
	for (size_t i = 0; i < sizeof said / sizeof said[0]; i++) {
		size_t k = strlen(said[i].start);

		if (!strncmp(line, said[i].start, k)) {
			if (detail && n) {
				snprintf(detail, n, "%s", line + k);
				detail[strcspn(detail, "\r\n")] = '\0';
			}
			return said[i].result;
		}
	}
	return UPDATE_RESULT_NONE;
}

bool update_battery_ok(bool readable, bool charging, int pct)
{
	return !readable || charging || pct > UPDATE_BATTERY_MIN_PCT;
}

bool update_offer(const char *file, const char *version, const char *size,
                  const char *sha256, const char *device, const char *installed,
                  update_release *out, char *why, size_t whyn)
{
	update_release r;
	char want[96];
	long part[4];
	int  parts;
	char *end;

	memset(&r, 0, sizeof r);
	if (!device_ok(device)) { say(why, whyn, "no device name"); return false; }
	if (!version || strlen(version) >= sizeof r.version ||
	    !version_parts(version, part, &parts)) {
		say(why, whyn, "not a version");
		return false;
	}
	if (!update_is_newer(version, installed)) {
		say(why, whyn, "not newer than this device's");
		return false;
	}
	snprintf(want, sizeof want, "TortOS-v%s-%s.img.xz", version, device);
	if (!file || strcmp(file, want)) {
		say(why, whyn, "not this device's image");
		return false;
	}
	if (!size || !*size || !isdigit((unsigned char)*size)) {
		say(why, whyn, "no size");
		return false;
	}
	r.size = strtoll(size, &end, 10);
	if (*end || r.size <= 0 || r.size > IMAGE_MAX) {
		say(why, whyn, "not an image's size");
		return false;
	}
	if (!sha256 || !is_sha256(sha256)) { say(why, whyn, "no SHA-256"); return false; }
	snprintf(r.version, sizeof r.version, "%s", version);
	snprintf(r.file, sizeof r.file, "%s", want);
	memcpy(r.sha256, sha256, 65);
	*out = r;
	return true;
}
