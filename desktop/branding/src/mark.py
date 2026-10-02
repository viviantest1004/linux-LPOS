"""mark.py - the LP mark, the wordmark, and the colours they are shown in.

The mark is an L whose stem carries a ring. Read whole, the ring hanging
off the stem is the bowl of a P, so the shape says LP; read apart, it is
an L and a zero - LP-zero, the Raspberry Pi board the system was first
written for. The two readings are why the colour version splits the
shape where it does: the L is the ink colour and the ring is the one
accent the identity has.

It is drawn on a 16-unit grid because 16px is the hardest size it has to
survive (the top bar), and on that grid every edge of the ring, stem and
foot lands on a whole pixel. 32, 48 and 64 are multiples and stay sharp
for free; 24 is not, so it gets its own placement below rather than a
blurred scale of the 16.

This file is the single source for:
  desktop/branding/logo/ (SVG)       the mark, wordmark and lockups
  userland/splash/logo.h             the same shapes as integer tables
and is imported by icons.py (the logo icon) and wallpaper.py (colours).
"""

import os
from geom import Seg, Arc, TR, TL, BL, BR, bbox, paths, fmt, rrect_path

# ── colours ──────────────────────────────────────────────────────────────
#
# Deep blue, teal and orange. The first desktop was aubergine with an
# orange accent - which is Ubuntu's palette, and the owner said it looked
# like Ubuntu. LP's is a deep ocean blue with teal where the light falls,
# and one warm orange for "on" and for the mark's ring. The orange is a
# yellower one than Ubuntu's #e95420, so the two are not confused.
# It is not the design system's amber (#f0b350), which means "a limit is
# in force" and must stay free for that.
ORANGE = "#f28c28"        # the ring; "on"
ORANGE_ON_LIGHT = "#d9731a"  # same hue, darker, keeps 3:1 on white
INK_DARK = "#0d2740"      # the L on light backgrounds: deep blue 800
WHITE = "#ffffff"
MUTED_ON_DARK = "#dbe8f0"  # wordmark "linux" on dark: a cool off-white

# The deep-blue ramp, light to deep. The wallpaper and the splash use it.
# (The name is the old one: every generator reads this table by it.)
AUBERGINE = {
    "300": "#6cc9d6",
    "400": "#2f9fb8",
    "500": "#1b6f94",
    "600": "#165a7c",
    "700": "#123f5e",
    "800": "#0d2740",
    "900": "#08172a",
}

# ── the mark ─────────────────────────────────────────────────────────────
W = 2.0  # stroke weight on the 16 grid: two pixels at 16px


def mark16():
    """(ring, ell) on the 16-unit grid. The ring's centreline passes
    through the stem's centreline, so the stem covers the ring's west
    side and the ring reads as a bowl attached to it rather than an O
    parked beside an L."""
    ring = [Arc(7, 5, 3, W)]
    ell = [Seg(4, 2, 4, 14, W), Seg(4, 14, 12, 14, W)]
    return ring, ell


def mark24():
    """The same mark placed for a 24px grid: stroke 3, every edge on a
    whole pixel. A plain 1.5x scale of the 16 lands the edges on halves."""
    ring = [Arc(11, 8, 4.5, 3)]
    ell = [Seg(6.5, 3.5, 6.5, 21.5, 3), Seg(6.5, 21.5, 18.5, 21.5, 3)]
    return ring, ell


# ── the wordmark ─────────────────────────────────────────────────────────
#
# A monoline geometric lower case built from the same two primitives, so
# it sits beside the mark as the same hand. Metrics are centreline values
# in wordmark units; the stroke is WW. "linux" is lower case because that
# is how the kernel's name is written; "LP" is capitals because it is a
# name.
WW = 2.3125   # 37/16
XH = 8.0       # x-height, centreline to centreline
ASC = 13.0     # ascender and cap height, centreline
GAP = 3.125    # space between letters, stroke edge to stroke edge

