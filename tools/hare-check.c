/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Can a browser get at the card without the PIN?
 *
 *     make check-hare
 *
 * check-xfer proves a path cannot climb out of a root. This proves the routes
 * that use it are actually behind the lock - which is a separate claim, and the
 * one that fails when somebody adds a convenient endpoint and forgets. Every
 * route is tried twice: once with no session at all, and once with a session,
 * so a route that is accidentally public shows up as a pass where a 401 was
 * wanted.
 *
 * Also here: the lockout, which is the entire reason a four-digit PIN is a lock
 * rather than a doorbell. Ten thousand guesses is seconds of scripting; five
 * tries and a wait is not.
 *
 * A real server on a real socket, against a real directory tree in /tmp.
 */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include "../src/hare.h"
#include "../src/xfer.h"

static int failures;

/* The art match route's handler, standing in for the launcher's: how much of
 * the body it was handed. */
static size_t g_art_seen;

static char *art_seen(const char *folder, const char *body, size_t *len,
                      char *why, size_t wn)
{
	char *out = malloc(3);

	(void)folder; (void)why; (void)wn;
	g_art_seen = strlen(body);
	if (out) { memcpy(out, "{}", 3); *len = 2; }
	return out;
}

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		if (!(cond)) {                                                        \
			printf("  FAIL: ");                                               \
			printf(__VA_ARGS__);                                              \
			printf("\n        at %s:%d: %s\n", __FILE__, __LINE__, #cond);    \
			failures++;                                                       \
		}                                                                     \
	} while (0)

static char g_root[128], g_card[160], g_roms[160], g_web[160], g_shared[160];

/* What the update hook was handed, for the update checks. */
static int            g_up_calls;
static update_release g_up_offer;
static void up_received(const update_release *offer) { g_up_calls++; g_up_offer = *offer; }
static char g_cookie[128];

/* ---- the server on its own thread ------------------------------------ */

static pthread_t    g_thread;
static volatile int g_stop;

static void *server_loop(void *u)
{
	(void)u;
	while (!g_stop) { hare_poll(); usleep(200); }
	return NULL;
}

static bool serve_up(void)
{
	g_stop = 0;
	if (!hare_start(g_roms, g_card, g_shared, g_web)) return false;
	pthread_create(&g_thread, NULL, server_loop, NULL);
	return true;
}

static void serve_down(void)
{
	if (g_thread) { g_stop = 1; pthread_join(g_thread, NULL); g_thread = 0; }
	hare_stop();
}

/* ---- an ordinary client ---------------------------------------------- */

static bool complete(const char *out, size_t got)
{
	const char *blank = strstr(out, "\r\n\r\n");
	const char *cl;
	long len;

	if (!blank) return false;
	cl = strstr(out, "Content-Length: ");
	if (!cl || cl > blank) return false;
	len = strtol(cl + 16, NULL, 10);
	return (size_t)(blank + 4 - out) + (size_t)len <= got;
}

/* One request, one answer. Returns the status, and copies the body out. */
static int req(const char *method, const char *path, const char *cookie,
               const void *body, size_t blen, char *outbody, size_t outn)
{
	struct sockaddr_in a;
	struct timeval tv = { 3, 0 };
	/* Room for the biggest listing the device sends (JSON_MAX, 2 MB), and
	 * static, as that is no size for a stack. The server is on its own
	 * thread, but only this one ever reads it. */
	static char resp[4 * 1024 * 1024];
	char head[1024];
	size_t got = 0, sent = 0;
	int fd, n, status = 0;

	fd = socket(AF_INET, SOCK_STREAM, 0);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = htons((unsigned short)hare_port());
	if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) {
		printf("  FAIL: connect: %s\n", strerror(errno));
		failures++;
		close(fd);
		return -1;
	}
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

	n = snprintf(head, sizeof head,
	             "%s %s HTTP/1.1\r\nHost: x\r\n%s%s%s"
	             "Content-Length: %zu\r\nConnection: close\r\n\r\n",
	             method, path,
	             cookie ? "Cookie: hare=" : "", cookie ? cookie : "",
	             cookie ? "\r\n" : "", blen);
	while (sent < (size_t)n) {
		ssize_t w = write(fd, head + sent, (size_t)n - sent);
		if (w <= 0) break;
		sent += (size_t)w;
	}
	sent = 0;
	while (sent < blen) {
		ssize_t w = write(fd, (const char *)body + sent, blen - sent);
		if (w <= 0) break;
		sent += (size_t)w;
	}
	while (got + 1 < sizeof resp) {
		ssize_t r = recv(fd, resp + got, sizeof resp - got - 1, 0);
		if (r <= 0) break;
		got += (size_t)r;
		resp[got] = '\0';
		if (complete(resp, got)) break;
	}
	resp[got] = '\0';
	close(fd);
	sscanf(resp, "HTTP/1.1 %d", &status);
	if (outbody) {
		const char *b = strstr(resp, "\r\n\r\n");
		snprintf(outbody, outn, "%s", b ? b + 4 : "");
	}
	/* Stash a session cookie if one was handed out. */
	{
		const char *sc = strstr(resp, "Set-Cookie: hare=");
		if (sc) {
			sc += 17;
			snprintf(g_cookie, sizeof g_cookie, "%.*s",
			         (int)strcspn(sc, ";\r\n"), sc);
		}
	}
	return status;
}

