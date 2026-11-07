/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Does the release check read GitHub's answer the way the updater needs it?
 *
 *     make check-release
 *
 * From replies saved in tools/fixtures/release: the real one for 2.1.1, as
 * GitHub's API gave it, and copies of it changed to be later, a prerelease,
 * missing the Brick's image or SHA256SUMS, with a digest cut short, served
 * over http, with an address too long to keep, or cut off partway. Then the version rule's edges, and the
 * answer kept on the card: fresh, stale, for another version, or tampered
 * with. No network: the fetch itself is net.c's, and is tried on the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/update.h"

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

static char *slurp(const char *name, size_t *len)
{
	char path[256];
	FILE *f;
	char *buf;
	long n;

	snprintf(path, sizeof path, "tools/fixtures/release/%s", name);
	if (!(f = fopen(path, "rb"))) { printf("  FAIL: no fixture %s\n", path); exit(1); }
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	rewind(f);
	buf = malloc((size_t)n + 1);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) exit(1);
	buf[n] = '\0';
	fclose(f);
	*len = (size_t)n;
	return buf;
}

/* Parse a fixture for a device: whether it read, and why not. */
static bool parse(const char *name, const char *device, update_release *r, char *why)
{
	size_t len;
	char *json = slurp(name, &len);
	bool ok = update_parse(json, len, device, r, why, 160);

	free(json);
	return ok;
}

static void check_parse(void)
{
	update_release r;
	char why[160];

	printf("reading replies\n");
	CHECK(parse("v2.1.1.json", "brick", &r, why), "2.1.1 for the Brick: %s", why);
	CHECK(!strcmp(r.version, "2.1.1"), "version %s", r.version);
	CHECK(!strcmp(r.file, "TortOS-v2.1.1-brick.img.xz"), "file %s", r.file);
	CHECK(r.size == 40940100, "size %lld", r.size);
	CHECK(!strcmp(r.sha256, "8a06db6bb11c7f1920ea7369b4a33a081ba94c4d1bcc3245650ba11fc34dc14c"),
	      "sha256 %s", r.sha256);
	CHECK(!strcmp(r.url, "https://github.com/ericreinsmidt/TortOS/releases/download/v2.1.1/"
	                     "TortOS-v2.1.1-brick.img.xz"), "url %s", r.url);
	CHECK(!strcmp(r.sums_sha256, "5570b1ef99f6f13789ec08f4fe98833bd6576542e08c9ad9345cc6e30396f0a1"),
	      "sums %s", r.sums_sha256);
	CHECK(strstr(r.sums_url, "/v2.1.1/SHA256SUMS") != NULL, "sums url %s", r.sums_url);

	CHECK(parse("v2.1.1.json", "pixel2", &r, why), "2.1.1 for the Pixel 2: %s", why);
	CHECK(r.size == 36449284 && !strcmp(r.file, "TortOS-v2.1.1-pixel2.img.xz"),
	      "the Pixel's file %s, %lld", r.file, r.size);
	CHECK(!strcmp(r.sha256, "2dc8272b633cc0902c46067116bcf9d1c3e3a027d18ca27c1f40fb4796e3db02"),
	      "the Pixel's sha256 %s", r.sha256);

	CHECK(parse("v2.2.0.json", "brick", &r, why), "a later release: %s", why);
	CHECK(!strcmp(r.version, "2.2.0") && !strcmp(r.file, "TortOS-v2.2.0-brick.img.xz"),
	      "later: %s %s", r.version, r.file);

	CHECK(!parse("v2.1.1.json", "anvil", &r, why) && strstr(why, "TortOS-v2.1.1-anvil.img.xz"),
	      "a device the release doesn't have: %s", why);
	CHECK(!parse("v2.1.1.json", "", &r, why), "no device name");
	CHECK(!parse("v2.1.1.json", "Brick", &r, why), "a device name in capitals");
	CHECK(!parse("v2.1.1.json", "../brick", &r, why), "a device name with a path in it");
	CHECK(!parse("prerelease.json", "brick", &r, why), "a prerelease was taken");
	CHECK(!parse("no-brick.json", "brick", &r, why), "no Brick image, but it read");
	CHECK(parse("no-brick.json", "pixel2", &r, why), "the Pixel's image is still there: %s", why);
	CHECK(!parse("short-digest.json", "brick", &r, why) && strstr(why, "SHA-256"),
	      "a digest cut short: %s", why);
	CHECK(parse("short-digest.json", "pixel2", &r, why), "the Pixel's digest is whole: %s", why);
	CHECK(!parse("http.json", "brick", &r, why) && strstr(why, "download address"),
	      "an http download from the release feed: %s", why);
	CHECK(!parse("long-url.json", "brick", &r, why) && strstr(why, "download address"),
	      "an address too long for its field, cut to fit: %s", why);
	CHECK(!parse("no-sums.json", "brick", &r, why) && strstr(why, "SHA256SUMS"),
	      "no SHA256SUMS: %s", why);
	CHECK(!parse("cut.json", "brick", &r, why), "a reply cut off partway");
	CHECK(!update_parse("", 0, "brick", &r, why, sizeof why), "an empty reply");
	CHECK(!update_parse("not json", 8, "brick", &r, why, sizeof why), "not JSON");
	CHECK(!update_parse("{\"tag_name\":\"2.1.1\",\"assets\":[]}", 32, "brick", &r, why, sizeof why),
	      "a tag without its v");
}

