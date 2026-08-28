#!/usr/bin/env python3
"""Render the PlayOS boot animation and the still images that bracket it.

Run from the repo root:  python3 tools/genboot.py

Outputs (all under res/boot/):
  playos-boot.mp4   1024x768, 30 fps, exactly 72 frames (2.400 s), H.264.
  bootlogo.bmp      frame 0 as a 24-bit BMP, for the bootloader splash slot.
  splash.png        frame 0 as a PNG, for tooling that wants a lossless copy.

Frame 0 is pure black on purpose. The stock splash is replaced by bootlogo.bmp
and the kernel holds that image on screen until the video player takes over, so
starting the video on the same pure black the splash shows means the handoff has
no visible seam. The last frame is the finished lockup on the flat background
color for the mirror-image reason: it stays on the panel until the launcher
draws its first frame, so the video has to end on exactly what the launcher
starts on.

Frames are drawn supersampled and downsampled with LANCZOS, because the play
triangle is mostly diagonal edges and PIL's polygon/line rasteriser has no
antialiasing of its own. Shapes are composited as color-through-an-L-mask
rather than as RGBA layers, which avoids the dark fringes non-premultiplied
RGBA compositing leaves on antialiased edges. The bloom is built from a
separate coverage buffer, blurred at reduced resolution, and added on top of
the frame, so it lightens the field instead of covering it.
"""

import math
import os
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

# --- paths ------------------------------------------------------------------

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = os.path.join(ROOT, "res", "fonts", "menu.ttf")
OUT_DIR = os.path.join(ROOT, "res", "boot")
TMP_DIR = "/tmp/playos-boot-frames"

# --- output format ----------------------------------------------------------

W, H = 1024, 768
FPS = 30
N_FRAMES = 72                       # 72 / 30 == 2.400 s exactly
SS = 4                              # supersampling factor for the frame buffer
WS, HS = W * SS, H * SS

# --- palette ----------------------------------------------------------------

BG_DEEP = (0x07, 0x08, 0x0C)
TEXT = (0xED, 0xED, 0xF2)
CYAN = (0x3D, 0xD6, 0xFF)

# --- lockup geometry (in 1x pixels; scaled by SS when drawn) -----------------

TRI_H = 160.0                       # triangle height
TRI_W = TRI_H * 0.866               # equilateral-ish, pointing right
GAP = 54.0                          # triangle apex to first letter
FONT_PX = 108
TRACKING = 20.0                     # extra advance between letters
LINE_W = 2.0                        # thin sweep lines / triangle outline
STREAK_H = 3.0
CY = 384.0                          # lockup vertical center

WORD = "PLAYOS"
N_WHITE = 4                         # "PLAY" white, "OS" cyan

# --- timeline (seconds) -----------------------------------------------------

BG_FADE = (0.00, 0.45)              # black -> BG_DEEP
# per sweep line: (start, end, length, edge index)
LINE_SPECS = (
    (0.00, 0.40, 560.0, 0),
    (0.06, 0.47, 400.0, 1),
    (0.12, 0.55, 700.0, 2),
)
SNAP_T = (0.55, 0.66)               # outline -> solid triangle
STREAK_T = (0.55, 0.82)             # head travel
STREAK_TAIL_LAG = 0.10
STREAK_FADE = (0.72, 0.96)
LETTER_T0 = 0.75                    # first letter starts
LETTER_STEP = 0.04                  # ~40 ms apart
LETTER_DUR = 0.34
LETTER_SLIDE = 16.0                 # px each letter travels from the right
SETTLE_T = (1.45, 1.90)             # 1.04x -> 1.00x, then a still hold
OVERSIZE = 1.04


# --- easing helpers ---------------------------------------------------------

def clamp01(x):
    return 0.0 if x < 0.0 else (1.0 if x > 1.0 else x)


def smoothstep(edge0, edge1, x):
    t = clamp01((x - edge0) / (edge1 - edge0))
    return t * t * (3.0 - 2.0 * t)


def lerp(a, b, t):
    return a + (b - a) * t


def lerp_pt(a, b, t):
    return (lerp(a[0], b[0], t), lerp(a[1], b[1], t))


def lerp_rgb(a, b, t):
    return tuple(int(round(lerp(a[i], b[i], t))) for i in range(3))


# --- typography -------------------------------------------------------------

font = ImageFont.truetype(FONT_PATH, FONT_PX * SS)
# menu.ttf is Josefin Sans Thin; a sub-pixel stroke keeps the hairline strokes
# from disappearing on the panel without visibly changing the letterforms.
STROKE = max(1, int(round(SS * 0.55)))

