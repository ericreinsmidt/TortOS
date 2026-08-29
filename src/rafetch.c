/* SPDX-License-Identifier: 0BSD */
/* See rafetch.h. The network is ranet.c, the JSON is rajson.c, the hash is
 * rahash.c; this is the RetroAchievements workflow those three serve. */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "rafetch.h"
#include "rahash.h"
#include "rajson.h"
#include "ranet.h"

static char g_user[RA_USER_MAX];
static char g_token[RA_TOKEN_MAX];

/* ------------------------------------------------------ credentials ----- */

bool ra_signed_in(void) { return g_user[0] && g_token[0]; }
const char *ra_user(void) { return g_user; }

void ra_creds_clear(void) { g_user[0] = '\0'; g_token[0] = '\0'; }

bool ra_creds_load(const char *path)
{
	char line[256];
	FILE *f = fopen(path, "r");

	ra_creds_clear();
	if (!f) return false;
	/* A value too long for its field is REFUSED, not stored short. Half a
	 * token still looks like a token: it would read as signed in and every
	 * request would come back with an auth error that says nothing about
	 * the real cause. */
	while (fgets(line, sizeof line, f)) {
		const char *v = NULL;
		char *dst = NULL;
		size_t cap = 0;

		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "user=", 5)) {
			v = line + 5; dst = g_user; cap = sizeof g_user;
		} else if (!strncmp(line, "token=", 6)) {
			v = line + 6; dst = g_token; cap = sizeof g_token;
		} else {
			continue;
		}
		if (strlen(v) >= cap) {
			fprintf(stderr, "ra: %s has an over-long value; ignoring it\n", path);
			ra_creds_clear();
			break;
		}
		memcpy(dst, v, strlen(v) + 1);
	}
	fclose(f);
	return ra_signed_in();
}

bool ra_creds_save(const char *path)
{
	FILE *f;
	int fd;

	if (!ra_signed_in()) { unlink(path); return true; }
	/* 0600 from creation. A token is a credential: anything that can read it
	 * can act as this account. */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) return false;
	f = fdopen(fd, "w");
	if (!f) { close(fd); return false; }
	fprintf(f, "user=%s\ntoken=%s\n", g_user, g_token);
	fclose(f);
	return true;
}

bool ra_sign_in(const char *user, const char *password, char *err, size_t errn)
{
	char body[2048];
	ra_field f[3];
	jsv root, v;

	if (err && errn) err[0] = '\0';
	if (!user || !*user || !password || !*password) {
		if (err) snprintf(err, errn, "user and password are both needed");
		return false;
	}
	if (!ra_online()) {
		if (err) snprintf(err, errn, "not on a network");
		return false;
	}

	f[0].k = "r"; f[0].v = "login2";
	f[1].k = "u"; f[1].v = user;
	f[2].k = "p"; f[2].v = password;
	if (ra_post_buf(f, 3, body, sizeof body, 20) < 0) {
		if (err) snprintf(err, errn, "could not reach retroachievements.org");
		return false;
	}

	root = js_root(body, strlen(body));
	if (!js_member(root, "Success", &v) || !js_is_true(v)) {
		char msg[160] = "";
		if (js_member(root, "Error", &v)) js_str(v, msg, sizeof msg);
		if (err) snprintf(err, errn, "%s", msg[0] ? msg : "refused");
		return false;
	}
	if (!js_member(root, "Token", &v) || !js_str(v, g_token, sizeof g_token) ||
	    !g_token[0]) {
		if (err) snprintf(err, errn, "no token in the reply");
		return false;
	}
	snprintf(g_user, sizeof g_user, "%s", user);
	return true;
}

/* ---------------------------------------------------------- the set ----- */

long ra_game_for_rom(const char *rom_path, const char *tag)
{
	char hash[33], body[512];
	ra_field f[2];
	jsv root, v;

	if (!ra_hash_rom(rom_path, tag, hash)) return -1;
	if (!ra_online()) return -1;

	f[0].k = "r"; f[0].v = "gameid";
	f[1].k = "m"; f[1].v = hash;
	if (ra_post_buf(f, 2, body, sizeof body, 20) < 0) return -1;

	root = js_root(body, strlen(body));
	if (!js_member(root, "Success", &v) || !js_is_true(v)) return -1;
	if (!js_member(root, "GameID", &v)) return -1;
	return js_int(v);
}

/* A tab or a newline in a title would become a field in the set file. RA's
 * titles have neither today; this is here so that the day one does, the file
 * stays readable rather than becoming subtly wrong. */
static void tsv_safe(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

static char *read_whole(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	*len = 0;
	if (!f) return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n <= 0) { fclose(f); return NULL; }
	buf = malloc((size_t)n + 1);
	if (!buf) { fclose(f); return NULL; }
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
	fclose(f);
	buf[n] = '\0';
	*len = (size_t)n;
	return buf;
}

