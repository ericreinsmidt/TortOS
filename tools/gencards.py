#!/usr/bin/env python3
"""Render the three system cards used on the PlayOS launcher carousel.

Run from the repo root:  python3 tools/gencards.py

Outputs (all under res/cards/), 640x820 RGBA PNG each:
  NES.png   accent #C4443A, label NES
  PCE.png   accent #E8641E, label TURBOGRAFX-16
  GBA.png   accent #6B5BD6, label GAME BOY ADVANCE

One design, three accents. Each card is a rounded slab with a vertical
gradient, a cyan hairline picking out the top edge, an abstract geometric mark
in the accent colour, an accent band, and the system label under it. The marks
are invented shapes that gesture at the shape language of each machine; none of
them reproduce a logo or any other trademark.

The launcher uploads these as textured quads and draws them rotated, so the
corners have to carry clean antialiased alpha rather than relying on the
background behind them. Everything is drawn at 4x and downsampled with LANCZOS
to get that. The card is painted as fully opaque RGB first and the rounded mask
is applied as alpha only at the end, so the pixels underneath the transparent
corners still hold the gradient colour and the downsample cannot pull a dark
fringe in from unpainted areas.

The label font size is derived from the longest of the three labels, so all
three cards share one size.
"""

import os

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# --- paths ------------------------------------------------------------------

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = os.path.join(ROOT, "res", "fonts", "menu.ttf")
OUT_DIR = os.path.join(ROOT, "res", "cards")

# --- output format ----------------------------------------------------------

W, H = 640, 820
SS = 4                              # supersample factor
WS, HS = W * SS, H * SS

# --- palette ----------------------------------------------------------------

SLAB_TOP = (0x23, 0x27, 0x34)
SLAB_BOTTOM = (0x19, 0x1C, 0x26)
CYAN = (0x3D, 0xD6, 0xFF)
TEXT = (0xED, 0xED, 0xF2)

ACCENTS = {
    "NES": (0xC4, 0x44, 0x3A),
    "PCE": (0xE8, 0x64, 0x1E),
    "GBA": (0x6B, 0x5B, 0xD6),
}
LABELS = {
    "NES": "NES",
    "PCE": "TURBOGRAFX-16",
    "GBA": "GAME BOY ADVANCE",
}

# --- layout (1x pixels) -----------------------------------------------------

RADIUS = 28
HAIR_INSET = 3
HAIR_ALPHA = 0.35
DIM = 0.45                          # secondary geometry inside a mark

MARK_CX, MARK_CY = W / 2.0, 280.0   # centred in the upper two thirds
BAND_Y, BAND_H = 588, 10
LABEL_CY = 686
LABEL_MAX_W = 520                   # leaves 60px of margin either side
TRACKING_EM = 0.16


def px(v):
    return v * SS


# --- typography -------------------------------------------------------------

_probe = ImageDraw.Draw(Image.new("L", (8, 8)))
# menu.ttf is Josefin Sans Thin; a sub-pixel stroke keeps it legible at the size
# the longest label forces, without changing the letterforms.
STROKE = max(1, int(round(SS * 0.5)))


def measure(font, text, tracking):
    w = sum(font.getlength(c) for c in text) + tracking * (len(text) - 1)
    return w + 2 * STROKE


def fit_label_size():
    """Largest size at which the longest label still fits LABEL_MAX_W."""
    longest = max(LABELS.values(), key=len)
    for size in range(72, 7, -1):
        font = ImageFont.truetype(FONT_PATH, px(size))
        if measure(font, longest, TRACKING_EM * px(size)) <= px(LABEL_MAX_W):
            return size
    raise RuntimeError("no label size fits")


LABEL_SIZE = fit_label_size()
LABEL_FONT = ImageFont.truetype(FONT_PATH, px(LABEL_SIZE))
LABEL_TRACKING = TRACKING_EM * px(LABEL_SIZE)


# --- drawing helpers --------------------------------------------------------

def blank_mask():
    return Image.new("L", (WS, HS), 0)


def paint(card, mask, colour, alpha=1.0):
    """Composite a flat colour onto the card through an antialiased mask."""
    if alpha < 1.0:
        mask = mask.point(lambda v, a=alpha: int(v * a))
    card.paste(colour, (0, 0), mask)


def gradient_slab():
    t = np.linspace(0.0, 1.0, HS, dtype=np.float32)[:, None]
    top = np.array(SLAB_TOP, np.float32)
    bot = np.array(SLAB_BOTTOM, np.float32)
    rows = top[None, :] * (1.0 - t) + bot[None, :] * t
    arr = np.repeat(rows[:, None, :], WS, axis=1)
    return Image.fromarray(np.round(arr).astype(np.uint8), "RGB")


def rounded_mask():
    m = blank_mask()
    ImageDraw.Draw(m).rounded_rectangle(
        [0, 0, WS - 1, HS - 1], radius=px(RADIUS), fill=255)
    return m


