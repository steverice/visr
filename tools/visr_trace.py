#!/usr/bin/env python3
"""Trace the Master Chief's visor and helmet from a frame of the game, for the VISR icon.

The input is a frame the Mac runner captured from the c40 cutscene, rendered from your own disc:
`c40-02820`, where the Chief faces the camera. The frame is not part of this repository. The script
segments three things in it:

- the gold visor: warm pixels, joined with a GrabCut seeded on the gold, then the convex hull;
- the helmet: a GrabCut inside a rectangle around the head;
- the lit lower rims of the two brow pads: blue-tinted pixels above the visor.

It then levels the head's slight roll (the visor's bottom edge should be level), mirrors each mask
about the visor's center line, since the model is symmetric, and fits paths: a smooth closed curve
for the visor and polygons for the helmet and rims. It prints them as the constants that
`tools/visr_icon.py` embeds, and writes an overlay of the paths on the leveled frame.

    python3 tools/visr_trace.py FRAME.png --overlay traced.png

Needs `opencv-python` and `numpy`.
"""
import argparse
import math
from pathlib import Path

import cv2
import numpy as np

CROP = (1000, 300, 1000, 750)    # x, y, width, height of the head in the 2732 x 2048 frame
VISOR_BOX = (40, 250, 640, 360)  # around the visor, in the crop
HELMET_BOX = (15, 100, 700, 650)  # around the helmet, in the crop
HELMET_WIDTH = 900               # the helmet's width in icon units


def largest(mask):
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
    return max(contours, key=cv2.contourArea)


def filled(contour, shape):
    out = np.zeros(shape, np.uint8)
    cv2.drawContours(out, [contour], -1, 255, -1)
    return out


def disk(size):
    return cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (size, size))


def visor_mask(img):
    pixels = img.astype(int)
    b, g, r = pixels[..., 0], pixels[..., 1], pixels[..., 2]
    x, y, w, h = VISOR_BOX
    # warm: the gold, and the brown of the dark reflections, against the blue-purple brow above
    warm = np.zeros(img.shape[:2], np.uint8)
    region = (r > b + 4) & (r >= g - 2) & (r > 25)
    warm[y:y + h, x:x + w] = region[y:y + h, x:x + w] * 255
    warm = cv2.morphologyEx(warm, cv2.MORPH_OPEN, disk(5))
    warm = cv2.morphologyEx(warm, cv2.MORPH_CLOSE, disk(25))
    warm = filled(largest(warm), warm.shape)
    # GrabCut seeded on the gold gives the clean sides and bottom
    hsv = cv2.cvtColor(img, cv2.COLOR_BGR2HSV).astype(int)
    hue, sat, val = hsv[..., 0], hsv[..., 1], hsv[..., 2]
    seed = np.full(img.shape[:2], cv2.GC_BGD, np.uint8)
    seed[y:y + h, x:x + w] = cv2.GC_PR_BGD
    seed[(hue >= 14) & (hue <= 32) & (sat > 120) & (val > 110) & (seed == cv2.GC_PR_BGD)] = cv2.GC_FGD
    cv2.grabCut(img, seed, None, np.zeros((1, 65)), np.zeros((1, 65)), 8, cv2.GC_INIT_WITH_MASK)
    cut = np.where((seed == cv2.GC_FGD) | (seed == cv2.GC_PR_FGD), 255, 0).astype(np.uint8)
    cut = cv2.morphologyEx(cut, cv2.MORPH_OPEN, disk(9))
    cut = filled(largest(cut), cut.shape)
    both = cv2.morphologyEx(warm | cut, cv2.MORPH_OPEN, disk(31))
    return filled(cv2.convexHull(largest(both)), both.shape)


def helmet_mask(img):
    seed = np.zeros(img.shape[:2], np.uint8)
    cv2.grabCut(img, seed, HELMET_BOX, np.zeros((1, 65)), np.zeros((1, 65)), 10, cv2.GC_INIT_WITH_RECT)
    cut = np.where((seed == cv2.GC_FGD) | (seed == cv2.GC_PR_FGD), 255, 0).astype(np.uint8)
    cut = cv2.morphologyEx(cut, cv2.MORPH_OPEN, disk(15))
    return filled(largest(cut), cut.shape)


def pad_rims(img, helmet, visor):
    pixels = img.astype(int)
    b, g, r = pixels[..., 0], pixels[..., 1], pixels[..., 2]
    rims = (b > g + 12) & (b > r - 5) & (b > 35) & (helmet > 0)
    rims[np.nonzero(visor)[0].min() + 40:] = False
    mask = rims.astype(np.uint8) * 255
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, disk(7))
    return cv2.morphologyEx(mask, cv2.MORPH_CLOSE, disk(21))