# Each glyph: (advance-without-gap, prims, accent-prims), drawn with the
# baseline centreline at y=0 and the left stroke centreline at x=0.
#
# Every number here is a multiple of 1/16. The splash stores the letters
# in sixteenths, and with values like 3.1 the rounding added up along the
# word until the boot screen's "linux-LP" sat half a pixel from the SVG's.


def _l():
    return 0, [Seg(0, -ASC, 0, 0, WW)], []


def _i():
    # the dot is the accent - a small echo of the ring
    return 0, [Seg(0, -XH, 0, 0, WW)], [Seg(0, -ASC + 0.1875, 0, -ASC + 0.1875, 3.0)]


def _n():
    r = XH * 0.5 - 0.25
    return 2 * r, [Seg(0, -XH, 0, 0, WW), Arc(r, -XH + r, r, WW, (TR, TL)),
                   Seg(2 * r, -XH + r, 2 * r, 0, WW)], []


def _u():
    r = XH * 0.5 - 0.25
    return 2 * r, [Seg(0, -XH, 0, -r, WW), Arc(r, -r, r, WW, (BL, BR)),
                   Seg(2 * r, -XH, 2 * r, 0, WW)], []


def _x():
    w = 6.875
    return w, [Seg(0, -XH, w, 0, WW), Seg(w, -XH, 0, 0, WW)], []


def _hyphen():
    return 3.625, [Seg(0, -XH * 0.5, 3.625, -XH * 0.5, WW)], []


def _L():
    return 7.1875, [Seg(0, -ASC, 0, 0, WW), Seg(0, 0, 7.1875, 0, WW)], []


def _P():
    b = 7.0            # bowl height, centreline
    r = b / 2
    s = 3.1875         # straight part of the bowl before it turns
    return s + r, [Seg(0, -ASC, 0, 0, WW),
                   Seg(0, -ASC, s, -ASC, WW), Seg(0, -ASC + b, s, -ASC + b, WW),
                   Arc(s, -ASC + r, r, WW, (TR, BR))], []


def _z():
    w = 6.8125
    return w, [Seg(0, -XH, w, -XH, WW), Seg(w, -XH, 0, 0, WW), Seg(0, 0, w, 0, WW)], []


def _e():
    r = XH * 0.5
    return 2 * r, [Seg(0, -r, 2 * r, -r, WW), Arc(r, -r, r, WW, (TR, TL, BL))], []


def _r():
    r = XH * 0.5
    return r + 0.8125, [Seg(0, -XH, 0, 0, WW), Arc(r, -XH + r, r, WW, (TL,)),
                        Seg(r, -XH, r + 0.8125, -XH, WW)], []


def _o():
    r = XH * 0.5
    return 2 * r, [Arc(r, -r, r, WW)], []


GLYPHS = {"l": _l, "i": _i, "n": _n, "u": _u, "x": _x, "-": _hyphen,
          "L": _L, "P": _P, "z": _z, "e": _e, "r": _r, "o": _o}

# Letters whose left side is round sit a little further left, the usual
# optical correction: a curve touching the same line as a stem looks
# further away from its neighbour than the stem does.
ROUND_LEFT = set("eo")


def word(text):
    """(ink prims, accent prims, width) for a string, baseline at y=0."""
    x = 0.0
    ink, acc = [], []
    for i, ch in enumerate(text):
        adv, p, a = GLYPHS[ch]()
        if ch in ROUND_LEFT and i:
            x -= 0.5
        ink += [q.moved(x, 0) for q in p]
        acc += [q.moved(x, 0) for q in a]
        x += adv + WW + GAP
    return ink, acc, x - WW - GAP


# ── SVG output ───────────────────────────────────────────────────────────

HEADER = """<!-- {name} - generated by desktop/branding/src/mark.py; edit that, not this.
     {what} -->
"""


