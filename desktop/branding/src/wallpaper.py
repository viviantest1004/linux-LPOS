"""wallpaper.py - the deep-blue desktop, and the gradient the boot splash shares.

The owner's mockup has an aubergine-to-magenta radial gradient, light at
the top left and deep purple at the bottom right. Two things decide how
it is made rather than what it looks like.

Banding. A gradient this dark and this wide spends only a few dozen
8-bit steps across 3840 pixels, so plain rounding paints visible
contour lines 60-100 pixels apart - on an OLED or a good IPS panel they
are the first thing the eye finds. The colour is computed at full
precision and then dithered to 8 bits with an 8x8 ordered (Bayer)
pattern: every pixel is within one step of the true value and the
average over any 8x8 block is the true value, so the steps dissolve. An
ordered pattern rather than random noise because it is invisible at
this density and a PNG compresses its repetition several times better
- random noise would make this file three times the size for nothing.

Interpolation. The stops are blended in OKLab, not in sRGB. Blending
magenta to deep purple in sRGB passes through a dull brownish middle;
OKLab keeps the hue travelling straight and the lightness falling
evenly, which is what makes the result look lit rather than painted.

The same function, sampled at 65 points, is written into
userland/splash/logo.h by mark.py, so the boot screen is drawn on the
exact gradient the desktop then appears on (the dark variant).

Needs numpy and Pillow. ~5 seconds per variant.

    python3 wallpaper.py OUTDIR [WIDTH HEIGHT]
"""

import sys
import numpy as np

# Where the light comes from, as fractions of the width and height.
# Slightly off the top-left corner so the brightest point is on screen
# but the corner itself is not a hot spot.
FOCUS = (0.10, 0.02)

# (position 0..1 from the focus to the farthest corner, colour)
DARK = [
    (0.00, "#23a6a0"),
    (0.20, "#177585"),
    (0.46, "#12476e"),
    (0.74, "#0d2846"),
    (1.00, "#07121f"),
]

# The light variant is the same light falling on a paler surface: it is
# for the light interface style, where the desktop icons' labels turn dark,
# so the top left has to be light enough for dark text and the bottom
# right can keep some of the blue.
LIGHT = [
    (0.00, "#f1f8fa"),
    (0.28, "#cfe6ec"),
    (0.58, "#9fc7d6"),
    (0.84, "#6b9dba"),
    (1.00, "#46739a"),
]


def hex2rgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.clip(c, 0, 1)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * np.power(c, 1 / 2.4) - 0.055)


def linear_to_oklab(c):
    M1 = np.array([[0.4122214708, 0.5363325363, 0.0514459929],
                   [0.2119034982, 0.6806995451, 0.1073969566],
                   [0.0883024619, 0.2817188376, 0.6299787005]])
    M2 = np.array([[0.2104542553, 0.7936177850, -0.0040720468],
                   [1.9779984951, -2.4285922050, 0.4505937099],
                   [0.0259040371, 0.7827717662, -0.8086757660]])
    lms = c @ M1.T
    return np.cbrt(lms) @ M2.T


def oklab_to_linear(c):
    M2i = np.array([[1.0, 0.3963377774, 0.2158037573],
                    [1.0, -0.1055613458, -0.0638541728],
                    [1.0, -0.0894841775, -1.2914855480]])
    M1i = np.array([[4.0767416621, -3.3077115913, 0.2309699292],
                    [-1.2684380046, 2.6097574011, -0.3413193965],
                    [-0.0041960863, -0.7034186147, 1.7076147010]])
    lms = (c @ M2i.T) ** 3
    return lms @ M1i.T