static void check_versions(void)
{
	static const struct { const char *a, *b; bool newer; } cases[] = {
		{ "2.2.0", "2.1.1", true },  { "2.1.1", "2.1.1", false }, { "2.1.0", "2.1.1", false },
		{ "2.10.0", "2.9.0", true }, { "2.9.0", "2.10.0", false }, { "2.1", "2.1.0", false },
		{ "2.1.0", "2.1", false },   { "2.1.0.1", "2.1", true },   { "3", "2.99.99", true },
		{ "10.0.0", "9.9.9.9", true },
		/* not versions, either way round: never newer */
		{ "local", "2.1.1", false }, { "2.2.0", "local", false }, { "2.2.0", "check", false },
		{ "", "1", false },          { "2..1", "1", false },       { "2.1.", "1", false },
		{ ".2", "1", false },        { "1.2.3.4.5", "1", false },  { "123456", "1", false },
		{ "2.2.0-rc1", "2.1.1", false }, { "v2.2.0", "2.1.1", false }, { " 2.2", "2.1", false },
	};

	printf("versions\n");
	for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
		CHECK(update_is_newer(cases[i].a, cases[i].b) == cases[i].newer,
		      "%s newer than %s should be %s", cases[i].a, cases[i].b,
		      cases[i].newer ? "true" : "false");
}

/* Write a cache file from text, as anyone with the card could. */
static void put(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (!f) exit(1);
	fputs(text, f);
	fclose(f);
}

