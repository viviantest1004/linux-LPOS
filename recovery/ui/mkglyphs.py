#!/usr/bin/env python3
"""mkglyphs.py - bake the boot and recovery menus' text into bitmaps.

Neither the UEFI boot menu nor the recovery menu has a font renderer:
the first runs before any operating system does, and the second is a
static program on our own libc, which has no FreeType and no floating
point to run one with. So the glyphs are rendered here, once, from the
desktop's own face (Pretendard - the reason Korean and Latin sit on one
baseline at one weight), and compiled in as alpha bitmaps.

Three faces, one per size class in lp-strings.h, each rendered at the
exact pixel size it is drawn at on the owner's 3840x2160 panel:

    T  SemiBold  104 px    titles
    M  Medium     60 px    menu items, buttons
    S  Regular    40 px    everything smaller

At 2160 lines a glyph is therefore drawn 1:1 - no scaling, which is the
only way to be sure text is crisp at 4K. On a smaller screen the program
shrinks it with an area filter (lp-ui.h), and shrinking an anti-aliased
bitmap loses nothing a person can see. Growing one would; that is why
the masters are the largest size and not a middle one.

Each face holds only the characters the strings of its class use, plus
all of printable ASCII in M and S, because those two also draw text that
is not in the catalog (account names, fsck's output). That keeps a
Hangul-capable atlas to a few hundred kilobytes.

Alpha is stored at 4 bits a pixel, two to a byte, cropped to each
glyph's ink. Sixteen levels of edge coverage are indistinguishable from
256 at these sizes, and it halves the table.

    /usr/bin/python3.12 recovery/ui/mkglyphs.py   (rewrites lp-glyphs.h and
                                                   lp-glyphs-mono.h)

lp-glyphs-mono.h is a fourth face for the recovery shell's terminal
(recovery/term.c): D2Coding, the desktop's monospaced face, at 44 px on
a 2160-line panel, holding printable ASCII, Latin-1, the box-drawing
and block characters and a few symbols ls, less and editors print. It
is a separate header because only lp-recovery draws a terminal; the
boot menu includes lp-glyphs.h alone and stays small. Hangul is left
out on purpose: every syllable at this size is megabytes of source, and
the shell's own messages are English (a Korean file name shows as boxes
two cells wide, and `ls` still lists it correctly).

Needs Pillow and the Pretendard variable font; LP_FONT overrides where
the font is. (The build host's default python3 has a broken Pillow, which
is why the command above names 3.12.) The output is committed, so a
normal build needs neither.
"""
import os
import re
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
FONT = os.environ.get(
    "LP_FONT",
    "/home/user/kernel-work/deb/usr/share/fonts/truetype/pretendard/PretendardVariable.ttf")

# class -> (pixel size, weight on the variable font's wght axis)
FACES = [("T", 104, 600), ("M", 60, 500), ("S", 40, 400)]

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
# Characters drawn by code rather than written in a string: the password
# bullet, the on-screen keyboard's special keys and arrows, the
# replacement box.
EXTRA = {"T": "0123456789", "M": ASCII + "•⌫⏎⇧×←↑→↓", "S": ASCII + "•…·"}


MONO_FONT = os.environ.get(
    "LP_MONO_FONT",
    "/home/user/kernel-work/deb/usr/share/fonts/truetype/d2coding/D2Coding-Ver1.3.2-20180524.ttf")
MONO_PX = 44
MONO_CHARS = (ASCII
              + "".join(chr(c) for c in range(0xA1, 0x100) if c != 0xAD)
              + "‘’“”•…←↑→↓■●◆✓�"
              + "".join(chr(c) for c in range(0x2500, 0x2580))
              + "".join(chr(c) for c in range(0x2580, 0x25A0)))