def svg(name, what, vb, body, w=None, h=None):
    x, y, bw, bh = vb
    size = ""
    if w:
        size = f' width="{fmt(w)}" height="{fmt(h)}"'
    return (HEADER.format(name=name, what=what) +
            f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{fmt(x)} {fmt(y)} {fmt(bw)} {fmt(bh)}"{size}>\n'
            f'{body}</svg>\n')


def path_el(prims, fill, extra=""):
    return f'  <path fill="{fill}"{extra} d="{paths(prims)}"/>\n'


def mark_body(ring, ell, ring_fill, ell_fill):
    # the ring first: the stem is drawn over its west side
    return path_el(ring, ring_fill) + path_el(ell, ell_fill)


def tile_defs(uid="t"):
    return (f'  <defs>\n'
            f'    <linearGradient id="{uid}bg" x1="0" y1="0" x2="1" y2="1">\n'
            f'      <stop offset="0" stop-color="{AUBERGINE["500"]}"/>\n'
            f'      <stop offset="1" stop-color="{AUBERGINE["800"]}"/>\n'
            f'    </linearGradient>\n'
            f'    <linearGradient id="{uid}hl" x1="0" y1="0" x2="0" y2="1">\n'
            f'      <stop offset="0" stop-color="#fff" stop-opacity=".30"/>\n'
            f'      <stop offset=".5" stop-color="#fff" stop-opacity="0"/>\n'
            f'    </linearGradient>\n'
            f'  </defs>\n')


def tile_body(size=128, inset=8, radius=26, uid="t", mark=None, glyph_h=None):
    """The mark on the aubergine tile - the app-icon form of the logo, for
    the installer, the favicon and os-release LOGO=. Shares its tile shape
    with every icon in desktop/icons/LP."""
    t = size - 2 * inset
    body = tile_defs(uid)
    body += f'  <path fill="url(#{uid}bg)" d="{rrect_path(inset, inset, t, t, radius)}"/>\n'
    hw = max(1.0, size / 128 * 1.5)
    body += (f'  <path fill="url(#{uid}hl)" fill-rule="evenodd" d="'
             f'{rrect_path(inset, inset, t, t, radius)}'
             f'{rrect_path(inset + hw, inset + hw, t - 2 * hw, t - 2 * hw, radius - hw)}"/>\n')
    ring, ell = mark if mark else mark16()
    gh = glyph_h if glyph_h else t * 0.56
    s = gh / 14.0                              # the mark is 14 units tall
    bx0, by0, bx1, by1 = bbox(ring + ell)
    ox = size / 2 - (bx0 + bx1) / 2 * s
    oy = size / 2 - (by0 + by1) / 2 * s
    ring = [p.moved(ox / s, oy / s, s) for p in ring]
    ell = [p.moved(ox / s, oy / s, s) for p in ell]
    body += mark_body(ring, ell, ORANGE, WHITE)
    return body