/* Is this JSON, structurally?
 *
 * The check used to look for substrings - strstr(body, "\"roms\"") - and
 * passed with flying colors over a reply that no parser would accept: a
 * hand-counted string literal had swallowed the opening bracket, so the
 * listing read "entries":{...},{...}] and the page died on it. A check that
 * looks for words in a document it never parses is checking that the words
 * are there, which is not the claim being made.
 *
 * Not a full parser: brackets and braces balanced, in order, ignoring
 * anything inside a string. That is enough to catch every way a hand-built
 * emitter goes wrong, which is by dropping or doubling a delimiter. */
static bool json_ok(const char *s)
{
	int depth = 0;
	bool instr = false;

	if (!s || *s != '{') return false;
	for (; *s; s++) {
		if (instr) {
			if (*s == '\\' && s[1]) s++;
			else if (*s == '"') instr = false;
			continue;
		}
		if (*s == '"') { instr = true; continue; }
		if (*s == '{' || *s == '[') depth++;
		else if (*s == '}' || *s == ']') { if (--depth < 0) return false; }
	}
	return depth == 0 && !instr;
}

/* An "entries" array, opened with a bracket rather than whatever else. */
static bool has_array(const char *s, const char *key)
{
	char pat[64];

	snprintf(pat, sizeof pat, "\"%s\":[", key);
	return strstr(s, pat) != NULL;
}

static int get(const char *path, const char *cookie)
{
	return req("GET", path, cookie, NULL, 0, NULL, 0);
}
static int post(const char *path, const char *cookie)
{
	return req("POST", path, cookie, NULL, 0, NULL, 0);
}

/* ---- a small tree to serve --------------------------------------------- */

static void mkfile(const char *path, const char *text)
{
	FILE *f = fopen(path, "wb");
	if (f) { fputs(text, f); fclose(f); }
}

static void build_tree(void)
{
	char p[256];

	snprintf(g_root, sizeof g_root, "/tmp/tortos-hare-%ld", (long)getpid());
	snprintf(g_card, sizeof g_card, "%s/card", g_root);
	snprintf(g_roms, sizeof g_roms, "%s/card/Roms", g_root);
	snprintf(g_web,  sizeof g_web,  "%s/web", g_root);
	snprintf(g_shared, sizeof g_shared, "%s/shared", g_root);

	mkdir(g_root, 0777);
	mkdir(g_card, 0777);
	mkdir(g_roms, 0777);
	mkdir(g_web, 0777);
	mkdir(g_shared, 0777);
	snprintf(p, sizeof p, "%s/.tortos", g_shared);      mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/.tortos/NES", g_shared);  mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/.tortos/NES/Contra.state", g_shared);
	mkfile(p, "STATEDATA");
	/* The launcher's own, in the same directory as the states. */
	snprintf(p, sizeof p, "%s/.tortos/cheevos.cfg", g_shared);
	mkfile(p, "1447\t6850\ts\n");
	snprintf(p, sizeof p, "%s/Bios", g_card);   mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/Saves", g_card);  mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/NES", g_roms);    mkdir(p, 0777);
	snprintf(p, sizeof p, "%s/NES/Contra.nes", g_roms); mkfile(p, "ROMDATA");
	snprintf(p, sizeof p, "%s/index.html", g_web);      mkfile(p, "<h1>hare</h1>");
	/* Outside every root, and the thing an escape would be aiming at. */
	snprintf(p, sizeof p, "%s/secret.txt", g_card);     mkfile(p, "TOPSECRET");
}

static void rmtree(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (!d) return;
	while ((e = readdir(d))) {
		char full[512];
		struct stat st;

		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
		if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) rmtree(full);
		else remove(full);
	}
	closedir(d);
	rmdir(dir);
}