def strings_by_class():
    """(class, text) for every English and Korean string in the catalog."""
    src = open(os.path.join(HERE, "lp-strings.h"), encoding="utf-8").read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    out = []
    for m in re.finditer(r"LPS\(\s*(\w+)\s*,\s*([TMS])\s*,((?:\s*\"(?:[^\"\\]|\\.)*\"\s*,?)+)\)", src):
        parts = re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(3))
        if len(parts) != 2:
            sys.exit("lp-strings.h: %s needs exactly an English and a Korean string" % m.group(1))
        for p in parts:
            out.append((m.group(2), p.encode().decode("unicode_escape").encode("latin-1").decode("utf-8")))
    return out


def render(font, ch):
    """(advance*64, x0, y0, w, h, 4-bit rows) - x0/y0 from the pen on the baseline."""
    adv = int(round(font.getlength(ch) * 64))
    l, t, r, b = font.getbbox(ch, anchor="ls")
    if r <= l or b <= t:
        return adv, 0, 0, 0, 0, b""
    pad = 2
    w, h = r - l + 2 * pad, b - t + 2 * pad
    img = Image.new("L", (w, h), 0)
    ImageDraw.Draw(img).text((pad - l, pad - t), ch, font=font, fill=255, anchor="ls")
    box = img.getbbox()
    if not box:
        return adv, 0, 0, 0, 0, b""
    img = img.crop(box)
    x0, y0 = l - pad + box[0], t - pad + box[1]
    w, h = img.size
    px = img.load()
    data = bytearray()
    for y in range(h):
        for x in range(0, w, 2):
            a = (px[x, y] * 15 + 127) // 255
            b2 = (px[x + 1, y] * 15 + 127) // 255 if x + 1 < w else 0
            data.append(a | (b2 << 4))
    return adv, x0, y0, w, h, bytes(data)


def main():
    by_class = {c: set(EXTRA[c]) for c, _, _ in FACES}
    for cls, text in strings_by_class():
        for ch in text:
            if ch not in "\n":
                by_class[cls].add(ch)

    missing_probe = None
    out = []
    w = out.append
    w("/* lp-glyphs.h - GENERATED by recovery/ui/mkglyphs.py from lp-strings.h and\n"
      " * Pretendard. Do not edit; add the string to lp-strings.h and rerun it.\n"
      " *\n"
      " * Three faces (T, M, S), each rendered at the size it is drawn at on a\n"
      " * 2160-line panel. A glyph is its ink box as 4-bit alpha, two pixels a\n"
      " * byte (low nibble first), rows padded to a whole byte; x0/y0 place the\n"
      " * box relative to the pen on the baseline (y0 < 0 is above it). Glyphs\n"
      " * are sorted by code point for a binary search. */\n"
      "#ifndef LP_GLYPHS_H\n#define LP_GLYPHS_H\n\n"
      "typedef struct {\n"
      "    unsigned int   cp;\n"
      "    short          adv64;      /* advance, 1/64 px */\n"
      "    short          x0, y0;\n"
      "    unsigned short w, h;\n"
      "    unsigned int   off;        /* into the face's data */\n"
      "} lpg_glyph_t;\n\n"
      "typedef struct {\n"
      "    short px, ascent, descent, line;\n"
      "    unsigned short n;\n"
      "    const lpg_glyph_t *g;\n"
      "    const unsigned char *data;\n"
      "} lpg_face_t;\n\n")
    total = 0
    faces = []
    for cls, size, weight in FACES:
        font = ImageFont.truetype(FONT, size)
        font.set_variation_by_axes([weight])
        if missing_probe is None:
            missing_probe = bytes(font.getmask("\U0010FFFD"))
        glyphs = []
        data = bytearray()
        for ch in sorted(by_class[cls]):
            if ch != " " and bytes(font.getmask(ch)) == missing_probe:
                sys.exit("Pretendard has no glyph for %r (U+%04X) in class %s" % (ch, ord(ch), cls))
            adv, x0, y0, gw, gh, bits = render(font, ch)
            glyphs.append((ord(ch), adv, x0, y0, gw, gh, len(data)))
            data += bits
        asc, desc = font.getmetrics()
        total += len(data) + len(glyphs) * 16
        w("/* %s: %d glyphs, %d bytes of alpha */\n" % (cls, len(glyphs), len(data)))
        w("static const lpg_glyph_t LPG_%s_G[%d] = {\n" % (cls, len(glyphs)))
        for g in glyphs:
            w("    { 0x%04x, %d, %d, %d, %d, %d, %d },\n" % g)
        w("};\n")
        w("static const unsigned char LPG_%s_D[%d] = {\n" % (cls, max(1, len(data))))
        for i in range(0, len(data), 24):
            w("    " + ",".join("%d" % b for b in data[i:i + 24]) + ",\n")
        if not data:
            w("    0\n")
        w("};\n\n")
        faces.append((cls, size, asc, desc, int(round(size * 1.3)), len(glyphs)))
    w("enum { LPG_T, LPG_M, LPG_S };\n")
    w("static const lpg_face_t LPG_FACES[3] = {\n")
    for cls, size, asc, desc, line, n in faces:
        w("    { %d, %d, %d, %d, %d, LPG_%s_G, LPG_%s_D },\n" % (size, asc, desc, line, n, cls, cls))
    w("};\n\n#endif\n")
    path = os.path.join(HERE, "lp-glyphs.h")
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        f.write("".join(out))
    os.replace(path + ".tmp", path)
    print("%s: %d bytes of glyph data, %d bytes of source"
          % (path, total, os.path.getsize(path)))