def write_logo(outdir):
    os.makedirs(outdir, exist_ok=True)
    out = {}
    ring, ell = mark16()
    vb = (0, 0, 16, 16)
    out["lp-mark.svg"] = svg("lp-mark.svg", "The mark in colour, for dark backgrounds.",
                             vb, mark_body(ring, ell, ORANGE, WHITE), 256, 256)
    out["lp-mark-on-light.svg"] = svg("lp-mark-on-light.svg",
                                      "The mark in colour, for light backgrounds.",
                                      vb, mark_body(ring, ell, ORANGE_ON_LIGHT, INK_DARK), 256, 256)
    out["lp-mono.svg"] = svg("lp-mono.svg",
                             "The mark in one colour (white): top bar, boot, engraving.",
                             vb, path_el(ring + ell, WHITE), 256, 256)
    out["lp-mono-dark.svg"] = svg("lp-mono-dark.svg", "The mark in one colour (ink).",
                                  vb, path_el(ring + ell, INK_DARK), 256, 256)
    r24, e24 = mark24()
    out["lp-mark-24px.svg"] = svg("lp-mark-24px.svg",
                                  "The mark placed on a 24px pixel grid; for 24px renders only.",
                                  (0, 0, 24, 24), mark_body(r24, e24, ORANGE, WHITE), 24, 24)
    out["lp-tile.svg"] = svg("lp-tile.svg",
                             "The mark on its tile: app icon, favicon, os-release LOGO.",
                             (0, 0, 128, 128), tile_body(), 256, 256)

    # wordmark: "linux-LP"
    ink, acc, wwid = word("linux-LP")
    x0, y0, x1, y1 = bbox(ink + acc)
    pad = 1.0
    vbw = (x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad)
    for name, inkc, accc, what in (
            ("lp-wordmark.svg", WHITE, ORANGE, "The wordmark, for dark backgrounds."),
            ("lp-wordmark-on-light.svg", INK_DARK, ORANGE_ON_LIGHT,
             "The wordmark, for light backgrounds.")):
        out[name] = svg(name, what, vbw, path_el(ink, inkc) + path_el(acc, accc),
                        vbw[2] * 8, vbw[3] * 8)

    # lockup: mark left, wordmark right, wordmark cap height = 0.62 mark
    lk = lockup()
    for name, dark in (("lp-lockup.svg", True), ("lp-lockup-on-light.svg", False)):
        ringc = ORANGE if dark else ORANGE_ON_LIGHT
        inkc = WHITE if dark else INK_DARK
        body = (path_el(lk["ring"], ringc) + path_el(lk["ell"], inkc) +
                path_el(lk["ink"], inkc) + path_el(lk["acc"], ringc))
        out[name] = svg(name, "Mark and wordmark side by side, for "
                        + ("dark" if dark else "light") + " backgrounds.",
                        lk["vb"], body, lk["vb"][2] * 6, lk["vb"][3] * 6)
    for k, v in out.items():
        with open(os.path.join(outdir, k), "w") as f:
            f.write(v)
    return out


def lockup(text="linux-LP"):
    ring, ell = mark16()
    ink, acc, wwid = word(text)
    # wordmark scaled so its cap height (outer) is 62% of the mark's 14
    cap_outer = ASC + WW
    s = 14 * 0.62 / cap_outer
    # baseline of the wordmark on the mark's foot baseline (y=15 outer
    # bottom of the foot is y=15; the wordmark's outer bottom is +WW/2)
    base = 15 - WW / 2 * s
    gap = 5.2
    ox = 13 + gap
    ink = [p.moved(ox / s, base / s, s) for p in ink]
    acc = [p.moved(ox / s, base / s, s) for p in acc]
    x0, y0, x1, y1 = bbox(ring + ell + ink + acc)
    pad = 1.0
    return {"ring": ring, "ell": ell, "ink": ink, "acc": acc,
            "vb": (x0 - pad, y0 - pad, x1 - x0 + 2 * pad, y1 - y0 + 2 * pad)}


# ── the boot splash's copy ───────────────────────────────────────────────
#
# splash.c cannot read an SVG, so it gets the same primitives as integer
# tables. Coordinates are in sixteenths of a design unit (the mark is 14
# units tall), which keeps every value in an int16 and loses nothing that
# a screen could show.

SPLASH_Q = 16
SPLASH_INK = "#ffffff"          # the L
SPLASH_WORD = "#dbe8f0"         # the name under it: a warm off-white, one
                                # step quieter than the mark


def _hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def _q(v):
    """v in sixteenths - exactly, or the splash and the SVGs disagree."""
    n = v * SPLASH_Q
    assert abs(n - round(n)) < 1e-6, f"{v} is not a multiple of 1/{SPLASH_Q}"
    return int(round(n))


