/* SPDX-License-Identifier: 0BSD */
#ifndef PLAYOS_UI_H
#define PLAYOS_UI_H

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdbool.h>

/* The look: near-black, one accent per system, and nothing on screen that is
 * not either a card, the name of what is under the cursor, or a rail saying
 * where you are in the list. */

#define UI_BG_R 7
#define UI_BG_G 8
#define UI_BG_B 12

/* Four steps, brightest first. A list is read at all of them at once, so the
 * gaps have to be wide enough to rank the rows and narrow enough that the
 * quietest step is still text rather than texture. SOFT is where an ordinary
 * unselected row sits: selection is said by the highlight behind it, not by
 * dimming everything else down to it. */
#define UI_TEXT      ((SDL_Color){ 237, 237, 242, 255 })
#define UI_TEXT_SOFT ((SDL_Color){ 198, 201, 214, 255 })
#define UI_TEXT_DIM  ((SDL_Color){ 148, 153, 172, 255 })

/* PlayOS cyan: 0x3DD6FF. The system accents come from systems.cfg. */
#define UI_CYAN_R 61
#define UI_CYAN_G 214
#define UI_CYAN_B 255

/* One thickness for every horizontal indicator: the settings line across the
 * top and the position rail across the bottom are the same bar in two places,
 * so they are the same weight. */
#define UI_BAR_H 6

/* The settings line is tinted by which setting it is. Brightness is the colour
 * of light, volume is the launcher's own cyan -- fixed per function rather
 * than taken from the system accent, which would make one control change
 * colour as you scrolled past it. */
#define UI_OSD_BRIGHT ((SDL_Color){ 255, 206, 128, 255 })
#define UI_OSD_VOLUME ((SDL_Color){  61, 214, 255, 255 })

bool ui_init(SDL_Renderer *r, const char *font_path);
void ui_quit(void);

/* The type scale. One base size, a multiplier per role, and a global user
 * scale over the top -- so retuning a role is one number here instead of a
 * size hunted down at every call site, and the whole scale can move together,
 * which on a panel this dense is usually what is wanted.
 *
 * UI_F_CARD is drawn into the 512px-wide generated card rather than onto the
 * screen, so it is sized for the card and not for the panel. */
typedef enum {
	UI_F_TITLE,   /* the name of the thing under the cursor */
	UI_F_MENU,    /* menu rows */
	UI_F_LABEL,   /* headings, slot names */
	UI_F_META,    /* counts, timestamps -- the quiet line */
	UI_F_CARD,
	UI_F_COUNT
} ui_font_role;

/* Multiply every role. Applied when the fonts are opened, so this has to be
 * set before ui_init -- it persists across the ui_quit/ui_init pair that the
 * standalone-emulator fallback path goes through. */
void ui_set_font_scale(float scale);
float ui_get_font_scale(void);
TTF_Font *ui_font(ui_font_role role);
/* Baseline-to-baseline distance for a role, the unit menu rows are laid out in. */
int ui_font_line(ui_font_role role);

/* Draw text with its top-left at (x,y). anchor: -1 left, 0 centre, 1 right,
 * applied to x. Returns the drawn width. Rendering is cached per (font,
 * string), so redrawing the same title every frame costs one blit. */
int ui_text(SDL_Renderer *r, TTF_Font *f, const char *s, int x, int y,
            int anchor, SDL_Color col);
int ui_text_width(TTF_Font *f, const char *s);

/* An additive radial glow, tinted, centred on rect and spilling past it.
 * This is what tells you which card has focus without drawing a frame
 * around anything. */
void ui_glow(SDL_Renderer *r, const SDL_Rect *rect, unsigned rgb, int alpha,
             float spread);

/* The position rail across the bottom: a faint full-width track with a bright
 * accent segment showing where the cursor sits in a list of `count`. */
void ui_rail(SDL_Renderer *r, int screen_w, int screen_h, int index, int count,
             unsigned rgb);

/* A filled rounded rectangle. SDL has no such primitive; this is the middle as
 * one rect and the two caps as one inset row each, so a panel costs a few
 * dozen fills rather than one per scanline. */
void ui_round_rect(SDL_Renderer *r, const SDL_Rect *q, int radius, SDL_Color col);

/* A menu slab: near-opaque, so a list drawn on it reads against a paused game
 * frame or a lit shelf without either showing through, with a hairline of the
 * system accent around it. `border` is 0xRRGGBB. */
void ui_panel(SDL_Renderer *r, const SDL_Rect *q, int radius, unsigned border);

/* A card for a game with no art: a tinted slab with the title on it. Owned by
 * the caller. */
SDL_Texture *ui_make_card(SDL_Renderer *r, const char *title, unsigned rgb,
                          int *w, int *h);

/* rgb interpolation, for easing the background tint between systems */
unsigned ui_mix(unsigned a, unsigned b, float t);

#endif