int main(void)
{
	char body[4096];
	char pin[8];

	build_tree();
	printf("hare: the routes, and who may use them\n");

	if (!serve_up()) {
		printf("  FAIL: the server did not start\n");
		rmtree(g_root);
		return 1;
	}
	snprintf(pin, sizeof pin, "%s", hare_pin());
	CHECK(strlen(pin) == 4, "the PIN is \"%s\", wanted four digits", pin);
	{
		int i, digits = 0;
		for (i = 0; pin[i]; i++) if (pin[i] >= '0' && pin[i] <= '9') digits++;
		CHECK(digits == 4, "the PIN is not four digits");
	}

	printf("  the page is reachable without a PIN, because it IS the prompt:\n");
	CHECK(get("/", NULL) == 200, "the page was not served");

	printf("  nothing else is:\n");
	{
		/* Every route, unauthenticated. A 401 each. This is the check that
		 * fails the day somebody adds an endpoint and forgets the session
		 * test - which is why it lists them rather than sampling. */
		CHECK(get("/api/list", NULL) == 401, "listing the roots needed no PIN");
		CHECK(get("/api/list?p=roms", NULL) == 401, "listing ROMs needed no PIN");
		CHECK(get("/api/file?p=roms/NES/Contra.nes", NULL) == 401,
		      "DOWNLOADING A ROM NEEDED NO PIN");
		CHECK(req("PUT", "/api/file?p=roms/NES/x.nes", NULL, "X", 1, NULL, 0) == 401,
		      "UPLOADING NEEDED NO PIN");
		CHECK(post("/api/delete?p=roms/NES/Contra.nes", NULL) == 401,
		      "DELETING NEEDED NO PIN");
		CHECK(post("/api/rename?p=roms/NES/Contra.nes&to=x", NULL) == 401,
		      "RENAMING NEEDED NO PIN");
		CHECK(post("/api/mkdir?p=roms/NES/sub", NULL) == 401,
		      "MAKING A FOLDER NEEDED NO PIN");
		CHECK(get("/api/update", NULL) == 401, "asking about updates needed no PIN");
		CHECK(req("PUT", "/api/update?file=TortOS-v2.3.0-brick.img.xz&version=2.3.0"
		          "&size=1&sha256=x", NULL, "X", 1, NULL, 0) == 401,
		      "SENDING AN UPDATE NEEDED NO PIN");
		CHECK(post("/api/update?file=TortOS-v2.3.0-brick.img.xz", NULL) == 401,
		      "STARTING AN UPDATE NEEDED NO PIN");
		/* And the file is still there. */
		{
			char p[256];
			struct stat st;
			snprintf(p, sizeof p, "%s/NES/Contra.nes", g_roms);
			CHECK(stat(p, &st) == 0, "an unauthenticated call deleted the ROM");
		}
	}

	printf("  a wrong PIN is refused, and a right one is not:\n");
	CHECK(req("POST", "/api/auth", NULL, "0000", 4, NULL, 0) == 401 ||
	      !strcmp(pin, "0000"), "a wrong PIN was accepted");
	g_cookie[0] = '\0';
	CHECK(req("POST", "/api/auth", NULL, pin, 4, NULL, 0) == 200,
	      "the right PIN was refused");
	CHECK(g_cookie[0] != '\0', "no session cookie came back");
	CHECK(strlen(g_cookie) == 32, "the session token is %zu chars, wanted 32",
	      strlen(g_cookie));

	printf("  a made-up cookie is not a session:\n");
	CHECK(get("/api/list", "00000000000000000000000000000000") == 401,
	      "a guessed token was accepted");
	CHECK(get("/api/list", "short") == 401, "a short token was accepted");

	printf("  with a session, the routes work:\n");
	{
		CHECK(req("GET", "/api/list", g_cookie, NULL, 0, body, sizeof body) == 200,
		      "listing the roots failed");
		CHECK(json_ok(body), "the roots listing is not valid JSON: %s", body);
		CHECK(has_array(body, "entries"),
		      "entries is not an array: %s", body);
		CHECK(strstr(body, "\"roms\"") && strstr(body, "\"bios\"") &&
		      strstr(body, "\"saves\""), "the roots listing is wrong: %s", body);

		CHECK(req("GET", "/api/list?p=roms/NES", g_cookie, NULL, 0,
		          body, sizeof body) == 200, "listing NES failed");
		CHECK(json_ok(body), "the NES listing is not valid JSON: %s", body);
		CHECK(has_array(body, "entries"), "entries is not an array: %s", body);
		CHECK(strstr(body, "Contra.nes") != NULL,
		      "the ROM is missing from the listing: %s", body);

		/* A name with the characters JSON cares about, which is the other way
		 * a hand-built emitter breaks: unescaped, the reply stops parsing. */
		{
			char p[512];
			snprintf(p, sizeof p, "%s/NES/quote\"back\\slash.nes", g_roms);
			mkfile(p, "X");
			CHECK(req("GET", "/api/list?p=roms/NES", g_cookie, NULL, 0,
			          body, sizeof body) == 200, "listing after odd name failed");
			CHECK(json_ok(body),
			      "a name with a quote or backslash broke the JSON: %s", body);
			remove(p);
		}

		CHECK(req("GET", "/api/file?p=roms/NES/Contra.nes", g_cookie, NULL, 0,
		          body, sizeof body) == 200, "download failed");
		CHECK(!strcmp(body, "ROMDATA"), "downloaded \"%s\"", body);

		CHECK(req("PUT", "/api/file?p=roms/NES/New.nes", g_cookie,
		          "NEWROM", 6, NULL, 0) == 200, "upload failed");
		{
			char p[256];
			struct stat st;
			snprintf(p, sizeof p, "%s/NES/New.nes", g_roms);
			CHECK(stat(p, &st) == 0 && st.st_size == 6,
			      "the upload did not land");
			/* The part-file is renamed, not left behind. */
			snprintf(p, sizeof p, "%s/NES/New.nes.part", g_roms);
			CHECK(stat(p, &st) != 0, "a part-file was left behind");
		}

		CHECK(post("/api/rename?p=roms/NES/New.nes&to=Renamed.nes", g_cookie) == 200,
		      "rename failed");
		CHECK(post("/api/delete?p=roms/NES/Renamed.nes", g_cookie) == 200,
		      "delete failed");
	}

	printf("  and a session does not let a path climb out:\n");
	{
		/* xfer_resolve already refuses these; this proves the ROUTES ask it,
		 * which is a different claim and the one that regresses. */
		CHECK(get("/api/file?p=../secret.txt", g_cookie) == 403,
		      "read a file outside every root");
		CHECK(get("/api/file?p=roms/../../secret.txt", g_cookie) == 403,
		      "climbed out of a root");
		CHECK(get("/api/list?p=roms/..", g_cookie) == 403, "listed above a root");
		CHECK(post("/api/delete?p=roms/../../secret.txt", g_cookie) == 403,
		      "DELETED A FILE OUTSIDE EVERY ROOT");
		{
			char p[256];
			struct stat st;
			snprintf(p, sizeof p, "%s/secret.txt", g_card);
			CHECK(stat(p, &st) == 0, "the file outside the roots was deleted");
		}
		/* A rename names a sibling. If it could name a path, it could move a
		 * file into another root - or out of all of them. */
		CHECK(post("/api/rename?p=roms/NES/Contra.nes&to=../../escaped", g_cookie)
		      != 200, "a rename escaped its directory");
		CHECK(post("/api/rename?p=roms/NES/Contra.nes&to=%2e%2e%2fescaped",
		           g_cookie) != 200, "an encoded rename escaped its directory");
		CHECK(post("/api/rename?p=roms/NES/Contra.nes&to=sub%2fx", g_cookie)
		      != 200, "a rename carried a separator");
	}

	printf("  save states are reachable and the config beside them is not:\n");
	{
		CHECK(req("GET", "/api/file?p=states/NES/Contra.state", g_cookie,
		          NULL, 0, body, sizeof body) == 200,
		      "a save state could not be fetched");
		CHECK(!strcmp(body, "STATEDATA"), "the state came back as \"%s\"", body);
		CHECK(get("/api/file?p=states/cheevos.cfg", g_cookie) == 403,
		      "READ THE UNLOCK STORE THAT SITS BESIDE THE SAVE STATES");
		CHECK(post("/api/delete?p=states/cheevos.cfg", g_cookie) == 403,
		      "DELETED THE ONLY RECORD OF UNSUBMITTED UNLOCKS");
		{
			char p[512];
			struct stat st;
			snprintf(p, sizeof p, "%s/.tortos/cheevos.cfg", g_shared);
			CHECK(stat(p, &st) == 0, "cheevos.cfg was deleted");
		}
	}

	/* A body read back by its handler, whole: the art routes stream theirs to
	 * a file and read it in the second call. The file was still open then, its
	 * last bytes still buffered, so a small body arrived empty - a console's
	 * whole cover list, 2026-10-05 - and a large one short by its tail. One
	 * small and one past a stdio buffer, to catch both. */
	printf("  a body is all there when its handler reads it:\n");
	{
		static char big[100000];
		char resp[256];
		size_t i;

		hare_set_art(NULL, art_seen, NULL, NULL);
		CHECK(req("POST", "/api/art/match?s=NES", g_cookie, "abc\ndef", 7,
		          resp, sizeof resp) == 200 && g_art_seen == 7,
		      "a 7-byte body reached the handler as %zu bytes", g_art_seen);
		for (i = 0; i < sizeof big; i++) big[i] = (char)('a' + i % 26);
		CHECK(req("POST", "/api/art/match?s=NES", g_cookie, big, sizeof big,
		          resp, sizeof resp) == 200 && g_art_seen == sizeof big,
		      "a %zu-byte body reached the handler as %zu bytes", sizeof big, g_art_seen);
		hare_set_art(NULL, NULL, NULL, NULL);
	}

	/* Music listed as the page draws it: an album marked as one with its
	 * cover, an artist with their albums' covers. Two covers for the artist,
	 * because the second is the one that went out as seven bytes of garbage
	 * on 2026-10-05 and left a folder of artists unopenable. */
	printf("  Music's albums and artists come with their covers:\n");
	{
		static const char *const DIRS[] = {
			"Music", "Music/.media", "Music/Flat", "Music/Band", "Music/Band/.media",
			"Music/Band/One", "Music/Band/Two",
		};
		static const char *const FILES[] = {
			"Music/Flat/01 A.mp3", "Music/.media/Flat.jpg",
			"Music/Band/One/01 B.mp3", "Music/Band/Two/01 C.mp3",
			"Music/Band/.media/One.jpg", "Music/Band/.media/Two.png",
		};
		char p[512], body[4096];
		size_t i;

		for (i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++) {
			snprintf(p, sizeof p, "%s/%s", g_card, DIRS[i]);
			mkdir(p, 0777);
		}
		for (i = 0; i < sizeof FILES / sizeof FILES[0]; i++) {
			snprintf(p, sizeof p, "%s/%s", g_card, FILES[i]);
			mkfile(p, "X");
		}
		CHECK(req("GET", "/api/list?p=music", g_cookie, NULL, 0, body, sizeof body) == 200,
		      "listing Music failed");
		CHECK(json_ok(body), "the Music listing is not valid JSON: %s", body);
		CHECK(strstr(body, "\"album\":true,\"cover\":\"music/.media/Flat.jpg\"") != NULL,
		      "the album and its cover: %s", body);
		CHECK(strstr(body, "\"covers\":[\"music/Band/.media/One.jpg\","
		                   "\"music/Band/.media/Two.png\"]") != NULL,
		      "the artist and both albums' covers, in order: %s", body);
		for (i = sizeof FILES / sizeof FILES[0]; i-- > 0; ) {
			snprintf(p, sizeof p, "%s/%s", g_card, FILES[i]);
			remove(p);
		}
		for (i = sizeof DIRS / sizeof DIRS[0]; i-- > 1; ) {
			snprintf(p, sizeof p, "%s/%s", g_card, DIRS[i]);
			rmdir(p);
		}
	}

	/* A folder as big as real ones get, whole. The listing was one 256 KB
	 * reply, and a folder past that would not open: a full SNES set with
	 * covers on Reddit, 2026-11-13. A game with a cover is about 180 bytes and
	 * an artist with four albums' covers about 290, so both of these are well
	 * past the old limit: 3,500 games, a full No-Intro SNES set, and 1,500
	 * artists. Each must be listed once, with its cover. */
	printf("  a full set of games, and a big Music folder, list whole:\n");
	{
		enum { GAMES = 3500, ARTISTS = 1500, ALBUMS = 4 };
		static unsigned char seen[GAMES > ARTISTS ? GAMES : ARTISTS];
		const size_t bodyn = 4 * 1024 * 1024;
		char *body = malloc(bodyn), p[512], want[1024], *at;
		int i, k, n, once;

		snprintf(p, sizeof p, "%s/SNES", g_roms);         mkdir(p, 0777);
		snprintf(p, sizeof p, "%s/SNES/.media", g_roms);  mkdir(p, 0777);
		for (i = 0; i < GAMES; i++) {
			snprintf(p, sizeof p, "%s/SNES/Super Test Game %04d (USA).sfc", g_roms, i);
			mkfile(p, "X");
			snprintf(p, sizeof p, "%s/SNES/.media/Super Test Game %04d (USA).png", g_roms, i);
			mkfile(p, "X");
		}
		CHECK(body && req("GET", "/api/list?p=roms/SNES", g_cookie, NULL, 0,
		                  body, bodyn) == 200, "listing %d games failed", GAMES);
		CHECK(body && strlen(body) > 256 * 1024,
		      "the games listing is %zu bytes, not past the old 256 KB",
		      body ? strlen(body) : 0);
		CHECK(body && json_ok(body), "the games listing is not valid JSON");
		memset(seen, 0, sizeof seen);
		for (at = body; at && (at = strstr(at, "{\"name\":\"Super Test Game ")); at++) {
			if (sscanf(at, "{\"name\":\"Super Test Game %d", &k) != 1 ||
			    k < 0 || k >= GAMES) continue;
			n = snprintf(want, sizeof want,
			             "{\"name\":\"Super Test Game %04d (USA).sfc\","
			             "\"path\":\"roms/SNES/Super Test Game %04d (USA).sfc\","
			             "\"cover\":\"roms/SNES/.media/Super Test Game %04d (USA).png\","
			             "\"dir\":false,", k, k, k);
			if (!strncmp(at, want, (size_t)n) && seen[k] < 255) seen[k]++;
		}
		for (i = 0, once = 0; i < GAMES; i++) once += seen[i] == 1;
		CHECK(once == GAMES, "%d of %d games were listed once with their cover",
		      once, GAMES);

		snprintf(p, sizeof p, "%s/Music", g_card);  mkdir(p, 0777);
		for (i = 0; i < ARTISTS; i++) {
			snprintf(p, sizeof p, "%s/Music/Test Artist %04d", g_card, i);
			mkdir(p, 0777);
			snprintf(p, sizeof p, "%s/Music/Test Artist %04d/.media", g_card, i);
			mkdir(p, 0777);
			for (k = 1; k <= ALBUMS; k++) {
				snprintf(p, sizeof p, "%s/Music/Test Artist %04d/Album %d", g_card, i, k);
				mkdir(p, 0777);
				snprintf(p, sizeof p, "%s/Music/Test Artist %04d/.media/Album %d.jpg",
				         g_card, i, k);
				mkfile(p, "X");
			}
		}
		CHECK(body && req("GET", "/api/list?p=music", g_cookie, NULL, 0,
		                  body, bodyn) == 200, "listing %d artists failed", ARTISTS);
		CHECK(body && strlen(body) > 256 * 1024,
		      "the Music listing is %zu bytes, not past the old 256 KB",
		      body ? strlen(body) : 0);
		CHECK(body && json_ok(body), "the Music listing is not valid JSON");
		memset(seen, 0, sizeof seen);
		for (at = body; at && (at = strstr(at, "{\"name\":\"Test Artist ")); at++) {
			if (sscanf(at, "{\"name\":\"Test Artist %d", &k) != 1 ||
			    k < 0 || k >= ARTISTS) continue;
			n = snprintf(want, sizeof want,
			             "{\"name\":\"Test Artist %04d\",\"path\":\"music/Test Artist %04d\","
			             "\"covers\":[\"music/Test Artist %04d/.media/Album 1.jpg\","
			             "\"music/Test Artist %04d/.media/Album 2.jpg\","
			             "\"music/Test Artist %04d/.media/Album 3.jpg\","
			             "\"music/Test Artist %04d/.media/Album 4.jpg\"],\"dir\":true,",
			             k, k, k, k, k, k);
			if (!strncmp(at, want, (size_t)n) && seen[k] < 255) seen[k]++;
		}
		for (i = 0, once = 0; i < ARTISTS; i++) once += seen[i] == 1;
		CHECK(once == ARTISTS,
		      "%d of %d artists were listed once with their four covers", once, ARTISTS);

		free(body);
		snprintf(p, sizeof p, "%s/SNES", g_roms);  rmtree(p);
		for (i = 0; i < ARTISTS; i++) {
			snprintf(p, sizeof p, "%s/Music/Test Artist %04d", g_card, i);
			rmtree(p);
		}
	}

	printf("  deleting a folder with what is in it, where that is allowed:\n");
	{
		char p[512], body[1024];
		struct stat st;
		FILE *f;
		int i;

		snprintf(p, sizeof p, "%s/Music/Artist", g_card);          mkdir(p, 0777);
		snprintf(p, sizeof p, "%s/Music/Artist/Album", g_card);    mkdir(p, 0777);
		snprintf(p, sizeof p, "%s/Music/Artist/Album/01 A.mp3", g_card);
		if ((f = fopen(p, "w"))) fclose(f);
		snprintf(p, sizeof p, "%s/Music/Artist/Album/02 B.mp3", g_card);
		if ((f = fopen(p, "w"))) fclose(f);
		snprintf(p, sizeof p, "%s/NES/Kept.nes", g_roms);
		if ((f = fopen(p, "w"))) fclose(f);

		CHECK(req("GET", "/api/count?p=music/Artist", g_cookie, NULL, 0, body, sizeof body) == 200 &&
		      json_ok(body) && strstr(body, "\"files\":2") && strstr(body, "\"folders\":1") &&
		      strstr(body, "\"rule\":\"all\""),
		      "what an artist's folder holds, and that it may go whole: %s", body);
		CHECK(post("/api/delete?p=music/Artist", g_cookie) == 409,
		      "a full folder went without all=1");
		CHECK(post("/api/delete?p=music/Artist&all=1", g_cookie) == 200,
		      "an artist's folder did not go whole");
		snprintf(p, sizeof p, "%s/Music/Artist", g_card);
		CHECK(stat(p, &st) != 0, "the artist's folder is still there");

		CHECK(req("GET", "/api/count?p=roms/NES", g_cookie, NULL, 0, body, sizeof body) == 200 &&
		      strstr(body, "\"rule\":\"empty\""), "a console's folder is empty-only: %s", body);
		CHECK(post("/api/delete?p=roms/NES&all=1", g_cookie) == 409,
		      "A WHOLE CONSOLE'S GAMES WENT IN ONE CLICK");
		snprintf(p, sizeof p, "%s/NES/Kept.nes", g_roms);
		CHECK(stat(p, &st) == 0, "a game went with the refused console folder");

		CHECK(post("/api/delete?p=music", g_cookie) == 403, "an empty root was deleted");
		snprintf(p, sizeof p, "%s/Music", g_card);
		CHECK(stat(p, &st) == 0, "the Music folder is gone");
		CHECK(post("/api/delete?p=roms&all=1", g_cookie) == 403, "THE ROM ROOT WAS DELETED");

		/* Past the limit it is asked for in parts, and nothing is touched. */
		snprintf(p, sizeof p, "%s/Music/Big", g_card);             mkdir(p, 0777);
		for (i = 0; i <= HARE_DELETE_MAX; i++) {
			snprintf(p, sizeof p, "%s/Music/Big/%04d.mp3", g_card, i);
			if ((f = fopen(p, "w"))) fclose(f);
		}
		CHECK(post("/api/delete?p=music/Big&all=1", g_cookie) == 413,
		      "a folder past the limit was deleted");
		snprintf(p, sizeof p, "%s/Music/Big/0000.mp3", g_card);
		CHECK(stat(p, &st) == 0, "a refused folder lost files");
	}

	printf("  updates over Down To The Wire:\n");
	{
		static const char sha[] =
			"8a06db6bb11c7f1920ea7369b4a33a081ba94c4d1bcc3245650ba11fc34dc14c";
		char dir[256], p[512], url[512], got[64];
		struct stat st;
		FILE *f;
		size_t n;

		CHECK(get("/api/update", g_cookie) == 404, "updates answered with no folder set");
		snprintf(dir, sizeof dir, "%s/.update", g_card);
		mkdir(dir, 0777);
		snprintf(p, sizeof p, "%s/TortOS-v2.2.5-brick.img.xz", dir); mkfile(p, "old");
		snprintf(p, sizeof p, "%s/update.log", dir);                mkfile(p, "log");
		hare_set_update(dir, "2.2.0", "brick", up_received);

		CHECK(req("GET", "/api/update", g_cookie, NULL, 0, body, sizeof body) == 200,
		      "no answer about updates");
		CHECK(json_ok(body), "the update answer isn't JSON: %s", body);
		CHECK(strstr(body, "\"version\":\"2.2.0\"") && strstr(body, "\"device\":\"brick\"") &&
		      strstr(body, "\"state\":\"idle\""), "the update answer: %s", body);
		CHECK(strstr(body, "\"have\":\"2.2.5\""), "the image already there: %s", body);
		CHECK(strstr(body, "\"feed\":\"" UPDATE_FEED "\"") &&
		      strstr(body, "\"files\":\"" UPDATE_FILES "\""),
		      "where the page asks and downloads: %s", body);

		/* The room: free space, plus what the PUT clears first. A hole of
		 * 1 GiB counts as that size and takes no space, so the difference
		 * stands out from whatever else this computer is writing. */
		{
			char *at = strstr(body, "\"room\":");
			long long room = at ? strtoll(at + 7, NULL, 10) : -1, room2;
			const long long GiB = 1LL << 30;

			CHECK(room > 0, "the room for an update: %s", body);
			snprintf(p, sizeof p, "%s/TortOS-v2.2.6-brick.img.xz.part", dir);
			mkfile(p, "");
			CHECK(truncate(p, GiB) == 0, "making a hole");
			req("GET", "/api/update", g_cookie, NULL, 0, body, sizeof body);
			at = strstr(body, "\"room\":");
			room2 = at ? strtoll(at + 7, NULL, 10) : -1;
			CHECK(room2 - room > GiB / 2 && room2 - room < GiB * 3 / 2,
			      "a leftover the PUT clears wasn't counted: %lld then %lld", room, room2);
			unlink(p);

			snprintf(p, sizeof p, "%s/result", dir);
			mkfile(p, "");
			CHECK(truncate(p, GiB) == 0, "making a hole");
			req("GET", "/api/update", g_cookie, NULL, 0, body, sizeof body);
			at = strstr(body, "\"room\":");
			room2 = at ? strtoll(at + 7, NULL, 10) : -1;
			CHECK(room2 - room < GiB / 2, "the updater's result was counted as room");
			unlink(p);

			/* A card that has never had an update has no folder yet */
			snprintf(p, sizeof p, "%s/not-made-yet", dir);
			hare_set_update(p, "2.2.0", "brick", up_received);
			req("GET", "/api/update", g_cookie, NULL, 0, body, sizeof body);
			at = strstr(body, "\"room\":");
			CHECK(at && strtoll(at + 7, NULL, 10) > 0, "no room with no folder: %s", body);
			hare_set_update(dir, "2.2.0", "brick", up_received);
		}

#define OFFER(file, ver, size) "/api/update?file=" file "&version=" ver "&size=" size "&sha256="
		snprintf(url, sizeof url, OFFER("TortOS-v2.3.0-pixel2.img.xz", "2.3.0", "9") "%s", sha);
		CHECK(req("PUT", url, g_cookie, "IMAGEDATA", 9, NULL, 0) == 400,
		      "the Pixel's image was taken on the Brick");
		snprintf(url, sizeof url, OFFER("TortOS-v2.1.0-brick.img.xz", "2.1.0", "9") "%s", sha);
		CHECK(req("PUT", url, g_cookie, "IMAGEDATA", 9, NULL, 0) == 400,
		      "an older version was taken");
		snprintf(url, sizeof url, OFFER("../TortOS-v2.3.0-brick.img.xz", "2.3.0", "9") "%s", sha);
		CHECK(req("PUT", url, g_cookie, "IMAGEDATA", 9, NULL, 0) == 400,
		      "a name with a path in it was taken");
		snprintf(url, sizeof url, OFFER("TortOS-v2.3.0-brick.img.xz", "2.3.0", "12") "%s", sha);
		CHECK(req("PUT", url, g_cookie, "IMAGEDATA", 9, NULL, 0) == 400,
		      "a body shorter than the size it gave was taken");
		CHECK(g_up_calls == 0, "a refused update reached the launcher");
		snprintf(p, sizeof p, "%s/TortOS-v2.2.5-brick.img.xz", dir);
		CHECK(stat(p, &st) == 0, "a refused update cleared the folder");

		snprintf(url, sizeof url, OFFER("TortOS-v2.3.0-brick.img.xz", "2.3.0", "9") "%s", sha);
		CHECK(req("PUT", url, g_cookie, "IMAGEDATA", 9, NULL, 0) == 200, "a good update was refused");
		snprintf(p, sizeof p, "%s/TortOS-v2.3.0-brick.img.xz", dir);
		got[0] = '\0';
		if ((f = fopen(p, "rb"))) { n = fread(got, 1, sizeof got - 1, f); got[n] = '\0'; fclose(f); }
		CHECK(!strcmp(got, "IMAGEDATA"), "the update on the card is \"%s\"", got);
		CHECK(g_up_calls == 1 && !strcmp(g_up_offer.version, "2.3.0") &&
		      g_up_offer.size == 9 && !strcmp(g_up_offer.sha256, sha),
		      "the launcher was told %d times, of %s", g_up_calls, g_up_offer.version);
		snprintf(p, sizeof p, "%s/TortOS-v2.2.5-brick.img.xz", dir);
		CHECK(stat(p, &st) != 0, "the older image is still there");
		snprintf(p, sizeof p, "%s/update.log", dir);
		CHECK(stat(p, &st) == 0, "the updater's log went");
		snprintf(p, sizeof p, "%s/TortOS-v2.3.0-brick.img.xz.part", dir);
		CHECK(stat(p, &st) != 0, "a part-file was left");

		req("GET", "/api/update", g_cookie, NULL, 0, body, sizeof body);
		CHECK(strstr(body, "\"state\":\"checking\""), "while checking: %s", body);
		CHECK(req("PUT", url, g_cookie, "IMAGEDATA", 9, NULL, 0) == 409,
		      "a second update was taken over one being checked");
		snprintf(url, sizeof url, OFFER("TortOS-v2.3.0-brick.img.xz", "2.3.0", "9") "%s", sha);
		CHECK(post(url, g_cookie) == 409, "the copy was used over one being checked");

		hare_update_state("refused", "it didn't \"match\"");
		req("GET", "/api/update", g_cookie, NULL, 0, body, sizeof body);
		CHECK(json_ok(body) && strstr(body, "\"state\":\"refused\""),
		      "a reason with quotes in it broke the answer: %s", body);
		hare_update_state("notnow", NULL);
		req("GET", "/api/update", g_cookie, NULL, 0, body, sizeof body);
		CHECK(strstr(body, "\"state\":\"notnow\"") && strstr(body, "\"have\":\"2.3.0\""),
		      "after Not now: %s", body);
		CHECK(post(url, g_cookie) == 200, "the copy already there wasn't used");
		CHECK(g_up_calls == 2, "the launcher was told %d times, wanted 2", g_up_calls);
		hare_update_state("notnow", NULL);
		snprintf(url, sizeof url, OFFER("TortOS-v2.3.0-brick.img.xz", "2.3.0", "10") "%s", sha);
		CHECK(post(url, g_cookie) == 404, "a copy of another size was used");
		CHECK(g_up_calls == 2, "a copy of another size reached the launcher");

		/* Safari's pad byte comes off, as for any upload. */
		snprintf(url, sizeof url, OFFER("TortOS-v2.3.1-brick.img.xz", "2.3.1", "9") "%s&pad=1", sha);
		CHECK(req("PUT", url, g_cookie, "IMAGEDATA!", 10, NULL, 0) == 200, "a padded update");
		snprintf(p, sizeof p, "%s/TortOS-v2.3.1-brick.img.xz", dir);
		CHECK(stat(p, &st) == 0 && st.st_size == 9, "the padded update is %lld bytes",
		      (long long)st.st_size);
		snprintf(p, sizeof p, "%s/TortOS-v2.3.0-brick.img.xz", dir);
		CHECK(stat(p, &st) != 0, "the earlier image wasn't cleared for the new one");
#undef OFFER
		hare_set_update(NULL, NULL, NULL, NULL);
		CHECK(get("/api/update", g_cookie) == 404, "updates still answered once unset");
	}

	printf("  the lockout, which is what makes four digits a lock:\n");
	{
		char wrong[5];
		int i, last = 0;

		/* A PIN that is definitely not the right one. */
		snprintf(wrong, sizeof wrong, "%04d",
		         (atoi(pin) + 1) % 10000);
		for (i = 0; i < 6; i++)
			last = req("POST", "/api/auth", NULL, wrong, 4, NULL, 0);
		CHECK(last == 429, "after six wrong PINs the answer was %d, wanted 429",
		      last);
		/* And the right PIN is refused too while locked out - otherwise the
		 * lockout is advice rather than a lock. */
		CHECK(req("POST", "/api/auth", NULL, pin, 4, NULL, 0) == 429,
		      "the lockout let the right PIN through, so it only slows a "
		      "guesser down between attempts");
	}

	printf("  closing the screen takes the whole thing down:\n");
	serve_down();
	CHECK(hare_port() == 0, "still listening after stop");
	CHECK(hare_pin()[0] == '\0', "the PIN outlived the screen");

	rmtree(g_root);
	if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
	printf("\nok: every check passed\n");
	return 0;
}