def main_mono():
    font = ImageFont.truetype(MONO_FONT, MONO_PX)
    probe = bytes(font.getmask("\U0010FFFD"))
    glyphs, data = [], bytearray()
    for ch in sorted(set(MONO_CHARS)):
        if ch != " " and bytes(font.getmask(ch)) == probe:
            continue                    # not in D2Coding: drawn as a box
        adv, x0, y0, gw, gh, bits = render(font, ch)
        glyphs.append((ord(ch), adv, x0, y0, gw, gh, len(data)))
        data += bits
    asc, desc = font.getmetrics()
    cell_w = int(round(font.getlength("M")))
    line = asc + desc + 3               # a little air between rows
    out = []
    w = out.append
    w("/* lp-glyphs-mono.h - GENERATED by recovery/ui/mkglyphs.py from D2Coding.\n"
      " * Do not edit. The recovery shell's terminal face (recovery/term.c): one\n"
      " * cell is LPG_MONO_CELL_W x LPG_MONO_LINE pixels on a 2160-line panel,\n"
      " * baseline LPG_MONO_ASCENT below the cell's top. Same glyph format as\n"
      " * lp-glyphs.h. %d glyphs, %d bytes of alpha. */\n" % (len(glyphs), len(data)))
    w("#ifndef LP_GLYPHS_MONO_H\n#define LP_GLYPHS_MONO_H\n\n#include \"lp-glyphs.h\"\n\n")
    w("#define LPG_MONO_PX %d\n#define LPG_MONO_CELL_W %d\n#define LPG_MONO_LINE %d\n"
      "#define LPG_MONO_ASCENT %d\n\n" % (MONO_PX, cell_w, line, asc + 1))
    w("static const lpg_glyph_t LPG_MONO_G[%d] = {\n" % len(glyphs))
    for g in glyphs:
        w("    { 0x%04x, %d, %d, %d, %d, %d, %d },\n" % g)
    w("};\nstatic const unsigned char LPG_MONO_D[%d] = {\n" % max(1, len(data)))
    for i in range(0, len(data), 24):
        w("    " + ",".join("%d" % b for b in data[i:i + 24]) + ",\n")
    w("};\nstatic const lpg_face_t LPG_MONO = { %d, %d, %d, %d, %d, LPG_MONO_G, LPG_MONO_D };\n\n"
      % (MONO_PX, asc, desc, line, len(glyphs)))
    w("#endif\n")
    path = os.path.join(HERE, "lp-glyphs-mono.h")
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        f.write("".join(out))
    os.replace(path + ".tmp", path)
    print("%s: %d glyphs, %d bytes of glyph data, cell %dx%d"
          % (path, len(glyphs), len(data), cell_w, line))


if __name__ == "__main__":
    main()
    main_mono()
