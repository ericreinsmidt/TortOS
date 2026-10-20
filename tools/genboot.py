#!/usr/bin/env python3
"""Render TortOS's boot animation, the flyby.

Run from the repo root:  python3 tools/genboot.py   (or make boot)

Output: res/boot/tortos-fly.mp4, 1024x768, 60 fps, 36 frames (0.60 s), H.264,
and its PNG frames in build/boot-frames.

plastron plays it on both devices as its splash, from the moment the panel has
a framebuffer until TortOS's first frame; its scripts/make-splash-frames.py
turns it into the frames it ships. Those boots are fast, the shelf up about
half a second after the panel lights, so the wordmark is there from frame 0,
large and low on the screen, and is seen however fast the boot is. Above it the
turtle flies in head first from the left edge and shoots across and off the
right with the lockup's three speed bars streaming behind him, fading toward
their tails. Then the wordmark holds alone, and the splash keeps the last frame
up until TortOS draws. At 60 fps, as he moves about 60 px a frame.

The turtle, the wordmark and the bars are the lockup's (tools/genmark.py's
tortos-lockup.svg), from the same constants.

Everything is drawn at 3x and downsampled with LANCZOS: the shell is all
diagonal hexagon edges and PIL's polygon rasterizer has no antialiasing of its
own.
"""

import math
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(ROOT, "res", "boot")
TMP_DIR = os.path.join(ROOT, "build", "boot-frames")
FONT_PATH = os.path.join(ROOT, "res", "fonts", "wordmark.ttf")

W, H = 1024, 768
SS = 3                             # supersample factor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from markdef import BG, LTGRN, CYAN, MIDGRN, DKGREEN, CELLS, HEAD   # noqa: E402

# ---- the lockup, in the same design units the exported SVG uses -------------
FSIZE  = 150.0
K      = FSIZE / 1000.0            # font units -> design units
TUCK   = -24.0                     # the T pulled toward 'ort'
ANG    = math.radians(7.0)         # the font's own italic angle
TH     = 20.0                      # speed bar thickness
TCAP   = 110.0                     # the T's cap height; the bar stack matches it
GAP    = (TCAP - 3 * TH) / 2
PITCH  = TH + GAP
BASELINE = 248.0
T_TOP  = BASELINE - TCAP
# The capital T as an outline, so it can be positioned independently of 'ort'.
T_PTS  = [(109, 728), (596, 728), (581, 614), (396, 614),
          (320, 0), (200, 0), (276, 614), (95, 614)]
T_ADV  = 583.0
INK_R  = 477.0                     # measured ink right edge, with TUCK applied
DESIGN_W = 1200.0

BARS_RAW = [(T_TOP + TH / 2,             400, 600, DKGREEN),
            (T_TOP + TH / 2 + PITCH,     250, 622, CYAN),
            (T_TOP + TH / 2 + PITCH * 2, 355, 600, MIDGRN)]


def bar_pts(x0, x1, yc):
    """A speed bar: ends cut parallel to the T's stem rather than square."""
    s = math.tan(ANG) * TH / 2
    return [(x0 + s, yc - TH / 2), (x1 + s, yc - TH / 2),
            (x1 - s, yc + TH / 2), (x0 - s, yc + TH / 2)]


_raw_left = min(min(p[0] for p in bar_pts(a, b, y)) for (y, a, b, _) in BARS_RAW)
_raw_right = 650.0 + INK_R
DX = DESIGN_W / 2 - (_raw_left + _raw_right) / 2      # center the lockup
LOCK_L, LOCK_R = _raw_left + DX, _raw_right + DX

# ---- fit that lockup into the 4:3 boot frame -------------------------------
TARGET_W = 800.0                                      # of 1024, leaving margins
SCALE = TARGET_W / (LOCK_R - LOCK_L)
OX = (W - TARGET_W) / 2 - LOCK_L * SCALE
OY = H / 2 - ((T_TOP + BASELINE) / 2) * SCALE
R = 40.0                                              # turtle cell radius


def X(v): return (OX + v * SCALE) * SS
def Y(v): return (OY + v * SCALE) * SS


def hexpts(cx, cy, r, sx, sy):
    return [(cx + r * math.cos(math.pi / 6 + i * math.pi / 3) * sx,
             cy + r * math.sin(math.pi / 6 + i * math.pi / 3) * sy) for i in range(6)]


def draw_turtle(d, cx, cy, r, sx, sy, alpha):
    """The shell, from tools/markdef.py - the one place its cells are defined."""
    dx, dy = math.sqrt(3) * r, 1.5 * r
    for (i, j, col) in list(CELLS) + [HEAD]:
        d.polygon(hexpts(cx + i * dx * sx, cy + j * dy * sy, r * 0.95, sx, sy),
                  fill=col + (alpha,), outline=(0, 0, 0, alpha), width=max(1, int(3 * SS * SCALE)))
    d.polygon(hexpts(cx, cy, r * 0.95, sx, sy),
              fill=CYAN + (alpha,), outline=(0, 0, 0, alpha), width=max(1, int(3 * SS * SCALE)))


def cl(t): return max(0.0, min(1.0, t))


# ---- the flyby -------------------------------------------------------------
FLY_FPS = 60
FLY_SX, FLY_SY = 1.30, 0.92   # stretched with speed
F_FLY = (0, 22)          # head in at the left edge -> gone off the right
FLY_FRAMES = 36          # his bars are gone by then; the rest hold the wordmark
FLY_BAR_LEN = 1.7        # his speed bars, this many times the lockup's lengths
FLY_GAP = 40.0           # design units between his back and his bars' heads
FLY_WORD_SCALE = 1.8     # the wordmark, this much larger than in the lockup
FLY_WORD_CY = 0.66       # the middle of its cap height, down the screen
FLY_ABOVE = 90.0         # px between his shell and the top of the wordmark