_probe = Image.new("L", (8, 8))
_pd = ImageDraw.Draw(_probe)


def letter_layout():
    """Return (x offsets, total width) for WORD at SS scale, with tracking."""
    xs, x = [], 0.0
    for ch in WORD:
        xs.append(x)
        x += font.getlength(ch) + TRACKING * SS
    return xs, x - TRACKING * SS


LX, WORD_W = letter_layout()
_ink = _pd.textbbox((0, 0), WORD, font=font, anchor="ls", stroke_width=STROKE)
BASELINE = CY * SS - (_ink[1] + _ink[3]) / 2.0     # center the ink on CY

TOTAL_W = TRI_W * SS + GAP * SS + WORD_W
X0 = (WS - TOTAL_W) / 2.0

# play triangle vertices, right-pointing
TA = (X0, CY * SS - TRI_H * SS / 2.0)             # top left
TB = (X0, CY * SS + TRI_H * SS / 2.0)             # bottom left
TC = (X0 + TRI_W * SS, CY * SS)                   # apex, right
EDGES = ((TA, TC), (TC, TB), (TB, TA))

WORD_X = X0 + TRI_W * SS + GAP * SS
WORD_END = WORD_X + WORD_W
LOCKUP_C = (WS / 2.0, CY * SS)                    # scale pivot


# --- drawing primitives -----------------------------------------------------

def new_mask():
    return Image.new("L", (WS, HS), 0)


def stamp(base, glow, mask, color, alpha, glow_weight):
    """Composite `color` onto base through `mask`, and add to the glow buffer."""
    if alpha <= 0.0:
        return
    m = mask if alpha >= 1.0 else mask.point(lambda v, a=alpha: int(v * a))
    base.paste(color, (0, 0), m)
    if glow_weight > 0.0:
        w = glow_weight * alpha
        glow.paste(int(round(255 * min(1.0, w))), (0, 0), m)


def thick_line(draw, p0, p1, width):
    """A line with round caps, so butt joints do not notch at the vertices."""
    draw.line([p0, p1], fill=255, width=int(round(width)))
    r = width / 2.0
    for p in (p0, p1):
        draw.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=255)


# --- per-frame composition --------------------------------------------------

def bloom_amount(t):
    a = 0.30 * smoothstep(0.02, 0.18, t)              # the sweep lines glow
    if t >= SNAP_T[0]:
        a += 1.10 * math.exp(-(t - SNAP_T[0]) / 0.26)  # the snap spike, decaying
    return a * (1.0 - smoothstep(SETTLE_T[0], SETTLE_T[1] - 0.02, t))