def pchip(x, y, t):
    """Monotone cubic (Fritsch-Carlson) through (x, y), evaluated at t.

    Straight lines between the stops were the first version, and every
    stop showed up as a faint ring: the eye sees a change in the rate of
    change (a Mach band) long before it sees a change in colour. A cubic
    whose slope is continuous across each stop has no corner to see; the
    monotone kind is used because an ordinary spline overshoots between
    stops and would put a halo of the wrong colour around the light."""
    h = np.diff(x)
    d = np.diff(y) / h
    m = np.zeros_like(y)
    for i in range(1, len(y) - 1):
        if d[i - 1] * d[i] > 0:
            w1, w2 = 2 * h[i] + h[i - 1], h[i] + 2 * h[i - 1]
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i])
    m[0], m[-1] = d[0], d[-1]
    i = np.clip(np.searchsorted(x, t, side="right") - 1, 0, len(h) - 1)
    s = (t - x[i]) / h[i]
    h00, h10 = 2 * s**3 - 3 * s**2 + 1, s**3 - 2 * s**2 + s
    h01, h11 = -2 * s**3 + 3 * s**2, s**3 - s**2
    return h00 * y[i] + h10 * h[i] * m[i] + h01 * y[i + 1] + h11 * h[i] * m[i + 1]


def ramp(stops, t):
    """sRGB (0..1 floats) at positions t, blended in OKLab."""
    pos = np.array([p for p, _ in stops], float)
    lab = np.array([linear_to_oklab(srgb_to_linear(hex2rgb(c))) for _, c in stops])
    t = np.clip(np.asarray(t, float), 0, 1)
    out = np.stack([pchip(pos, lab[:, k], t) for k in range(3)], axis=-1)
    return linear_to_srgb(oklab_to_linear(out))


def field(w, h):
    """t in 0..1 for every pixel: distance from the focus in a space where
    the width and height both measure 1, so the light spreads to fit the
    screen's shape instead of being a circle that only fits a square."""
    fx, fy = FOCUS
    x = (np.arange(w) + 0.5) / w - fx
    y = (np.arange(h) + 0.5) / h - fy
    dmax = np.hypot(1 - fx, 1 - fy)
    return np.sqrt(x[None, :] ** 2 + y[:, None] ** 2) / dmax


BAYER8 = np.array([
    [0, 32, 8, 40, 2, 34, 10, 42], [48, 16, 56, 24, 50, 18, 58, 26],
    [12, 44, 4, 36, 14, 46, 6, 38], [60, 28, 52, 20, 62, 30, 54, 22],
    [3, 35, 11, 43, 1, 33, 9, 41], [51, 19, 59, 27, 49, 17, 57, 25],
    [15, 47, 7, 39, 13, 45, 5, 37], [63, 31, 55, 23, 61, 29, 53, 21]])


def render(stops, w, h):
    # The ramp through a 16384-entry table rather than per pixel: the
    # colour maths is the slow part and the table is finer than anything
    # 8 bits can show.
    lut_t = np.linspace(0, 1, 16384)
    lut = ramp(stops, lut_t) * 255.0
    t = field(w, h)
    rgb = np.stack([np.interp(t, lut_t, lut[:, k]) for k in range(3)], axis=-1)
    thr = (BAYER8 + 0.5) / 64.0
    thr = np.tile(thr, (h // 8 + 1, w // 8 + 1))[:h, :w]
    # the three channels get the pattern at different offsets, so the
    # dither does not line up into a grey grid of its own
    out = np.empty((h, w, 3), np.uint8)
    for k, (dy, dx) in enumerate(((0, 0), (3, 5), (6, 2))):
        tk = np.roll(np.roll(thr, dy, 0), dx, 1)
        out[..., k] = np.clip(np.floor(rgb[..., k] + tk), 0, 255).astype(np.uint8)
    return out


def splash_table(n=65):
    """The dark ramp sampled at n points, as 8.8 fixed-point sRGB - what
    userland/splash/logo.h carries."""
    t = np.linspace(0, 1, n)
    c = ramp(DARK, t) * 255.0
    return [tuple(int(round(v * 256)) for v in row) for row in c]


if __name__ == "__main__":
    from PIL import Image
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 3840
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 2160
    for name, stops in (("dark", DARK), ("light", LIGHT)):
        Image.fromarray(render(stops, w, h)).save(
            f"{outdir}/lp-wallpaper-{name}.png", optimize=True)
