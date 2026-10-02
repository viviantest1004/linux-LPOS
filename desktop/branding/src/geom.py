"""geom.py - the shapes the whole identity is drawn with, and nothing else.

The mark, the wordmark and the boot splash are built from exactly two
primitives: a straight stroke with round ends (Seg) and a circular stroke
covering whole quarters of a circle (Arc). That restriction is the design
and the engineering at once.

As design, it is what makes the letters one family - every stem, bowl,
arch and terminal has the same weight and the same round end, so the
mark and the wordmark read as the same hand at 16px and at 4K.

As engineering, it is what lets the boot splash draw the logo itself.
splash runs before anything else is up, on a libc with no floating point
and no image decoder, on screens from 640x480 to 3840x2160. A bitmap of
the logo would be blurry at one end of that range or huge at the other;
a font rasteriser would be a project. The distance from a point to a
round-ended segment, or to a quarter-circle band, is a few integer
multiplies and one square root - so the splash evaluates the same shapes
this file writes into the SVGs, at whatever size the screen asks for, and
the two cannot drift apart because userland/splash/logo.h is generated
from here (see mark.py).

Arcs are limited to whole quadrants for the same reason: whether a point
lies in the swept part of a quarter circle is two sign tests, where an
arbitrary angle would need trigonometry the splash does not have.

Everything is emitted as filled outlines, never as SVG strokes, because
GTK recolours symbolic icons by forcing `fill` on rect, circle and path
elements - a stroked path would come out as a filled blob in the top bar.
"""

import math

# Quadrants in screen coordinates (y grows downwards), named by where they
# sit on the screen. Angles are the visual ones: 0 is east, 90 is north.
TR, TL, BL, BR = 0, 1, 2, 3
QUAD_START = {TR: 0, TL: 90, BL: 180, BR: 270}


def fmt(v):
    s = f"{v:.3f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


class Seg:
    """A straight stroke of width w from (x0,y0) to (x1,y1), round ends."""

    def __init__(self, x0, y0, x1, y1, w):
        self.x0, self.y0, self.x1, self.y1, self.w = x0, y0, x1, y1, w

    def moved(self, dx, dy, s=1.0):
        return Seg((self.x0 + dx) * s, (self.y0 + dy) * s,
                   (self.x1 + dx) * s, (self.y1 + dy) * s, self.w * s)

    def bbox(self):
        h = self.w / 2
        return (min(self.x0, self.x1) - h, min(self.y0, self.y1) - h,
                max(self.x0, self.x1) + h, max(self.y0, self.y1) + h)

    def path(self):
        h = self.w / 2
        dx, dy = self.x1 - self.x0, self.y1 - self.y0
        n = math.hypot(dx, dy)
        if n < 1e-9:                       # a dot
            return circle_path(self.x0, self.y0, h)
        # unit normal
        nx, ny = -dy / n * h, dx / n * h
        a = (self.x0 + nx, self.y0 + ny)
        b = (self.x1 + nx, self.y1 + ny)
        c = (self.x1 - nx, self.y1 - ny)
        d = (self.x0 - nx, self.y0 - ny)
        r = fmt(h)
        return (f"M{fmt(a[0])} {fmt(a[1])}L{fmt(b[0])} {fmt(b[1])}"
                f"A{r} {r} 0 0 0 {fmt(c[0])} {fmt(c[1])}"
                f"L{fmt(d[0])} {fmt(d[1])}"
                f"A{r} {r} 0 0 0 {fmt(a[0])} {fmt(a[1])}Z")

    def sdf(self, px, py):
        """Signed distance (numpy-friendly) - the reference the splash's
        integer version is checked against."""
        import numpy as np
        dx, dy = self.x1 - self.x0, self.y1 - self.y0
        L2 = dx * dx + dy * dy
        if L2 == 0:
            t = 0
        else:
            t = np.clip(((px - self.x0) * dx + (py - self.y0) * dy) / L2, 0, 1)
        qx, qy = self.x0 + t * dx - px, self.y0 + t * dy - py
        return np.sqrt(qx * qx + qy * qy) - self.w / 2


