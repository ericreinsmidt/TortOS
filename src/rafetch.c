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

		/* Two notices RetroAchievements sends as if they were achievements,
		 * both with the condition `1=1.300.` - true after 300 frames.
		 * Neither is something a player earned, and writing either would put
		 * an entry in a game's list that unlocks itself five seconds in.
		 *
		 *   Warning: Unknown Emulator   this client is not registered with
		 *                               RetroAchievements. Registering is
		 *                               the fix; dropping it keeps the list
		 *                               honest meanwhile.
		 *   Unsupported Game Version    this ROM is a dump RA has not
		 *                               verified. It comes back as the ONLY
		 *                               achievement, under a synthetic game
		 *                               id, so the whole set is a placeholder
		 *                               and `kept` ends at zero - which is
		 *                               how the caller learns there is
		 *                               nothing here.
		 */
		if (!strncmp(atitle, "Warning: Unknown Emulator", 25) ||
		    !strncmp(atitle, "Unsupported Game Version", 24)) { warned++; continue; }

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

/* ---- the account -------------------------------------------------------- */

int ra_account_unlocks(long gameid, int *out, int max)
{
	char *body;
	char g[24];
	ra_field f[5];
	jsv root, arr, it, e;
	int n = 0;

	if (gameid <= 0 || !out || max <= 0) return -1;
	if (!ra_signed_in() || !ra_online()) return -1;

	/* Heap rather than a guess on the stack: 200 ids is a few KB and the
	 * reply carries more than the ids. */
	body = malloc(65536);
	if (!body) return -1;

	snprintf(g, sizeof g, "%ld", gameid);
	f[0].k = "r"; f[0].v = "unlocks";
	f[1].k = "u"; f[1].v = g_user;
	f[2].k = "t"; f[2].v = g_token;
	f[3].k = "g"; f[3].v = g;
	f[4].k = "h"; f[4].v = "0";     /* softcore: TortOS enforces no hardcore */
	if (ra_post_buf(f, 5, body, 65536, 20) < 0) { free(body); return -1; }

	root = js_root(body, strlen(body));
	if (!js_member(root, "UserUnlocks", &arr)) { free(body); return -1; }

	memset(&it, 0, sizeof it);
	while (n < max && js_next(arr, &it, &e)) out[n++] = (int)js_int(e);
	free(body);
	return n;
}

/* ---- finding a set while the game is already running -------------------- */

enum { FQ_IDLE, FQ_GAMEID, FQ_PATCH, FQ_DONE, FQ_FAILED };

static int  g_fq_state = FQ_IDLE;
static long g_fq_game;
static char g_fq_hash[33];
static char g_fq_set[1024];
static char g_fq_tmp[256];

long ra_fetch_gameid(void) { return g_fq_game; }

static void fq_stop(int state)
{
	if (g_fq_tmp[0]) { unlink(g_fq_tmp); g_fq_tmp[0] = '\0'; }
	g_fq_state = state;
}

void ra_fetch_begin(const char *rom_hash, const char *set_path)
{
	ra_field f[2];

	g_fq_state = FQ_IDLE;
	g_fq_game = 0;
	if (!rom_hash || !*rom_hash || !set_path || !*set_path) return;
	if (!ra_signed_in() || !ra_online()) return;

	snprintf(g_fq_hash, sizeof g_fq_hash, "%s", rom_hash);
	snprintf(g_fq_set, sizeof g_fq_set, "%s", set_path);
	snprintf(g_fq_tmp, sizeof g_fq_tmp, "/tmp/tortos-ra-fetch-%ld.json",
	         (long)getpid());

	f[0].k = "r"; f[0].v = "gameid";
	f[1].k = "m"; f[1].v = g_fq_hash;
	if (ra_post_async(f, 2, g_fq_tmp, 25)) g_fq_state = FQ_GAMEID;
	else                                   fq_stop(FQ_FAILED);
}

int ra_fetch_step(void)
{
	char *body;
	size_t len;
	jsv root, v;
	int rc;

	if (g_fq_state == FQ_DONE)   { g_fq_state = FQ_IDLE; return 1; }
	if (g_fq_state == FQ_IDLE || g_fq_state == FQ_FAILED) return -1;

	rc = ra_async_poll();
	if (rc == 0) return 0;                         /* still running */
	if (rc < 0) { fq_stop(FQ_FAILED); return -1; }

	body = read_whole(g_fq_tmp, &len);
	if (!body) { fq_stop(FQ_FAILED); return -1; }
	root = js_root(body, len);

	if (g_fq_state == FQ_GAMEID) {
		ra_field f[4];
		static char g[24];       /* static: ra_post_async keeps the pointer */

		if (!js_member(root, "GameID", &v)) { free(body); fq_stop(FQ_FAILED); return -1; }
		g_fq_game = js_int(v);
		free(body);
		/* 0 is RetroAchievements saying it has never seen this ROM, which for
		 * a fan translation is the permanent and correct answer. */
		if (g_fq_game <= 0) { fq_stop(FQ_FAILED); return -1; }

		snprintf(g, sizeof g, "%ld", g_fq_game);
		f[0].k = "r"; f[0].v = "patch";
		f[1].k = "g"; f[1].v = g;
		f[2].k = "u"; f[2].v = g_user;
		f[3].k = "t"; f[3].v = g_token;
		if (!ra_post_async(f, 4, g_fq_tmp, 30)) { fq_stop(FQ_FAILED); return -1; }
		g_fq_state = FQ_PATCH;
		return 0;
	}

	/* FQ_PATCH */
	{
		char dir[1024];
		char *slash;
		bool ok;

		snprintf(dir, sizeof dir, "%s", g_fq_set);
		slash = strrchr(dir, '/');
		if (slash) { *slash = '\0'; mkdir(dir, 0755); }

		ok = ra_set_from_json(body, len, g_fq_game, g_fq_set);
		free(body);
		fq_stop(ok ? FQ_DONE : FQ_FAILED);
		return ok ? 1 : -1;
	}
}

