#!/usr/bin/env python3
"""Draw res/cards/FAVORITES.png, the card for the Favorites shelf.

The other nine cards are drawn by hand and nothing generates them - gencards.py
was deleted because it could not reproduce any of the nine. This one is
generated because it is not a console: it belongs to TortOS, its accent is
TortOS's cyan rather than a machine's, and that cyan already has exactly one
definition in tools/markdef.py. A hand-drawn copy would be a fourth place for
it to drift.

Matched to the set by measurement rather than by eye: 640x820, card stock
(28,31,42), a 12px accent rule at y=587, the label under it in (239,239,244).
Three marks, ascending, which is the composition Genesis and Game Gear already
use - the set's rhythm is two or three flat shapes, and a single huge heart
would read as a different kind of card.

The mark is markdef.heart(), the same curve src/main.c draws beside a favorited
game's name. They were stars until 2026-08-30; a card and a shelf disagreeing
about what "favorite" looks like is the kind of thing nobody notices and
everybody feels.
"""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from markdef import CYAN, heart  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "res", "cards", "FAVORITES.png")
FONT = os.path.join(ROOT, "res", "fonts", "menu.ttf")

W, H = 640, 820
STOCK = (28, 31, 42)
LABEL = (239, 239, 244)
RULE_Y, RULE_H = 587, 12
RADIUS = 28
SS = 4                      # supersample, so the curve is not ragged


def mark(d, cx, cy, r, col):
    """markdef.heart(), placed. The shelf draws the same curve in C - see
    draw_heart in src/main.c. Stars until 2026-08-30."""
    d.polygon([(cx + x * 2 * r, cy + y * 2 * r) for x, y in heart(96)],
              fill=col)


def main():
    im = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)

    d.rounded_rectangle([0, 0, W * SS - 1, H * SS - 1], radius=RADIUS * SS,
                        fill=STOCK + (255,))
    d.rectangle([0, RULE_Y * SS, W * SS - 1, (RULE_Y + RULE_H) * SS - 1],
                fill=CYAN + (255,))

    # Ascending, left to right, on one baseline - the same read as Genesis's
    # three circles. Centred on the panel above the rule, not on the card.
    cy = 300 * SS
    for cx, r in ((143, 44), (293, 64), (453, 88)):
        mark(d, cx * SS, cy, r * SS, CYAN + (255,))

    im = im.resize((W, H), Image.LANCZOS)
    d = ImageDraw.Draw(im)
    try:
        f = ImageFont.truetype(FONT, 54)
    except OSError:
        f = ImageFont.load_default()
    # Baseline and size measured off the set rather than chosen: the other
    # nine cards put a 42px cap height with its baseline at y=727.
    d.text((W // 2, 727), "FAVORITES", font=f, fill=LABEL + (255,), anchor="ms")

    im.save(OUT)
    print(OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