def _prim_c(p, accent):
    q = _q
    if isinstance(p, Seg):
        return f"{{ LP_SEG, 0x0, {int(accent)}, {q(p.x0)}, {q(p.y0)}, {q(p.x1)}, {q(p.y1)}, {q(p.w)} }}"
    mask = sum(1 << k for k in p.quads)
    return f"{{ LP_ARC, 0x{mask:x}, {int(accent)}, {q(p.cx)}, {q(p.cy)}, {q(p.r)}, 0, {q(p.w)} }}"


# Where the splash puts things. They are here, in the generated header,
# and not as numbers in splash.c, because a second program has to put
# the logo on exactly the same pixels: desktop/branding/lp-splash-fade,
# the session's first frame, which takes over from the splash and fades
# into the desktop. Two copies of "15% of the height, centred at 44%"
# would drift the first time one was tuned, and the hand-off would jump.
SPLASH_LAYOUT = [
    ("MARK_H", 15, "the mark's height, % of the screen's height"),
    ("MARK_MAXW", 22, "...but at most this % of its width (portrait panels)"),
    ("MARK_MIN", 24, "...and at least this many pixels"),
    ("CENTRE_Y", 44, "the mark's centre, % down the screen: dead centre reads low"),
    ("WORD_CAP", 27, "the name's cap height, % of the mark's height"),
    ("WORD_GAP", 62, "the name's baseline, % of the mark's height below its bottom"),
    ("SPIN_Y", 78, "the spinner's centre, % down the screen"),
    ("SPIN_R", 80, "the spinner's radius (to the stroke's centre), per mille of the mark"),
    ("SPIN_W", 18, "the spinner's stroke, per mille of the mark's height"),
    ("OEM_MARK_H", 6, "under a PC maker's logo (ACPI BGRT): the mark's height, % of the screen's"),
    ("OEM_CENTRE_Y", 84, "...its centre, % down the screen - near the bottom, as Ubuntu's"),
    ("OEM_SPIN_Y", 70, "...the spinner, when the maker's logo has no known bottom"),
]

# The splash's motion, from the design system's table (design/feel.md §2,
# desktop/common/lp-motion.h). The logo arriving is the one long move -
# the brief asks for about 400ms, the ceiling feel.md allows. The spinner
# comes and goes in a sheet's times (critically damped here, where the
# sheet spring is 0.95 - a difference no one can see in an opacity). One
# revolution in 1.3 s is calm: a faster spinner reads as urgency, and
# nothing is urgent at boot.
SPLASH_MOTION = [
    ("LOGO_IN_MS", 400, "the logo fading in"),
    ("SPIN_DELAY_MS", 1000, "held back after the logo: a fast boot never shows it"),
    ("SPIN_IN_MS", 260, "the spinner arriving (the sheet spring)"),
    ("SPIN_OUT_MS", 182, "and leaving: 0.7x"),
    ("SPIN_TURN_MS", 1300, "one revolution"),
    ("REDUCED_MS", 100, "any change, when motion is reduced: a crossfade this long"),
    ("PULSE_MS", 2400, "reduced motion's stand-in for turning: a slow brightness pulse"),
    ("HOLD_MS", 90000, "give up waiting for the desktop and hand the screen back"),
]


def _splash_layout(A):
    A("/* Layout - shared with desktop/branding/lp-splash-fade, which draws the")
    A(" * session's first frame on the same pixels (see mark.py SPLASH_LAYOUT). */")
    for k, v, what in SPLASH_LAYOUT:
        A(f"#define LP_LAYOUT_{k:10s} {v:5d}   /* {what} */")
    A("")


def _omega(ms):
    """The natural frequency of a critically damped spring that has
    settled by `ms` under lp-motion.c's rule: within 0.5% of the distance
    and moving slower than 5% of it per second. (Position alone gives
    w T = 7.43; the speed rule is what makes a short spring stiffer.)"""
    import math
    T = ms / 1000.0
    u = 1.0
    while (1 + u) * math.exp(-u) > 0.005 or u * u * math.exp(-u) / T > 0.05:
        u += 0.0005
    return u / T


