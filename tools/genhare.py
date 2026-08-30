"""The Over The Hare mark: the tortoise has already gone past.

Left to right: a hare's head, the speed lines the tortoise left behind, and
the tortoise itself - up and ahead. The joke is the fable's ending rather than
its premise, and it puts the three pieces in reading order.

Lattice and palette are tools/markdef.py's. Cells are (i, j) with
x = i*sqrt(3)*r and y = j*1.5*r; neighbours are (i +/- 1, j) and
(i +/- 0.5, j -/+ 1), so j even wants integer i and j odd wants half-integer.

The speed lines are three horizontal bars in the wordmark's own arrangement -
dark green above, cyan in the middle and longest, mid green below.
"""
import math, sys

LTGRN, MIDGRN, DKGREEN, CYAN = (128,176,118), (104,138,96), (61,89,67), (61,214,255)
# Not a new colour: the one markdef.py retired from the wordmark's "Tort" for
# belonging to no part of the mark. The hare is the part of this mark that is
# not a tortoise, so the colour that belonged to nothing has something to be.
OFFWHT = (233,236,227)

SHELL = [((-0.5,-1),LTGRN), ((0.5,-1),MIDGRN), ((1.0,0.0),LTGRN),
         ((0.5,1),MIDGRN), ((-0.5,1),LTGRN), ((-1.0,0.0),MIDGRN), ((0.0,0.0),CYAN)]

SX, SY = 3.0, -1.0            # where the tortoise is
HX, HY = -2.0, 2.0            # where the hare's head is

HEAD = [(HX-1,HY), (HX,HY), (HX+1,HY),
        (HX-0.5,HY-1), (HX+0.5,HY-1), (HX-0.5,HY+1), (HX+0.5,HY+1)]
# Two ears with a whole empty cell between them at every row. Hexes tile with
# no gaps, so anything adjacent merges into one mass - three earlier attempts
# put the ears next to the head and rendered a blob. The negative space is the
# ear.
EARS = [(HX-1,HY-2), (HX-1.5,HY-3), (HX-1,HY-4),
        (HX+1,HY-2), (HX+1.5,HY-3), (HX+1,HY-4)]

def cells():
    out  = [(c, OFFWHT) for c in HEAD + EARS]
    out += [((SX+i, SY+j), c) for (i, j), c in SHELL]
    return out

def bars(r):
    """(x0, x1, y, colour). In r units, then scaled - so the lines keep their
    proportions to the hexes at any size."""
    dx, dy = math.sqrt(3.0)*r, 1.5*r
    left   = SX*dx - 0.95*r - 0.45*r          # a gap before the shell
    mid    = SY*dy
    return [(-0.2*r, left, mid - 1.15*r, DKGREEN),
            (-1.5*r, left, mid,          CYAN),
            ( 0.3*r, left, mid + 1.15*r, MIDGRN)]

def svg(r=10.0, pad=2.0):
    dx, dy = math.sqrt(3.0)*r, 1.5*r
    cs = cells()
    xs = [i*dx for (i, _), _ in cs] + [b[0] for b in bars(r)] + [b[1] for b in bars(r)]
    ys = [j*dy for (_, j), _ in cs] + [b[2] for b in bars(r)]
    x0, x1 = min(xs)-r-pad, max(xs)+r+pad
    y0, y1 = min(ys)-r-pad, max(ys)+r+pad
    o = ['<svg viewBox="%.1f %.1f %.1f %.1f" xmlns="http://www.w3.org/2000/svg" '
         'role="img" aria-label="A tortoise, ahead of a hare">'
         % (x0, y0, x1-x0, y1-y0)]
    for bx0, bx1, by, col in bars(r):
        o.append('<rect x="%.2f" y="%.2f" width="%.2f" height="%.2f" rx="%.2f" '
                 'fill="#%02x%02x%02x"/>'
                 % (bx0, by - 0.16*r, bx1-bx0, 0.32*r, 0.16*r, *col))
    for (i, j), col in cs:
        cx, cy = i*dx, j*dy
        p = ["%.2f,%.2f" % (cx + r*0.94*math.cos(math.radians(60*k-90)),
                            cy + r*0.94*math.sin(math.radians(60*k-90)))
             for k in range(6)]
        o.append('<polygon points="%s" fill="#%02x%02x%02x"/>' % (" ".join(p), *col))
    o.append('</svg>')
    return "".join(o)

# Written to res/web/mark.svg, which the page loads. Regenerate with:
#
#     python3 tools/genhare.py res/web/mark.svg
#
# Kept as a generator rather than as hand-written SVG for the reason
# markdef.py exists at all: the cells and the palette have one definition, and
# a mark edited by hand drifts from the one the device draws.
if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "res/web/mark.svg"
    open(out, "w").write(svg())
    print("wrote", out)