def hairline_mask():
    """Top-edge hairline that fades out as it turns down the sides."""
    m = blank_mask()
    inset = px(HAIR_INSET)
    ImageDraw.Draw(m).rounded_rectangle(
        [inset, inset, WS - 1 - inset, HS - 1 - inset],
        radius=px(RADIUS - HAIR_INSET), outline=255, width=SS)
    arr = np.asarray(m, np.float32)
    y = np.arange(HS, dtype=np.float32)[:, None]
    fade = np.clip(1.0 - (y - px(RADIUS) * 0.7) / (px(RADIUS) * 2.0), 0.0, 1.0)
    return Image.fromarray(np.round(arr * fade).astype(np.uint8), "L")


# --- the three marks --------------------------------------------------------
# Abstract geometry only. Nothing here traces a logo or a product's trade dress.

def mark_nes(card, grad, accent):
    """A stack of horizontal bars with one solid accent bar: front-loader lines."""
    n, bw, bh, gap = 5, 330.0, 36.0, 30.0
    total = n * bh + (n - 1) * gap
    x0, x1 = MARK_CX - bw / 2.0, MARK_CX + bw / 2.0
    top = MARK_CY - total / 2.0
    for i in range(n):
        y = top + i * (bh + gap)
        m = blank_mask()
        ImageDraw.Draw(m).rectangle(
            [px(x0), px(y), px(x1) - 1, px(y + bh) - 1], fill=255)
        paint(card, m, accent, 1.0 if i == 3 else DIM)


def mark_pce(card, grad, accent):
    """A small square offset inside a larger square outline: a card in a slot."""
    outer, stroke = 300.0, 18.0
    inner, off = 104.0, 48.0
    m = blank_mask()
    d = ImageDraw.Draw(m)
    d.rectangle([px(MARK_CX - outer / 2.0), px(MARK_CY - outer / 2.0),
                 px(MARK_CX + outer / 2.0) - 1, px(MARK_CY + outer / 2.0) - 1],
                outline=255, width=int(px(stroke)))
    cx, cy = MARK_CX + off, MARK_CY + off
    d.rectangle([px(cx - inner / 2.0), px(cy - inner / 2.0),
                 px(cx + inner / 2.0) - 1, px(cy + inner / 2.0) - 1], fill=255)
    paint(card, m, accent)


def mark_gba(card, grad, accent):
    """A wide capsule with two circles cut out of one end: a handheld in the hand."""
    cw, ch = 366.0, 180.0
    m = blank_mask()
    ImageDraw.Draw(m).rounded_rectangle(
        [px(MARK_CX - cw / 2.0), px(MARK_CY - ch / 2.0),
         px(MARK_CX + cw / 2.0) - 1, px(MARK_CY + ch / 2.0) - 1],
        radius=px(ch / 2.0), fill=255)
    paint(card, m, accent)

    holes = blank_mask()
    hd = ImageDraw.Draw(holes)
    r = 26.0
    right = MARK_CX + cw / 2.0
    for hx, hy in ((right - 110.0, MARK_CY + 31.0), (right - 49.0, MARK_CY - 31.0)):
        hd.ellipse([px(hx - r), px(hy - r), px(hx + r), px(hy + r)], fill=255)
    card.paste(grad, (0, 0), holes)     # cut back to the slab gradient


MARKS = {"NES": mark_nes, "PCE": mark_pce, "GBA": mark_gba}


# --- card assembly ----------------------------------------------------------

def draw_label(card, text):
    tracking = LABEL_TRACKING
    total = measure(LABEL_FONT, text, tracking)
    x = (WS - total) / 2.0 + STROKE
    ink = _probe.textbbox((0, 0), text, font=LABEL_FONT, anchor="ls",
                          stroke_width=STROKE)
    baseline = px(LABEL_CY) - (ink[1] + ink[3]) / 2.0
    d = ImageDraw.Draw(card)
    for ch in text:
        d.text((x, baseline), ch, font=LABEL_FONT, fill=TEXT, anchor="ls",
               stroke_width=STROKE, stroke_fill=TEXT)
        x += LABEL_FONT.getlength(ch) + tracking


def render_card(key):
    accent = ACCENTS[key]
    grad = gradient_slab()
    card = grad.copy()

    MARKS[key](card, grad, accent)

    band = blank_mask()
    ImageDraw.Draw(band).rectangle(
        [0, px(BAND_Y), WS - 1, px(BAND_Y + BAND_H) - 1], fill=255)
    paint(card, band, accent)

    paint(card, hairline_mask(), CYAN, HAIR_ALPHA)
    draw_label(card, LABELS[key])

    out = card.convert("RGBA")
    out.putalpha(rounded_mask())
    return out.resize((W, H), Image.LANCZOS)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print("label size: %dpx (fitted to %r)" %
          (LABEL_SIZE, max(LABELS.values(), key=len)))
    for key in ("NES", "PCE", "GBA"):
        img = render_card(key)
        if img.getpixel((2, 2))[3] != 0:
            raise RuntimeError("%s: corner is not transparent" % key)
        path = os.path.join(OUT_DIR, "%s.png" % key)
        img.save(path)
        print("%s  %dx%d  %d bytes" % (path, img.width, img.height,
                                       os.path.getsize(path)))


if __name__ == "__main__":
    main()