def _splash_motion(A):
    import math
    A("/* Motion (mark.py SPLASH_MOTION; design/feel.md §2). */")
    for k, v, what in SPLASH_MOTION:
        A(f"#define LP_MOTION_{k:14s} {v:6d}   /* {what} */")
    A("")
    # The fades are springs, x'' = -w^2 (x - target) - 2 w x', stepped in
    # fixed point by splash.c. The splash has no floating point, so the
    # one number each needs is solved here.
    A("/* w for each of those springs (critically damped, settled by the time")
    A(" * above under lp-motion.c's rule), in 1/16 rad/s. */")
    for k in ("LOGO_IN_MS", "SPIN_IN_MS", "SPIN_OUT_MS", "REDUCED_MS"):
        ms = dict((a, b) for a, b, _ in SPLASH_MOTION)[k]
        A(f"#define LP_W16_{k[:-3]:10s} {int(round(_omega(ms) * 16)):5d}   /* {ms} ms */")
    A("")
    # the pulse: one smooth rise, (1 - cos(pi u)) / 2, used up then down
    wave = [int(round((1 - math.cos(math.pi * i / 64)) / 2 * 65535)) for i in range(65)]
    A("/* LP_WAVE[i]: (1 - cos(pi i/64)) / 2 in 0..65535 - a smooth rise from 0 to")
    A(" * 1: reduced motion's breathing, run forwards then backwards, and the")
    A(" * cosine the spinner's head is placed with. */")
    A("static const u16 LP_WAVE[65] = {")
    for i in range(0, 65, 13):
        A("    " + " ".join(f"{v:5d}," for v in wave[i:i + 13]))
    A("};")
    A("")
    at = [int(round(math.atan(i / 64) / (2 * math.pi) * 65536)) for i in range(65)]
    A("/* LP_ATAN[i]: atan(i/64) in 1/65536ths of a turn (0..8192, i.e. 0..45")
    A(" * degrees) - enough, with the octant, to give every pixel of the spinner")
    A(" * its angle without floating point. */")
    A("static const u16 LP_ATAN[65] = {")
    for i in range(0, 65, 13):
        A("    " + " ".join(f"{v:5d}," for v in at[i:i + 13]))
    A("};")
    A("")