static char g_sync_path[256];

void ra_sync_begin(long gameid)
{
	static char g[24];              /* static: the fields outlive this call */
	ra_field f[5];

	g_sync_path[0] = '\0';
	if (gameid <= 0 || !ra_signed_in() || !ra_online()) return;

	snprintf(g, sizeof g, "%ld", gameid);
	snprintf(g_sync_path, sizeof g_sync_path, "/tmp/tortos-ra-unlocks-%ld.json",
	         (long)getpid());

	f[0].k = "r"; f[0].v = "unlocks";
	f[1].k = "u"; f[1].v = g_user;
	f[2].k = "t"; f[2].v = g_token;
	f[3].k = "g"; f[3].v = g;
	f[4].k = "h"; f[4].v = "0";
	if (!ra_post_async(f, 5, g_sync_path, 25)) g_sync_path[0] = '\0';
}

int ra_sync_collect(int *out, int max)
{
	char *body;
	size_t len;
	jsv root, arr, it, e;
	int n = 0, rc;

	if (!g_sync_path[0] || !out || max <= 0) return -1;

	/* Blocks only if the child somehow has not finished a request started
	 * before a whole game session. It has. */
	do { rc = ra_async_poll(); } while (rc == 0 && (usleep(50000), 1));
	if (rc != 1) { unlink(g_sync_path); g_sync_path[0] = '\0'; return -1; }

	body = read_whole(g_sync_path, &len);
	unlink(g_sync_path);
	g_sync_path[0] = '\0';
	if (!body) return -1;

	root = js_root(body, len);
	if (!js_member(root, "UserUnlocks", &arr)) { free(body); return -1; }
	memset(&it, 0, sizeof it);
	while (n < max && js_next(arr, &it, &e)) out[n++] = (int)js_int(e);
	free(body);
	return n;
}

void ra_start_session(long gameid)
{
	char body[1024], g[24];
	ra_field f[4];

	if (gameid <= 0 || !ra_signed_in() || !ra_online()) return;
	snprintf(g, sizeof g, "%ld", gameid);
	f[0].k = "r"; f[0].v = "startsession";
	f[1].k = "u"; f[1].v = g_user;
	f[2].k = "t"; f[2].v = g_token;
	f[3].k = "g"; f[3].v = g;
	ra_post_buf(f, 4, body, sizeof body, 15);
}

int ra_submit_unlock(int achievement_id, const char *rom_hash)
{
	char body[1024], a[24], v[33], sig[128];
	ra_field f[7];
	jsv root, m;
	int n = 0;

	if (achievement_id <= 0 || !ra_signed_in() || !ra_online()) return -1;

	snprintf(a, sizeof a, "%d", achievement_id);
	/* rcheevos builds exactly this in rc_api_init_award_achievement_request,
	 * and the server checks it. Written from that source rather than guessed:
	 * a wrong signature is refused, and a refusal reads like a permissions
	 * problem. */
	snprintf(sig, sizeof sig, "%d%s0", achievement_id, g_user);
	ra_md5_hex(sig, strlen(sig), v);

	f[n].k = "r"; f[n++].v = "awardachievement";
	f[n].k = "u"; f[n++].v = g_user;
	f[n].k = "t"; f[n++].v = g_token;
	f[n].k = "a"; f[n++].v = a;
	f[n].k = "h"; f[n++].v = "0";
	if (rom_hash && *rom_hash) { f[n].k = "m"; f[n++].v = rom_hash; }
	f[n].k = "v"; f[n++].v = v;

	if (ra_post_buf(f, n, body, sizeof body, 20) < 0) return -1;

	root = js_root(body, strlen(body));
	if (js_member(root, "Success", &m) && js_is_true(m)) return 1;

	{
		char msg[192] = "";

		if (js_member(root, "Error", &m)) js_str(m, msg, sizeof msg);
		fprintf(stderr, "ra: %d refused: %s\n", achievement_id,
		        msg[0] ? msg : "no reason given");

		/* "already unlocked" is the account agreeing with us. Treating it as a
		 * failure left the row pending forever AND stopped the queue behind
		 * it, because the flush breaks on the first thing that will not send -
		 * so one settled achievement blocked every later one. Seen on the
		 * device with an unlock submitted by hand, which the store never
		 * learned about. */
		if (strstr(msg, "already has this achievement")) return 0;
	}
	return -1;
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