static void check_cache(void)
{
	const char *path = "build-native/release-check.cache";
	const time_t t = 1762500000;
	update_release r, back;
	update_state st;
	char why[160], text[4096], *at;
	FILE *f;
	size_t n;

	printf("the answer kept on the card\n");
	remove(path);
	CHECK(!update_cache_read(path, t, "2.1.1", &st, &back), "no file, but an answer");

	if (!parse("v2.2.0.json", "brick", &r, why)) { printf("  FAIL: %s\n", why); failures++; return; }
	CHECK(update_cache_write(path, t, "2.1.1", UPDATE_WAITING, &r), "writing");
	memset(&back, 0, sizeof back);
	CHECK(update_cache_read(path, t + 3600, "2.1.1", &st, &back) && st == UPDATE_WAITING,
	      "an hour later, waiting");
	CHECK(!memcmp(&back, &r, sizeof r) || (!strcmp(back.version, r.version) &&
	      !strcmp(back.file, r.file) && !strcmp(back.url, r.url) && back.size == r.size &&
	      !strcmp(back.sha256, r.sha256) && !strcmp(back.sums_url, r.sums_url) &&
	      !strcmp(back.sums_sha256, r.sums_sha256)), "what came back differs");
	CHECK(update_cache_read(path, t + UPDATE_CHECK_EVERY_S - 1, "2.1.1", &st, &back),
	      "a second short of a day");
	CHECK(!update_cache_read(path, t + UPDATE_CHECK_EVERY_S, "2.1.1", &st, &back),
	      "a day later, still kept");
	CHECK(!update_cache_read(path, t - 60, "2.1.1", &st, &back), "the clock went back");
	CHECK(!update_cache_read(path, t + 60, "2.2.0", &st, &back),
	      "kept across an update to the version it offered");
	CHECK(!update_cache_read(path, t + 60, "2.3.0", &st, &back), "kept for another version");

	CHECK(update_cache_write(path, t, "2.2.0", UPDATE_CURRENT, &r), "writing up to date");
	CHECK(update_cache_read(path, t + 60, "2.2.0", &st, &back) && st == UPDATE_CURRENT,
	      "up to date, read back");
	CHECK(!update_cache_write(path, t, "2.2.0", UPDATE_UNKNOWN, &r), "an unknown was kept");

	/* Tampered with: each change on its own must make it no answer */
	update_cache_write(path, t, "2.1.1", UPDATE_WAITING, &r);
	f = fopen(path, "r");
	n = fread(text, 1, sizeof text - 1, f);
	fclose(f);
	text[n] = '\0';
	{
		static const struct { const char *from, *to, *what; } bad[] = {
			{ "sha256=8a06", "sha256=8A06", "a digest in capitals" },
			{ "sha256=8a06", "sha256=8a0", "a digest cut short" },
			{ "url=https://", "url=http://", "an http download" },
			{ "file=TortOS-v2.2.0-brick", "file=../TortOS-v2.2.0-brick", "a file with a path" },
			{ "file=TortOS-v2.2.0-brick.img.xz", "file=TortOS-v2.2.0-brick.img", "not an .img.xz" },
			{ "file=TortOS-v2.2.0", "file=TortOS-v2.3.0", "a file of another version" },
			{ "version=2.2.0", "version=2.1.1", "no newer than installed" },
			{ "state=waiting", "state=maybe", "an unknown state" },
			{ "size=40940100", "size=0", "no size" },
		};
		for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
			char changed[4096];
			char *hit;

			snprintf(changed, sizeof changed, "%s", text);
			hit = strstr(changed, bad[i].from);
			if (!hit) { printf("  FAIL: no \"%s\" to change\n", bad[i].from); failures++; continue; }
			memmove(hit + strlen(bad[i].to), hit + strlen(bad[i].from),
			        strlen(hit + strlen(bad[i].from)) + 1);
			memcpy(hit, bad[i].to, strlen(bad[i].to));
			put(path, changed);
			CHECK(!update_cache_read(path, t + 60, "2.1.1", &st, &back), "%s was taken", bad[i].what);
		}
	}
	/* A value longer than its field is refused, not cut to fit */
	at = strstr(text, "url=");
	if (at) {
		char changed[4096];
		int  keep = (int)(at - text) + 4;

		snprintf(changed, sizeof changed, "%.*s%0600d%s", keep, text, 0, at + 4);
		put(path, changed);
		CHECK(!update_cache_read(path, t + 60, "2.1.1", &st, &back), "an over-long address was taken");
	}
	remove(path);
}

int main(void)
{
	check_parse();
	check_versions();
	check_cache();
	if (failures) {
		printf("\n%d FAILED\n", failures);
		return 1;
	}
	printf("\nok: every release check passed\n");
	return 0;
}