def write_splash_header(path):
    import wallpaper
    ring, ell = mark16()
    lines = []
    A = lines.append
    A("/* logo.h - the LP mark, the wordmark's letters, the desktop's gradient,")
    A(" * and where and how fast the boot splash draws them, as tables for")
    A(" * splash.c - and for desktop/branding/lp-splash-fade, which draws the")
    A(" * session's first frame to match it (it defines u8, s16 and u16 first).")
    A(" *")
    A(" * GENERATED by desktop/branding/src/mark.py from the same shapes that")
    A(" * produce the SVGs in desktop/branding/logo - change them there and rerun")
    A(" * `python3 make.py` in desktop/branding, never by hand here, or the boot")
    A(" * screen and the logo files stop being the same drawing.")
    A(" *")
    A(" * A primitive is a round-ended straight stroke (LP_SEG: x0 y0 x1 y1) or a")
    A(" * circular stroke over whole quadrants (LP_ARC: cx cy r, quads = bit 0")
    A(" * top-right, 1 top-left, 2 bottom-left, 3 bottom-right), both with width w.")
    A(f" * Units are 1/{SPLASH_Q} of a design unit; y grows downwards. `accent` picks")
    A(" * the orange instead of the ink colour. */")
    A("#ifndef LP_SPLASH_LOGO_H")
    A("#define LP_SPLASH_LOGO_H")
    A("")
    A("enum { LP_SEG, LP_ARC };")
    A("")
    A("typedef struct {")
    A("    u8  kind, quads, accent;")
    A("    s16 a, b, c, d, w;")
    A("} lp_prim_t;")
    A("")
    A(f"#define LP_UNIT {SPLASH_Q}")
    A("")
    A("/* The mark: ring first, the L drawn over its west side. Its box is")
    x0, y0, x1, y1 = bbox(ring + ell)
    A(f" * ({fmt(x0)},{fmt(y0)})-({fmt(x1)},{fmt(y1)}) in design units. */")
    A(f"#define LP_MARK_X0 {int(round(x0 * SPLASH_Q))}")
    A(f"#define LP_MARK_Y0 {int(round(y0 * SPLASH_Q))}")
    A(f"#define LP_MARK_X1 {int(round(x1 * SPLASH_Q))}")
    A(f"#define LP_MARK_Y1 {int(round(y1 * SPLASH_Q))}")
    A("static const lp_prim_t LP_MARK[] = {")
    for p in ring:
        A("    " + _prim_c(p, True) + ",")
    for p in ell:
        A("    " + _prim_c(p, False) + ",")
    A("};")
    A("")
    A("/* The wordmark's letters, baseline at y=0, left stroke centre at x=0.")
    A(" * A letter's pen advance is adv + LP_TRACK; `kern` is added before it")
    A(" * is drawn (round-sided letters sit a little into the gap). A letter")
    A(" * not listed here is left as a space - add it to mark.py's GLYPHS. */")
    A(f"#define LP_TRACK {_q(WW + GAP)}")
    A(f"#define LP_CAP   {_q(ASC + WW)}   /* cap height, outer edge to edge */")
    A("typedef struct { char ch; s16 adv, kern; u8 first, count; } lp_glyph_t;")
    prims, glyphs = [], []
    for ch in sorted(GLYPHS):
        adv, p, a = GLYPHS[ch]()
        first = len(prims)
        prims += [_prim_c(x, False) for x in p] + [_prim_c(x, True) for x in a]
        kern = -0.5 if ch in ROUND_LEFT else 0.0
        glyphs.append(f"    {{ '{ch}', {_q(adv)}, {_q(kern)}, "
                      f"{first}, {len(prims) - first} }},")
    A("static const lp_prim_t LP_LETTER_PRIMS[] = {")
    for p in prims:
        A("    " + p + ",")
    A("};")
    A("static const lp_glyph_t LP_LETTERS[] = {")
    lines.extend(glyphs)
    A("};")
    A("")
    A("/* Colours, 8 bits per channel. */")
    for name, col in (("LP_RGB_ACCENT", ORANGE), ("LP_RGB_INK", SPLASH_INK),
                      ("LP_RGB_WORD", SPLASH_WORD)):
        r, g, b = _hexrgb(col)
        A(f"#define {name} {{ 0x{r:02x}, 0x{g:02x}, 0x{b:02x} }}   /* {col} */")
    A("")
    A("/* The dark wallpaper's gradient (desktop/branding/src/wallpaper.py):")
    A(" * light from a point at LP_FOCUS_X, LP_FOCUS_Y (1/4096ths of the width")
    A(" * and height), and the colour at 65 evenly spaced distances from it, 0 at")
    A(" * the focus and 64 at the farthest corner, as 8.8 fixed-point sRGB. */")
    fx, fy = wallpaper.FOCUS
    A(f"#define LP_FOCUS_X {int(round(fx * 4096))}")
    A(f"#define LP_FOCUS_Y {int(round(fy * 4096))}")
    tab = wallpaper.splash_table(65)
    A(f"#define LP_GRAD_N {len(tab)}")
    A("static const u16 LP_GRAD[LP_GRAD_N][3] = {")
    for i in range(0, len(tab), 4):
        A("    " + " ".join(f"{{ {r:5d}, {g:5d}, {b:5d} }}," for r, g, b in tab[i:i + 4]))
    A("};")
    A("")
    _splash_layout(A)
    _splash_motion(A)
    A("#endif")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    import sys
    write_logo(sys.argv[1] if len(sys.argv) > 1 else "logo")
