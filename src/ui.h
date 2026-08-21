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

#define UI_TEXT      ((SDL_Color){ 237, 237, 242, 255 })
#define UI_TEXT_DIM  ((SDL_Color){ 138, 143, 163, 255 })

/* PlayOS cyan: 0x3DD6FF. The system accents come from systems.cfg. */
#define UI_CYAN_R 61
#define UI_CYAN_G 214
#define UI_CYAN_B 255

bool ui_init(SDL_Renderer *r, const char *font_path);
void ui_quit(void);

TTF_Font *ui_font_big(void);
TTF_Font *ui_font_small(void);

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

/* A card for a game with no art: a tinted slab with the title on it. Owned by
 * the caller. */
SDL_Texture *ui_make_card(SDL_Renderer *r, const char *title, unsigned rgb,
                          int *w, int *h);

/* rgb interpolation, for easing the background tint between systems */
unsigned ui_mix(unsigned a, unsigned b, float t);

#endif