/* The conversion, separated from the fetch so it can be checked against real
 * responses with no network - see tools/raset-check.c. */
bool ra_set_from_json(const char *json, size_t len, long gameid,
                      const char *out_path)
{
	char tmp[1024];
	jsv root, patch, ach, it = {0}, e, v;
	FILE *out;
	int kept = 0, warned = 0;
	char title[128] = "";
	long console = 0;

	root = js_root(json, len);
	if (!js_member(root, "PatchData", &patch) ||
	    !js_member(patch, "Achievements", &ach))
		return false;
	if (js_member(patch, "ConsoleID", &v)) console = js_int(v);
	if (js_member(patch, "Title", &v)) { js_str(v, title, sizeof title); tsv_safe(title); }

	snprintf(tmp, sizeof tmp, "%s.part", out_path);
	out = fopen(tmp, "w");
	if (!out) return false;

	fprintf(out, "#! tortos-cheevos 1\tgame=%ld\tconsole=%ld\ttitle=%s\n",
	        gameid, console, title);

	while (js_next(ach, &it, &e)) {
		char mem[65536], atitle[128] = "", adesc[256] = "";
		long id = 0, points = 0, flags = 3;

		if (js_member(e, "ID", &v)) id = js_int(v);
		if (js_member(e, "Flags", &v)) flags = js_int(v);
		if (!js_member(e, "MemAddr", &v) || !js_str(v, mem, sizeof mem) || !mem[0])
			continue;
		if (id <= 0) continue;

		/* Flags 5 is "unofficial" - in development, not part of the set
		 * anyone is playing. */
		if (flags != 3) continue;

		if (js_member(e, "Title", &v)) { js_str(v, atitle, sizeof atitle); tsv_safe(atitle); }
		if (js_member(e, "Points", &v)) points = js_int(v);
		if (js_member(e, "Description", &v)) { js_str(v, adesc, sizeof adesc); tsv_safe(adesc); }

		/* RA injects this into every set fetched by a client it does not
		 * recognize; its condition is `1=1.300.`, true after 300 frames. It
		 * is a notice to whoever builds the client, not an achievement, and
		 * writing it would put an entry in every game's list that unlocks
		 * itself five seconds in. Registering is the fix; dropping it is
		 * what keeps the list honest in the meantime. */
		if (!strncmp(atitle, "Warning: Unknown Emulator", 25)) { warned++; continue; }

		fprintf(out, "#:\t%ld\t%ld\t%s\t%s\n", id, points, atitle, adesc);
		fprintf(out, "%ld\t%s\n", id, mem);
		kept++;
	}
	fclose(out);

	if (kept == 0) { unlink(tmp); return false; }
	if (rename(tmp, out_path) != 0) { unlink(tmp); return false; }
	if (warned)
		fprintf(stderr, "ra: RA sent its unregistered-client notice for game %ld; "
		                "dropped\n", gameid);
	fprintf(stderr, "ra: %d achievements cached for game %ld\n", kept, gameid);
	return true;
}

bool ra_fetch_set(long gameid, const char *out_path)
{
	char tmp[1024], g[24];
	char *json;
	size_t len;
	ra_field f[4];
	bool ok;

	if (gameid <= 0 || !ra_signed_in() || !ra_online()) return false;

	snprintf(g, sizeof g, "%ld", gameid);
	snprintf(tmp, sizeof tmp, "%s.json", out_path);

	f[0].k = "r"; f[0].v = "patch";
	f[1].k = "g"; f[1].v = g;
	f[2].k = "u"; f[2].v = g_user;
	f[3].k = "t"; f[3].v = g_token;
	/* Straight to a file: a big set runs past 100KB and nothing here should
	 * have to guess how big is big enough. */
	if (!ra_post_file(f, 4, tmp, 30)) return false;

	json = read_whole(tmp, &len);
	unlink(tmp);
	if (!json) return false;

	ok = ra_set_from_json(json, len, gameid, out_path);
	free(json);
	return ok;
}

bool ra_ensure_set(const char *rom_path, const char *tag, const char *set_path)
{
	struct stat st;
	long gid;

	if (stat(set_path, &st) == 0 && st.st_size > 0) return true;   /* cached */
	if (!ra_signed_in() || !ra_online()) return false;

	gid = ra_game_for_rom(rom_path, tag);
	if (gid <= 0) return false;

	{	/* .cheevos/ beside the ROM, created on first use rather than at scan
		 * time: most systems will never need one. */
		char dir[1024];
		char *slash;

		snprintf(dir, sizeof dir, "%s", set_path);
		slash = strrchr(dir, '/');
		if (slash) { *slash = '\0'; mkdir(dir, 0755); }
	}
	return ra_fetch_set(gid, set_path);
}