def draw_frame(i):
    t = i / float(FPS)

    bg = lerp_rgb((0, 0, 0), BG_DEEP, smoothstep(BG_FADE[0], BG_FADE[1], t))
    base = Image.new("RGB", (WS, HS), bg)
    glow = new_mask()

    fill_p = smoothstep(SNAP_T[0], SNAP_T[1], t)
    edge_colour = lerp_rgb(CYAN, TEXT, fill_p)

    # --- beat 1: three lines sweep in and converge into the triangle outline
    line_mask = new_mask()
    ld = ImageDraw.Draw(line_mask)
    drew_line = False
    for t0, t1, length, edge in LINE_SPECS:
        p = smoothstep(t0, t1, t)
        if p <= 0.0:
            continue
        target = EDGES[edge]
        ym = (target[0][1] + target[1][1]) / 2.0
        off = -40.0 * SS
        s0 = (off - length * SS, ym)
        s1 = (off, ym)
        thick_line(ld, lerp_pt(s0, target[0], p), lerp_pt(s1, target[1], p),
                   LINE_W * SS)
        drew_line = True

    # --- beat 2: the triangle snaps solid, and a streak leaves the apex
    if fill_p > 0.0:
        tri_mask = new_mask()
        ImageDraw.Draw(tri_mask).polygon([TA, TB, TC], fill=255)
        stamp(base, glow, tri_mask, TEXT, fill_p, 0.90 * fill_p)

    if drew_line:
        stamp(base, glow, line_mask, edge_colour, 1.0, 0.75)

    su = smoothstep(STREAK_T[0], STREAK_T[1], t)
    sv = smoothstep(STREAK_T[0] + STREAK_TAIL_LAG, STREAK_T[1] + STREAK_TAIL_LAG, t)
    if su > 0.0:
        far = WORD_END + 90.0 * SS
        head = lerp(TC[0], far, su)
        tail = lerp(TC[0], far, sv)
        a = 1.0 - smoothstep(STREAK_FADE[0], STREAK_FADE[1], t)
        if head - tail > 1.0 and a > 0.0:
            r = STREAK_H * SS / 2.0
            sm = new_mask()
            ImageDraw.Draw(sm).rounded_rectangle(
                [tail, TC[1] - r, head, TC[1] + r], radius=r, fill=255)
            stamp(base, glow, sm, TEXT, a, 1.0 * a)

    # --- beat 3: the wordmark resolves letter by letter
    for k, ch in enumerate(WORD):
        p = smoothstep(LETTER_T0 + LETTER_STEP * k,
                       LETTER_T0 + LETTER_STEP * k + LETTER_DUR, t)
        if p <= 0.0:
            continue
        dx = (1.0 - p) * LETTER_SLIDE * SS
        lm = new_mask()
        ImageDraw.Draw(lm).text((WORD_X + LX[k] + dx, BASELINE), ch, font=font,
                                fill=255, anchor="ls", stroke_width=STROKE,
                                stroke_fill=255)
        stamp(base, glow, lm, TEXT if k < N_WHITE else CYAN, p, 0.28 * p)

    # --- beat 4: settle from 4% oversized down to final size, then hold still
    s = lerp(OVERSIZE, 1.0, smoothstep(SETTLE_T[0], SETTLE_T[1], t))
    if abs(s - 1.0) > 1e-6:
        cx, cy = LOCKUP_C
        coeffs = (1.0 / s, 0.0, cx - cx / s, 0.0, 1.0 / s, cy - cy / s)
        base = base.transform((WS, HS), Image.AFFINE, coeffs, Image.BICUBIC)
        glow = glow.transform((WS, HS), Image.AFFINE, coeffs, Image.BICUBIC)

    frame = base.resize((W, H), Image.LANCZOS)

    amount = bloom_amount(t)
    if amount > 0.0:
        g = glow.resize((W, H), Image.LANCZOS)
        tight = (g.resize((W // 2, H // 2), Image.LANCZOS)
                  .filter(ImageFilter.GaussianBlur(6))
                  .resize((W, H), Image.BICUBIC))
        wide = (g.resize((W // 6, H // 6), Image.LANCZOS)
                 .filter(ImageFilter.GaussianBlur(9))
                 .resize((W, H), Image.BICUBIC))
        acc = (0.50 * np.asarray(tight, np.float32)
               + 0.95 * np.asarray(wide, np.float32)) * amount
        rgb = np.stack([acc * (CYAN[0] / 255.0),
                        acc * (CYAN[1] / 255.0),
                        acc * (CYAN[2] / 255.0)], axis=-1)
        np.clip(rgb, 0.0, 255.0, out=rgb)
        frame = ImageChops.add(frame, Image.fromarray(rgb.astype(np.uint8)))

    return frame


# --- driver -----------------------------------------------------------------

def find_ffmpeg():
    for cand in ("/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg"):
        if os.path.isfile(cand):
            return cand
    found = shutil.which("ffmpeg")
    if not found:
        sys.exit("ffmpeg not found on PATH")
    return found


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    if os.path.isdir(TMP_DIR):
        shutil.rmtree(TMP_DIR)
    os.makedirs(TMP_DIR)

    first = None
    for i in range(N_FRAMES):
        frame = draw_frame(i)
        if i == 0:
            first = frame
        frame.save(os.path.join(TMP_DIR, "f%04d.png" % i))
        if (i + 1) % 12 == 0 or i == N_FRAMES - 1:
            print("  frame %2d/%d" % (i + 1, N_FRAMES))

    if first.convert("RGB").getextrema() != ((0, 0), (0, 0), (0, 0)):
        sys.exit("frame 0 is not pure black")

    bmp = os.path.join(OUT_DIR, "bootlogo.bmp")
    png = os.path.join(OUT_DIR, "splash.png")
    first.convert("RGB").save(bmp)                 # 24-bit BMP
    first.convert("RGB").save(png)

    mp4 = os.path.join(OUT_DIR, "playos-boot.mp4")
    cmd = [
        find_ffmpeg(), "-y", "-loglevel", "error",
        "-framerate", str(FPS),
        "-start_number", "0",
        "-i", os.path.join(TMP_DIR, "f%04d.png"),
        "-frames:v", str(N_FRAMES),
        "-fps_mode", "passthrough",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20",
        "-movflags", "+faststart",
        mp4,
    ]
    subprocess.run(cmd, check=True)

    for path in (mp4, bmp, png):
        print("%s  %d bytes" % (path, os.path.getsize(path)))


if __name__ == "__main__":
    main()