class Arc:
    """A circular stroke of centreline radius r and width w, covering the
    listed quadrants (a contiguous run, or all four for a ring). Open arcs
    end in round caps, like Seg."""

    def __init__(self, cx, cy, r, w, quads=(TR, TL, BL, BR)):
        self.cx, self.cy, self.r, self.w = cx, cy, r, w
        self.quads = tuple(sorted(set(quads)))

    def moved(self, dx, dy, s=1.0):
        return Arc((self.cx + dx) * s, (self.cy + dy) * s, self.r * s,
                   self.w * s, self.quads)

    def full(self):
        return len(self.quads) == 4

    def span(self):
        """(start angle, sweep) in degrees, counter-clockwise on screen."""
        if self.full():
            return 0, 360
        q = set(self.quads)
        # the start is the quadrant whose clockwise neighbour is not in the set
        for s in q:
            if (s - 1) % 4 not in q:
                return QUAD_START[s], 90 * len(q)
        raise ValueError("arc quadrants must be contiguous")

    def bbox(self):
        R = self.r + self.w / 2
        return (self.cx - R, self.cy - R, self.cx + R, self.cy + R)

    def point(self, deg, rad):
        a = math.radians(deg)
        return self.cx + rad * math.cos(a), self.cy - rad * math.sin(a)

    def path(self):
        h = self.w / 2
        Ro, Ri = self.r + h, self.r - h
        if self.full():
            # outer clockwise, inner counter-clockwise: a hole under nonzero
            return circle_path(self.cx, self.cy, Ro) + circle_path(self.cx, self.cy, Ri, ccw=True)
        a0, sweep = self.span()
        a1 = a0 + sweep
        large = 1 if sweep > 180 else 0
        o0, o1 = self.point(a0, Ro), self.point(a1, Ro)
        i0, i1 = self.point(a0, Ri), self.point(a1, Ri)
        f = fmt
        # visual counter-clockwise on screen is SVG sweep-flag 0
        return (f"M{f(o0[0])} {f(o0[1])}"
                f"A{f(Ro)} {f(Ro)} 0 {large} 0 {f(o1[0])} {f(o1[1])}"
                f"A{f(h)} {f(h)} 0 0 0 {f(i1[0])} {f(i1[1])}"
                f"A{f(Ri)} {f(Ri)} 0 {large} 1 {f(i0[0])} {f(i0[1])}"
                f"A{f(h)} {f(h)} 0 0 0 {f(o0[0])} {f(o0[1])}Z")

    def sdf(self, px, py):
        import numpy as np
        dx, dy = px - self.cx, py - self.cy
        ring = np.abs(np.sqrt(dx * dx + dy * dy) - self.r) - self.w / 2
        if self.full():
            return ring
        # quadrant of each point, the same two sign tests splash.c makes
        q = np.where(dy <= 0, np.where(dx >= 0, TR, TL), np.where(dx <= 0, BL, BR))
        inside = np.isin(q, self.quads)
        a0, sweep = self.span()
        e0, e1 = self.point(a0, self.r), self.point(a0 + sweep, self.r)
        d0 = np.hypot(px - e0[0], py - e0[1])
        d1 = np.hypot(px - e1[0], py - e1[1])
        cap = np.minimum(d0, d1) - self.w / 2
        return np.where(inside, ring, cap)


def circle_path(cx, cy, r, ccw=False):
    f = fmt
    sw = 0 if ccw else 1
    return (f"M{f(cx - r)} {f(cy)}A{f(r)} {f(r)} 0 1 {sw} {f(cx + r)} {f(cy)}"
            f"A{f(r)} {f(r)} 0 1 {sw} {f(cx - r)} {f(cy)}Z")


def rrect_path(x, y, w, h, r):
    f = fmt
    return (f"M{f(x + r)} {f(y)}H{f(x + w - r)}A{f(r)} {f(r)} 0 0 1 {f(x + w)} {f(y + r)}"
            f"V{f(y + h - r)}A{f(r)} {f(r)} 0 0 1 {f(x + w - r)} {f(y + h)}"
            f"H{f(x + r)}A{f(r)} {f(r)} 0 0 1 {f(x)} {f(y + h - r)}"
            f"V{f(y + r)}A{f(r)} {f(r)} 0 0 1 {f(x + r)} {f(y)}Z")


def bbox(prims):
    bs = [p.bbox() for p in prims]
    return (min(b[0] for b in bs), min(b[1] for b in bs),
            max(b[2] for b in bs), max(b[3] for b in bs))


def paths(prims):
    return "".join(p.path() for p in prims)
