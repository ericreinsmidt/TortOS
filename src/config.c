/* SPDX-License-Identifier: 0BSD */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void strip(char *s)
{
	char *e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
		*--e = '\0';
	char *b = s;
	while (*b == ' ' || *b == '\t') b++;
	if (b != s) memmove(s, b, strlen(b) + 1);
}

/* split a line on '|' into up to n fields, in place */
static int split(char *line, char *fields[], int n)
{
	int c = 0;
	char *p = line;
	while (c < n) {
		fields[c++] = p;
		char *bar = strchr(p, '|');
		if (!bar) break;
		*bar = '\0';
		p = bar + 1;
	}
	for (int i = 0; i < c; i++) strip(fields[i]);
	return c;
}

/* sys|display name|rom folder|core|tag|card|RRGGBB|extensions */
bool cfg_load_systems(const char *path, systems_cfg *out)
{
	memset(out, 0, sizeof *out);
	FILE *f = fopen(path, "r");
	if (!f) return false;
	char line[1024];
	while (fgets(line, sizeof line, f) && out->count < CFG_MAX_SYSTEMS) {
		strip(line);
		if (!line[0] || line[0] == '#') continue;
		char *fld[8] = { 0 };
		int n = split(line, fld, 8);
		if (n < 5 || strcmp(fld[0], "sys") != 0) continue;
		system_cfg *s = &out->systems[out->count];
		snprintf(s->name, CFG_STR, "%s", fld[1]);
		snprintf(s->folder, CFG_STR, "%s", fld[2]);
		snprintf(s->core, CFG_STR, "%s", fld[3]);
		snprintf(s->tag, sizeof s->tag, "%s", fld[4]);
		if (n > 5) snprintf(s->card, CFG_STR, "%s", fld[5]);
		s->accent = 0x3DD6FF;
		if (n > 6 && fld[6][0]) s->accent = (unsigned)strtoul(fld[6], NULL, 16);
		if (n > 7) snprintf(s->exts, CFG_STR, "%s", fld[7]);
		if (!s->name[0] || !s->folder[0] || !s->core[0] || !s->tag[0]) continue;
		out->count++;
	}
	fclose(f);
	return out->count > 0;
}

void cfg_load_tortos(const char *path, tortos_cfg *out)
{
	memset(out, 0, sizeof *out);
	out->volume = -1;
	out->brightness = -1;
	out->font_scale = 1.0f;
	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[1024];
	while (fgets(line, sizeof line, f)) {
		strip(line);
		if (!line[0] || line[0] == '#') continue;
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = '\0';
		char *key = line, *val = eq + 1;
		strip(key); strip(val);
		if (strcmp(key, "volume") == 0) out->volume = atoi(val);
		else if (strcmp(key, "brightness") == 0) out->brightness = atoi(val);
		/* A bad or absent font_scale has to leave the UI readable rather than
		 * unreadable, so anything that does not parse stays at 1.0 and the
		 * clamp is ui_set_font_scale's, in one place. */
		else if (strcmp(key, "font_scale") == 0) {
			float v = (float)atof(val);
			if (v > 0.0f) out->font_scale = v;
		}
		else if (strcmp(key, "startup_system") == 0)
			snprintf(out->startup_system, CFG_STR, "%s", val);
	}
	fclose(f);
}
