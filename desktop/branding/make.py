#!/usr/bin/env python3
"""make.py - regenerate every file of the LP identity from src/.

    python3 make.py            everything (about half a minute)
    python3 make.py logo png   only some stages; stages are listed below

Needs Python 3 with numpy and Pillow, nothing else - no SVG renderer, no
chroot. The PNGs are drawn from the shapes themselves (src/raster.py),
the wallpapers from the gradient's own formula (src/wallpaper.py).

Every output is committed, so nothing at image-build time runs this; it
is run by hand after a change to src/, and its outputs are reviewed like
any other diff. It writes outside this directory in exactly two
places, both owned by the branding track: desktop/icons/LP (the icon
theme) and userland/splash/logo.h (the boot splash's tables).

Stages:
  logo       logo/*.svg                  mark, wordmark, lockups, tile
  png        logo/png/*.png, logo/favicon.ico, logo/favicon.svg
  wallpaper  wallpaper/*.png             dark and light, 3840x2160 and 1920x1080
  icons      ../icons/LP                 the icon theme
  splash     ../../userland/splash/logo.h
  c          c/lp-logo-alpha.h           the mark as alpha planes, for the UEFI boot menu
  ascii      ascii/*                     the mark in text, for `info`
  release    os-release                  the recommended /etc/os-release
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "src"))

import numpy as np  # noqa: E402

import icons  # noqa: E402
import mark  # noqa: E402
import raster  # noqa: E402
import wallpaper  # noqa: E402

REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
SIZES = (16, 24, 32, 48, 64, 128, 256, 512)


def out(*p):
    path = os.path.join(HERE, *p)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    return path


def say(what, path):
    print(f"  {what:10s} {os.path.relpath(path, REPO)}")


# ── stages ───────────────────────────────────────────────────────────────

def stage_logo():
    d = out("logo", "x")
    mark.write_logo(os.path.dirname(d))
    say("logo", os.path.dirname(d))


def _mark_for(px):
    """The mark placed for a size: the 24px grid version at 24 (and its
    2x, 48, is a clean multiple of the 16 grid anyway), the 16-grid mark
    everywhere else."""
    if px == 24:
        r, e = mark.mark24()
        return r, e, (0, 0, 24, 24)
    r, e = mark.mark16()
    return r, e, (0, 0, 16, 16)


def stage_png():
    """The mark (colour on dark, colour on light, one-colour white and ink)
    and the tile, at every size a launcher, a web page or a boot screen is
    likely to want, plus the favicon."""
    variants = {
        "lp-mark": (mark.ORANGE, mark.WHITE),
        "lp-mark-on-light": (mark.ORANGE_ON_LIGHT, mark.INK_DARK),
        "lp-mono": (mark.WHITE, mark.WHITE),
        "lp-mono-dark": (mark.INK_DARK, mark.INK_DARK),
    }
    for name, (ringc, ellc) in variants.items():
        for px in SIZES:
            r, e, vb = _mark_for(px)
            img = raster.layers(px, px, vb, [(r, ringc), (e, ellc)])
            p = out("logo", "png", f"{name}-{px}.png")
            raster.to_image(img).save(p, optimize=True)
    say("png", out("logo", "png", "lp-mark-*.png"))
    tiles = {}
    for px in SIZES:
        r, e = mark.mark16()
        t = 128 - 16
        both = raster.fit(r + e, (0, 0, 128, 128), t * 0.56)
        ring, ell = both[:len(r)], both[len(r):]
        img = raster.tile(px, mark.AUBERGINE["500"], mark.AUBERGINE["800"],
                          [(ring, mark.ORANGE), (ell, mark.WHITE)])
        im = raster.to_image(img)
        tiles[px] = im
        im.save(out("logo", "png", f"lp-tile-{px}.png"), optimize=True)
    say("png", out("logo", "png", "lp-tile-*.png"))
    ico = out("logo", "favicon.ico")
    tiles[256].save(ico, sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64)],
                    append_images=[tiles[s] for s in (16, 24, 32, 48, 64)])
    say("favicon", ico)
    with open(out("logo", "lp-tile.svg")) as f:
        svg = f.read()
    with open(out("logo", "favicon.svg"), "w") as f:
        f.write(svg.replace("lp-tile.svg", "favicon.svg", 1))
    say("favicon", out("logo", "favicon.svg"))


def stage_wallpaper():
    """Native sizes only - the XPS panel and 1080p - so no screen this
    ships to ever resamples it: a resampled dither is a blurred dither,
    and the banding comes back."""
    from PIL import Image
    for (w, h) in ((3840, 2160), (1920, 1080)):
        for name, stops in (("dark", wallpaper.DARK), ("light", wallpaper.LIGHT)):
            p = out("wallpaper", f"lp-wallpaper-{name}-{w}x{h}.png")
            Image.fromarray(wallpaper.render(stops, w, h)).save(p, optimize=True)
            say("wallpaper", p)


def stage_icons():
    theme = icons.write_theme(os.path.join(REPO, "desktop", "icons"))
    say("icons", theme)


def stage_splash():
    p = os.path.join(REPO, "userland", "splash", "logo.h")
    mark.write_splash_header(p)
    say("splash", p)


# ── the boot menu's copy of the mark ─────────────────────────────────────

def _rle(plane):
    """(run, value) byte pairs, runs of 1..255. The mark is mostly clear
    or solid, so this is a few kilobytes where the raw plane is 256K."""
    flat = plane.reshape(-1)
    outb = []
    i, n = 0, len(flat)
    while i < n:
        v = flat[i]
        j = i + 1
        while j < n and j - i < 255 and flat[j] == v:
            j += 1
        outb += [j - i, int(v)]
        i = j
    return outb


def stage_c():
    """c/lp-logo-alpha.h: the mark at 256 and 512 pixels as two 8-bit
    alpha planes each - the ring and the L - run-length packed, with the
    ten-line unpacker beside them. Two planes, not a coloured picture,
    because the boot menu draws on whatever background it has chosen and
    in whatever colours its theme says; alpha is the part that is ours.

    The 16-unit design grid lands on whole pixels at both sizes (16 and 32
    pixels a unit), so these are exactly the SVG, edge for edge."""
    r, e = mark.mark16()
    lines = []
    A = lines.append
    A("/* lp-logo-alpha.h - the LP mark as alpha planes, for code that has no")
    A(" * image decoder: the UEFI boot menu and the recovery menu.")
    A(" *")
    A(" * GENERATED by desktop/branding/make.py (stage `c`) from the same shapes as")
    A(" * the SVGs and the boot splash. Do not edit; rerun it.")
    A(" *")
    A(" * For each size there are two planes, one byte of coverage per pixel, row")
    A(" * by row: RING (draw it in LP_LOGO_RGB_ACCENT) and INK (the L, in")
    A(" * LP_LOGO_RGB_INK on a dark background, LP_LOGO_RGB_INK_ON_LIGHT on a")
    A(" * light one). Composite RING first and INK over it - the L covers the")
    A(" * ring's west side. For a one-colour mark draw both in the same colour.")
    A(" *")
    A(" * The square is the mark's 16-unit design box; the ink itself spans")
    A(" * units 3..13 across and 1..15 down, so it sits centred with a margin.")
    A(" * Planes are packed as (run, value) byte pairs; lp_logo_unpack() expands")
    A(" * one into a caller's buffer of size*size bytes. */")
    A("#ifndef LP_LOGO_ALPHA_H")
    A("#define LP_LOGO_ALPHA_H")
    A("")
    for nm, col in (("ACCENT", mark.ORANGE), ("INK", mark.WHITE),
                    ("ACCENT_ON_LIGHT", mark.ORANGE_ON_LIGHT), ("INK_ON_LIGHT", mark.INK_DARK)):
        h = col.lstrip("#")
        A(f"#define LP_LOGO_RGB_{nm} 0x{h}   /* 0xRRGGBB */")
    A("")
    A("static inline void lp_logo_unpack(const unsigned char *rle, unsigned rle_len,")
    A("                                  unsigned char *out, unsigned out_len)")
    A("{")
    A("    for (unsigned i = 0; i + 1 < rle_len && out_len; i += 2) {")
    A("        unsigned run = rle[i];")
    A("        if (run > out_len)")
    A("            run = out_len;")
    A("        for (unsigned k = 0; k < run; k++)")
    A("            *out++ = rle[i + 1];")
    A("        out_len -= run;")
    A("    }")
    A("}")
    total = 0
    for px in (256, 512):
        for plane, prims in (("RING", r), ("INK", e)):
            cov = raster.coverage(prims, px, px, (0, 0, 16, 16), ss=8)
            a = np.clip(np.round(cov * 255), 0, 255).astype(np.uint8)
            data = _rle(a)
            total += len(data)
            A("")
            A(f"#define LP_LOGO_{px}_{plane}_LEN {len(data)}")
            A(f"static const unsigned char LP_LOGO_{px}_{plane}[{len(data)}] = {{")
            for i in range(0, len(data), 20):
                A("    " + ",".join(str(b) for b in data[i:i + 20]) + ",")
            A("};")
    A("")
    A("#endif")
    p = out("c", "lp-logo-alpha.h")
    with open(p, "w") as f:
        f.write("\n".join(lines) + "\n")
    say("c", p)
    print(f"             {total} bytes packed")


# ── the mark in a terminal ───────────────────────────────────────────────

ANSI_RING = "\x1b[38;2;255;106;61m"
ANSI_INK = "\x1b[38;2;255;255;255m"
ANSI_OFF = "\x1b[0m"


def _cells(rows):
    """The mark sampled for a terminal `rows` lines tall. A cell is half as
    wide as it is tall, so each line is two square pixels stacked, and a
    pixel belongs to the ring, the L, or neither - whichever covers most
    of it, with a threshold that keeps the strokes one pixel wide rather
    than letting the round ends swell."""
    r, e = mark.mark16()
    h = rows * 2
    # the mark is 14 units tall (1..15); pixels are square
    unit = h / 14.0
    w = int(round(10 * unit)) + 2
    vb = (3 - 1 / unit, 1, w / unit, 14)
    cr = raster.coverage(r, w, h, vb, ss=8)
    ce = raster.coverage(e, w, h, vb, ss=8)
    px = np.zeros((h, w), np.uint8)
    px[cr > 0.42] = 1
    px[ce > 0.42] = 2                   # the L is drawn over the ring
    return px


def _plain(rows):
    """The mark in plain ASCII, one character per cell: # for the L and @
    for the ring, so it still says which part is which with no colour.
    A cell is sampled whole - two stacked pixels would need two different
    half characters, and ASCII has none that line up."""
    r, e = mark.mark16()
    ch = 14.0 / rows
    cw = ch / 2
    w = int(np.ceil(10.4 / cw)) + 1
    vb = (3 - cw / 2, 1, w * cw, 14)
    cr = raster.coverage(r, w, rows, vb, ss=8)
    ce = raster.coverage(e, w, rows, vb, ss=8)
    lines = []
    for y in range(rows):
        s = "".join("#" if ce[y, x] > 0.4 else "@" if cr[y, x] > 0.4 else " "
                    for x in range(w))
        lines.append(" " + s.rstrip())
    return lines


def _ansi(px):
    """Half blocks in 24-bit colour: each character is the two pixels of
    its cell, the upper one as the foreground of U+2580 and the lower as
    its background when they differ."""
    lines = []
    for y in range(0, px.shape[0], 2):
        s, cur = " ", None
        for x in range(px.shape[1]):
            t, b = px[y, x], px[y + 1, x]
            if t == 0 and b == 0:
                ch, want = " ", None
            elif t == b:
                ch, want = "\u2588", (t, 0)
            elif b == 0:
                ch, want = "\u2580", (t, 0)
            elif t == 0:
                ch, want = "\u2584", (b, 0)
            else:
                ch, want = "\u2580", (t, b)
            if want != cur:
                if cur:
                    s += ANSI_OFF
                if want:
                    s += ANSI_RING if want[0] == 1 else ANSI_INK
                    if want[1]:
                        s += "\x1b[48;2;255;106;61m" if want[1] == 1 else "\x1b[48;2;255;255;255m"
                cur = want
            s += ch
        if cur:
            s += ANSI_OFF
        lines.append(s.rstrip(" "))
    return lines


def stage_ascii():
    """ascii/: the mark for `info` and the login banner, three heights, as
    plain ASCII (any terminal, any font) and as colour half-blocks (a
    UTF-8 terminal with 24-bit colour: foot, or the console with our
    font). Plus ascii/lp-logo-ascii.h with all of it as C strings.
    Heights are in lines: 7, 9 and 12."""
    header = ["/* lp-logo-ascii.h - the LP mark in a terminal, for `info`.",
              " *",
              " * GENERATED by desktop/branding/make.py (stage `ascii`). Each size",
              " * comes as LP_ASCII_<SIZE> (plain ASCII, any terminal) and",
              " * LP_ANSI_<SIZE> (UTF-8 half blocks with 24-bit colour escapes; the",
              " * ring in the accent, the L in white). Print lines top to bottom; the",
              " * ANSI lines reset their colour at the end of each line. Widths are",
              " * the plain version's, in columns. */",
              "#ifndef LP_LOGO_ASCII_H", "#define LP_LOGO_ASCII_H", ""]
    for nm, rows in (("small", 7), ("medium", 9), ("large", 12)):
        plain = _plain(rows)
        ansi = _ansi(_cells(rows))
        with open(out("ascii", f"lp-logo-{nm}.txt"), "w") as f:
            f.write("\n".join(plain) + "\n")
        with open(out("ascii", f"lp-logo-{nm}.ansi"), "w") as f:
            f.write("\n".join(ansi) + "\n")
        up = nm.upper()
        width = max(len(s) for s in plain)
        header.append(f"#define LP_ASCII_{up}_LINES {len(plain)}")
        header.append(f"#define LP_ASCII_{up}_WIDTH {width}")
        for kind, ls in (("ASCII", plain), ("ANSI", ansi)):
            header.append(f"static const char *const LP_{kind}_{up}[] = {{")
            for s in ls:
                c = s.replace("\\", "\\\\").replace("\"", "\\\"").replace("\x1b", "\\033")
                header.append(f"    \"{c}\",")
            header.append("};")
        header.append("")
    header.append("#endif")
    p = out("ascii", "lp-logo-ascii.h")
    with open(p, "w") as f:
        f.write("\n".join(header) + "\n")
    say("ascii", os.path.dirname(p))


# ── os-release ───────────────────────────────────────────────────────────

OS_RELEASE = """\
# os-release - what this system is, for every program that asks.
# GENERATED by desktop/branding/make.py (stage `release`); the image
# installs it as /usr/lib/os-release with /etc/os-release a link to it.
#
# ID_LIKE=debian and VERSION_CODENAME=bookworm are there because the
# desktop's packages come from Debian 12, and install scripts written for
# "a Debian" (Docker's, NodeSource's, most .deb READMEs) read exactly
# these two keys to choose their apt source. Without them they refuse to
# run or pick the wrong suite. They say what apt can install here, not
# that this is Debian: NAME and ID say that.
NAME="linux-LP"
PRETTY_NAME="linux-LP 1.0"
ID=lp
ID_LIKE=debian
VERSION="1.0"
VERSION_ID="1.0"
VERSION_CODENAME=bookworm
LOGO=distributor-logo-lp
ANSI_COLOR="38;2;242;140;40"
"""


def stage_release():
    p = out("os-release")
    with open(p, "w") as f:
        f.write(OS_RELEASE)
    say("release", p)


STAGES = {
    "logo": stage_logo, "png": stage_png, "wallpaper": stage_wallpaper,
    "icons": stage_icons, "splash": stage_splash, "c": stage_c,
    "ascii": stage_ascii, "release": stage_release,
}

if __name__ == "__main__":
    want = sys.argv[1:] or list(STAGES)
    for s in want:
        if s not in STAGES:
            sys.exit(f"make.py: no stage {s!r} (stages: {', '.join(STAGES)})")
    for s in want:
        STAGES[s]()