def level_and_mirror(img, visor, helmet):
    """rotate so the visor's bottom edge is level, then make each mask symmetric about the visor's center"""
    ys, xs = np.nonzero(visor)
    x0, x1 = xs.min(), xs.max()
    cols = range(int(x0 + 0.2 * (x1 - x0)), int(x1 - 0.2 * (x1 - x0)))
    slope, _ = np.polyfit(list(cols), [ys[xs == x].max() for x in cols], 1)
    height, width = visor.shape
    turn = cv2.getRotationMatrix2D(((x0 + x1) / 2, ys.mean()), math.degrees(math.atan(slope)), 1)
    img, visor, helmet = (cv2.warpAffine(m, turn, (width, height)) for m in (img, visor, helmet))
    xs = np.nonzero(visor > 127)[1]
    axis = (xs.min() + xs.max()) / 2

    def mirrored(m):
        flip = cv2.warpAffine(m, np.float32([[-1, 0, 2 * axis], [0, 1, 0]]), (width, height))
        return (((m.astype(float) + flip) / 2) > 127).astype(np.uint8) * 255
    visor, helmet = mirrored(visor), mirrored(helmet)
    rims = mirrored(pad_rims(img, helmet, visor)) & ~visor
    return img, visor, helmet, rims, axis


def resample(points, n):
    closed = np.vstack([points, points[:1]])
    t = np.concatenate([[0], np.cumsum(np.hypot(*np.diff(closed, axis=0).T))])
    u = np.linspace(0, t[-1], n, endpoint=False)
    return np.stack([np.interp(u, t, closed[:, 0]), np.interp(u, t, closed[:, 1])], 1)


def smooth(points, sigma):
    k = int(sigma * 3)
    weights = np.exp(-0.5 * (np.arange(-k, k + 1) / sigma) ** 2)
    weights /= weights.sum()
    padded = np.vstack([points[-k:], points, points[:k]])
    return np.stack([np.convolve(padded[:, i], weights, "valid") for i in (0, 1)], 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("frame", type=Path, help="the captured c40-02820 frame")
    parser.add_argument("--overlay", type=Path, help="write the traced paths over the leveled frame here")
    args = parser.parse_args()
    x, y, w, h = CROP
    img = np.ascontiguousarray(cv2.imread(str(args.frame))[y:y + h, x:x + w])
    img, visor, helmet, rims, axis = level_and_mirror(img, visor_mask(img), helmet_mask(img))

    # frame the icon: the helmet's width across, from its top to a little under the visor
    vy = np.nonzero(visor)[0]
    clip = vy.max() + 50
    helmet[clip:] = 0
    hy, hx = np.nonzero(helmet)
    scale = HELMET_WIDTH / (hx.max() - hx.min())
    cy = (hy.min() + clip) / 2

    def pt(p):
        return f"{(p[0] - axis) * scale:.0f} {(p[1] - cy) * scale:.0f}"

    def polygon(contour, epsilon):
        points = cv2.approxPolyDP(contour.astype(np.float32), epsilon, True)[:, 0, :]
        return "M" + "L".join(pt(p) for p in points) + "Z"

    # the visor: a closed Catmull-Rom curve through 32 points of the smoothed outline
    points = resample(smooth(resample(largest(visor)[:, 0, :].astype(float), 480), 6), 32)
    n = len(points)
    d = ["M" + pt(points[0])]
    for i in range(n):
        p0, p1, p2, p3 = points[i - 1], points[i], points[(i + 1) % n], points[(i + 2) % n]
        d.append("C" + " ".join(pt(p) for p in (p1 + (p2 - p0) / 6, p2 - (p3 - p1) / 6, p2)))
    rim_contours = sorted(cv2.findContours(rims, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)[0],
                          key=cv2.contourArea, reverse=True)[:2]
    print(f'VISOR_PATH = "{"".join(d)}Z"')
    print(f'HELMET_PATH = "{polygon(largest(helmet), 3.0)}"')
    print(f'BROW_RIMS = "{" ".join(polygon(c, 2.0) for c in rim_contours)}"')
    print(f"HELMET_CLIP_Y = {(clip - cy) * scale:.0f}")

    if args.overlay:
        out = img.copy()
        for mask, color in ((helmet, (0, 255, 255)), (rims, (0, 0, 255)), (visor, (255, 255, 0))):
            contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)
            cv2.drawContours(out, contours, -1, color, 2)
        cv2.imwrite(str(args.overlay), out)


if __name__ == "__main__":
    main()