def fly_turtle_px():
    """His center at the start, in px: the right cell just off the left edge,
    so only the head shows; and at the end, the left cell just off the right."""
    r = R * SCALE
    start = -(math.sqrt(3) * r + r) * FLY_SX - 1
    end = W + (math.sqrt(3) * r + r) * FLY_SX + 1
    return start, end


def fly_word(font_big):
    """The wordmark alone, large and low, centered across: a layer to put
    under every frame."""
    k = SCALE * FLY_WORD_SCALE
    cy = FLY_WORD_CY * H

    def wx(x): return (W / 2.0 + (x - INK_R / 2.0) * k) * SS
    def wy(y): return (cy + (y - (BASELINE - TCAP / 2.0)) * k) * SS

    L = Image.new("RGBA", (W * SS, H * SS), (0, 0, 0, 0)); ld = ImageDraw.Draw(L)
    ld.polygon([(wx(px_ * K), wy(BASELINE - py_ * K)) for px_, py_ in T_PTS],
               fill=LTGRN + (255,))
    ox = T_ADV * K + TUCK
    ld.text((wx(ox), wy(BASELINE)), "ort", font=font_big, fill=LTGRN + (255,), anchor="ls")
    tw = ld.textlength("ort", font=font_big)
    ld.text((wx(ox) + tw, wy(BASELINE)), "OS", font=font_big, fill=CYAN + (255,), anchor="ls")
    return L


def render(n, word):
    im = Image.alpha_composite(Image.new("RGBA", (W * SS, H * SS), BG + (255,)), word)

    # His line, in design units: his shell FLY_ABOVE px above the wordmark
    word_top = FLY_WORD_CY * H - TCAP / 2.0 * SCALE * FLY_WORD_SCALE
    cy = (word_top - FLY_ABOVE - 2.5 * R * SCALE * FLY_SY - OY) / SCALE
    start, end = fly_turtle_px()
    step = (end - start) / (F_FLY[1] - F_FLY[0])
    cx = (start + step * n - OX) / SCALE
    head = cx - (math.sqrt(3) * R + R) * FLY_SX - FLY_GAP     # his bars' heads
    longest = max(b - a for (_, a, b, _) in BARS_RAW) * FLY_BAR_LEN

    # --- his speed bars, fading toward their tails ---------------------------
    # Drawn solid on a layer of their own, their alpha then multiplied by a
    # ramp from head to tail. (Polygons drawn straight onto an RGBA image take
    # their alpha literally rather than blending, so the fade is composited.)
    if X(head - longest) < W * SS:
        L = Image.new("RGBA", im.size, (0, 0, 0, 0))
        for (yc, x0, x1, col) in BARS_RAW:
            length = (x1 - x0) * FLY_BAR_LEN
            y = cy + (yc - (T_TOP + TCAP / 2))
            bar = Image.new("RGBA", im.size, (0, 0, 0, 0))
            ImageDraw.Draw(bar).polygon(
                [(X(px_), Y(py_)) for px_, py_ in bar_pts(head - length, head, y)],
                fill=col + (255,))
            tail_x, head_x = X(head - length), X(head)
            ramp = bytes(int(255 * cl((x - tail_x) / (head_x - tail_x)) ** 1.5)
                         for x in range(im.size[0]))
            fade = Image.frombytes("L", (im.size[0], 1), ramp).resize(im.size, Image.NEAREST)
            bar.putalpha(ImageChops.multiply(bar.getchannel("A"), fade))
            L = Image.alpha_composite(L, bar)
        im = Image.alpha_composite(im, L)

    # --- the turtle ----------------------------------------------------------
    if n < F_FLY[1]:
        d = ImageDraw.Draw(im, "RGBA")
        draw_turtle(d, X(cx), Y(cy), R * SCALE * SS, FLY_SX, FLY_SY, 255)

    return im.convert("RGB").resize((W, H), Image.LANCZOS)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    font = ImageFont.truetype(FONT_PATH, int(FSIZE * SCALE * FLY_WORD_SCALE * SS))
    word = fly_word(font)
    if os.path.isdir(TMP_DIR):
        shutil.rmtree(TMP_DIR)
    os.makedirs(TMP_DIR)
    for i in range(FLY_FRAMES):
        render(i, word).save(os.path.join(TMP_DIR, "f%04d.png" % i))
    mp4 = os.path.join(OUT_DIR, "tortos-fly.mp4")
    subprocess.run([
        find_ffmpeg(), "-y", "-loglevel", "error",
        "-framerate", str(FLY_FPS), "-start_number", "0",
        "-i", os.path.join(TMP_DIR, "f%04d.png"),
        "-frames:v", str(FLY_FRAMES), "-fps_mode", "passthrough",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20",
        "-movflags", "+faststart", mp4,
    ], check=True)
    print("%s  %d frames, %.2f s, %d bytes" % (
        mp4, FLY_FRAMES, FLY_FRAMES / float(FLY_FPS), os.path.getsize(mp4)))


def find_ffmpeg():
    for cand in ("/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"):
        if os.path.exists(cand):
            return cand
    found = shutil.which("ffmpeg")
    if not found:
        sys.exit("ffmpeg not found on PATH")
    return found


if __name__ == "__main__":
    main()
