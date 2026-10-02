"""raster.py - the logo drawn straight from its shapes, without an SVG renderer.

The mark and wordmark are two primitives (geom.py), and each primitive
knows its own signed distance. So a pixel's coverage can be measured
directly: sample it on a fine grid, count the samples inside, and that is
the exact area of the pixel the shape covers - which is what a good SVG
renderer computes, reached without one.

This is what the PNG renders, the favicon and the boot menu's alpha
tables are made with. It means `make.py` needs numpy and Pillow and
nothing else - no librsvg on the build host, no chroot - and that every
bitmap of the logo is the same drawing as the SVGs and the boot splash,
because all three read the same Seg and Arc objects.
"""

import numpy as np

from geom import bbox


def _grid(W, H, vb, ss):
    x0, y0, vw, vh = vb
    xs = x0 + (np.arange(W * ss) + 0.5) / (W * ss) * vw
    ys = y0 + (np.arange(H * ss) + 0.5) / (H * ss) * vh
    return np.meshgrid(xs, ys)


def _down(inside, W, H, ss):
    return inside.reshape(H, ss, W, ss).mean(axis=(1, 3))


def coverage(prims, W, H, vb, ss=8):
    """Area coverage (0..1, shape H x W) of the union of `prims`, with the
    design-unit box vb = (x, y, w, h) mapped onto W x H pixels."""
    px, py = _grid(W, H, vb, ss)
    d = np.full(px.shape, np.inf)
    for p in prims:
        d = np.minimum(d, p.sdf(px, py))
    return _down(d < 0, W, H, ss)


def rrect_sdf(px, py, x, y, w, h, r):
    cx, cy = x + w / 2, y + h / 2
    qx = np.abs(px - cx) - (w / 2 - r)
    qy = np.abs(py - cy) - (h / 2 - r)
    out = np.hypot(np.maximum(qx, 0), np.maximum(qy, 0))
    return out + np.minimum(np.maximum(qx, qy), 0) - r


def hex_rgb(h):
    h = h.lstrip("#")
    return np.array([int(h[i:i + 2], 16) / 255.0 for i in (0, 2, 4)])


def over(dst, rgb, a):
    """Composite a flat colour at coverage a over premultiplied RGBA dst."""
    a = a[..., None]
    dst[..., :3] = np.asarray(rgb) * a + dst[..., :3] * (1 - a)
    dst[..., 3:] = a + dst[..., 3:] * (1 - a)
    return dst


def to_image(prem):
    """Premultiplied float RGBA to a straight-alpha 8-bit PIL image."""
    from PIL import Image
    a = prem[..., 3:]
    rgb = np.where(a > 0, prem[..., :3] / np.maximum(a, 1e-9), 0)
    out = np.concatenate([rgb, a], axis=-1)
    return Image.fromarray(np.clip(np.round(out * 255), 0, 255).astype(np.uint8), "RGBA")


def layers(W, H, vb, groups, ss=8):
    """groups: [(prims, '#rrggbb'), ...] in drawing order, on transparent."""
    img = np.zeros((H, W, 4))
    for prims, col in groups:
        over(img, hex_rgb(col), coverage(prims, W, H, vb, ss))
    return img


def tile(N, top, bottom, mark_groups, inset=8, radius=26, ss=8, rim=0.30):
    """The app-icon tile at N pixels: the rounded square in its diagonal
    gradient, the lit rim, then the mark - the same recipe as
    mark.tile_body's SVG, for the sizes that ship as PNGs."""
    vb = (0, 0, 128, 128)
    px, py = _grid(N, N, vb, ss)
    t = 128 - 2 * inset
    d = rrect_sdf(px, py, inset, inset, t, t, radius)
    body = _down(d < 0, N, N, ss)
    hw = max(1.0, 128 / 128 * 1.5)
    inner = rrect_sdf(px, py, inset + hw, inset + hw, t - 2 * hw, t - 2 * hw, radius - hw)
    ring = _down((d < 0) & (inner >= 0), N, N, ss)
    # the gradient runs corner to corner of the tile's box (SVG's
    # objectBoundingBox with x1=y1=0, x2=y2=1); colours blend in sRGB as SVG does
    cx = (np.arange(N) + 0.5) / N * 128
    u = (cx[None, :] - inset) / t
    v = (cx[:, None] - inset) / t
    g = np.clip((u + v) / 2, 0, 1)[..., None]
    col = hex_rgb(top) * (1 - g) + hex_rgb(bottom) * g
    img = np.zeros((N, N, 4))
    img[..., :3] = col * body[..., None]
    img[..., 3] = body
    va = np.clip(1 - v / 0.5, 0, 1) * rim
    over(img, (1, 1, 1), ring * va)
    for prims, c in mark_groups:
        over(img, hex_rgb(c), coverage(prims, N, N, vb, ss))
    return img


def fit(prims, box, target):
    """Scale and move prims so their bounding box is centred in `box`
    (x, y, w, h) at height `target`."""
    x0, y0, x1, y1 = bbox(prims)
    s = target / (y1 - y0)
    bx, by, bw, bh = box
    ox = bx + bw / 2 - (x0 + x1) / 2 * s
    oy = by + bh / 2 - (y0 + y1) / 2 * s
    return [p.moved(ox / s, oy / s, s) for p in prims]
