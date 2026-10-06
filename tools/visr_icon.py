#!/usr/bin/env python3
"""Draw the VISR app icon as SVG and render it into the asset catalogs.

The icon is original vector art: a dark backdrop with a faint grid, and a HUD
mark on top in two layers (middle and front), so visionOS and tvOS can float
the layers apart. Several concepts are drawn; `--concept` picks the one that
goes into the catalogs.

    python3 tools/visr_icon.py --catalogs                   # render the default concept into port/ios/Assets*.xcassets
    python3 tools/visr_icon.py --concept reticle --catalogs # render another concept
    python3 tools/visr_icon.py --previews DIR               # every concept's layers and mockups, and the comparison sheets

Needs `rsvg-convert` (librsvg) and `magick` (ImageMagick) on PATH.
"""
import argparse
import json
import math
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IOS = ROOT / "port/ios"
DEFAULT_CONCEPT = "reticle"

CYAN = "#3fd8ff"
ICE = "#e9fbff"
AMBER = "#ffb547"
GLOW = """<filter id="glow" x="-30%" y="-30%" width="160%" height="160%">
  <feGaussianBlur stdDeviation="{blur}" result="b"/>
  <feMerge><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
</filter>"""


def svg(width, height, body, blur=10):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
            f'viewBox="0 0 {width} {height}"><defs>{GLOW.format(blur=blur)}</defs>{body}</svg>')


def place(width, height, scale, x=None, y=None):
    """a transform that puts the 1024-unit mark, centered on its origin, at (x, y) at this scale"""
    x = width / 2 if x is None else x
    y = height / 2 if y is None else y
    return f'translate({x:.2f} {y:.2f}) scale({scale:.5f})'


def backdrop(width, height, scale):
    """the back layer: a radial gradient lit a little above center, and a faint grid"""
    step = 64 * scale
    lines = []
    x = width / 2 % step
    while x < width:
        lines.append(f'M{x:.2f} 0V{height}')
        x += step
    y = height / 2 % step
    while y < height:
        lines.append(f'M0 {y:.2f}H{width}')
        y += step
    radius = max(width, height) * 0.75
    return (f'<defs><radialGradient id="bg" cx="{width / 2}" cy="{height * 0.45}" r="{radius}" '
            f'gradientUnits="userSpaceOnUse"><stop offset="0" stop-color="#123a49"/>'
            f'<stop offset="0.45" stop-color="#08161e"/><stop offset="1" stop-color="#020507"/></radialGradient></defs>'
            f'<rect width="{width}" height="{height}" fill="url(#bg)"/>'
            f'<path d="{" ".join(lines)}" stroke="#2b6577" stroke-opacity="0.28" stroke-width="{max(1.0, 2 * scale):.2f}" fill="none"/>')


def arc(radius, start, end):
    """an SVG arc path, angles in degrees clockwise from 12 o'clock"""
    def point(angle):
        a = math.radians(angle - 90)
        return radius * math.cos(a), radius * math.sin(a)
    (x0, y0), (x1, y1) = point(start), point(end)
    large = 1 if (end - start) % 360 > 180 else 0
    return f'M{x0:.2f} {y0:.2f}A{radius} {radius} 0 {large} 1 {x1:.2f} {y1:.2f}'


# Letterforms for "VISR": chamfered strokes on a 300-unit cap height, drawn by hand so no font is needed.
CAP = 300
CHAMFER = 56
LETTERS = {
    "V": (170, [[(0, 0), (85, CAP), (170, 0)]]),
    "I": (0, [[(0, 0), (0, CAP)]]),
    "S": (150, [[(150, 0), (CHAMFER, 0), (0, CHAMFER), (0, CAP / 2 - CHAMFER / 2), (CHAMFER / 2, CAP / 2),
                 (150 - CHAMFER / 2, CAP / 2), (150, CAP / 2 + CHAMFER / 2), (150, CAP - CHAMFER),
                 (150 - CHAMFER, CAP), (0, CAP)]]),
    "R": (160, [[(0, CAP), (0, 0), (160 - CHAMFER, 0), (160, CHAMFER), (160, CAP / 2 - CHAMFER), (160 - CHAMFER, CAP / 2),
                 (0, CAP / 2)], ["butt", (64, CAP / 2), (160, CAP + 10)]]),
}


def wordmark(stroke=46, gap=84, color=ICE):
    """VISR centered on the origin, in mark units"""
    width = sum(LETTERS[c][0] for c in "VISR") + gap * 3
    x = -width / 2
    paths = {"square": [], "butt": []}
    for c in "VISR":
        advance, strokes = LETTERS[c]
        for points in strokes:
            # a stroke that starts inside another one has butt caps, so its cap does not poke out
            cap = points[0] if isinstance(points[0], str) else "square"
            points = [p for p in points if not isinstance(p, str)]
            paths[cap].append("M" + "L".join(f"{x + px:.1f} {py - CAP / 2:.1f}" for px, py in points))
        x += advance + gap
    return "".join(f'<path d="{" ".join(d)}" fill="none" stroke="{color}" stroke-width="{stroke}" '
                   f'stroke-linejoin="miter" stroke-miterlimit="2" stroke-linecap="{cap}"/>' for cap, d in paths.items())


def reticle_middle():
    ring = "".join(f'<path d="{arc(360, q + 14, q + 76)}"/>' for q in (0, 90, 180, 270))
    ticks = []
    for i in range(36):
        angle = i * 10
        if angle % 90 == 0:
            continue
        a = math.radians(angle - 90)
        inner, outer = (282, 318) if angle % 30 == 0 else (296, 314)
        ticks.append(f'M{inner * math.cos(a):.1f} {inner * math.sin(a):.1f}L{outer * math.cos(a):.1f} {outer * math.sin(a):.1f}')
    return (f'<g filter="url(#glow)"><g fill="none" stroke="{CYAN}" stroke-width="38" stroke-linecap="butt">{ring}</g>'
            f'<path d="{" ".join(ticks)}" stroke="{CYAN}" stroke-opacity="0.7" stroke-width="8"/></g>')


def reticle_front():
    hairs = "M0 -420V-205 M0 205V420 M-420 0H-205 M205 0H420"
    chevron = "M-92 70L0 -40L92 70"
    return (f'<g filter="url(#glow)"><path d="{hairs}" stroke="{ICE}" stroke-width="30"/>'
            f'<path d="{chevron}" fill="none" stroke="{ICE}" stroke-width="30" stroke-linejoin="miter"/>'
            f'<circle cx="0" cy="118" r="22" fill="{AMBER}"/></g>')


VISOR = "M-330 -150H330L420 -64V58L300 150H-300L-420 58V-64Z"


def visor_middle():
    return (f'<defs><linearGradient id="lens" x1="0" y1="-150" x2="0" y2="150" gradientUnits="userSpaceOnUse">'
            f'<stop offset="0" stop-color="#7fe9ff"/><stop offset="0.55" stop-color="#1aa6c9"/>'
            f'<stop offset="1" stop-color="#063f52"/></linearGradient></defs>'
            f'<g filter="url(#glow)"><path d="{VISOR}" fill="url(#lens)" stroke="{ICE}" stroke-width="14" stroke-linejoin="miter"/></g>')


def visor_front():
    shine = "M-250 -150H-120L-260 150H-390Z"
    bars = "".join(f'<rect x="{-360 + i * 34}" y="{86 - h}" width="20" height="{h}"/>' for i, h in enumerate((28, 46, 64, 40)))
    return (f'<path d="{shine}" fill="#ffffff" fill-opacity="0.22" clip-path="url(#lensclip)"/>'
            f'<defs><clipPath id="lensclip"><path d="{VISOR}"/></clipPath></defs>'
            f'<g filter="url(#glow)" stroke="{ICE}" fill="none">'
            f'<circle r="74" stroke-width="16"/><path d="M-26 0H-150 M26 0H150 M0 -26V-112 M0 26V112" stroke-width="12"/>'
            f'<g fill="{ICE}" stroke="none" fill-opacity="0.85">{bars}</g>'
            f'<circle cx="320" cy="-62" r="18" fill="{AMBER}" stroke="none"/></g>')


def wordmark_middle():
    corner = 120
    d = []
    for sx in (-1, 1):
        for sy in (-1, 1):
            x, y = sx * 400, sy * 330
            d.append(f'M{x:.0f} {y - sy * corner:.0f}L{x:.0f} {y:.0f}L{x - sx * corner:.0f} {y:.0f}')
    return (f'<g filter="url(#glow)"><path d="{" ".join(d)}" fill="none" stroke="{CYAN}" stroke-width="28"/>'
            f'<path d="M-300 230H300" stroke="{CYAN}" stroke-opacity="0.55" stroke-width="10"/>'
            f'<circle cx="0" cy="230" r="16" fill="{AMBER}"/></g>')


def wordmark_front():
    return f'<g filter="url(#glow)">{wordmark()}</g>'


# The Halo: CE visor and helmet, traced from a frame of the game's c40 cutscene rendered from the user's own disc
# (`c40-02820`, the Chief facing the camera). `tools/visr_trace.py` segments the gold visor, the helmet and the lit
# rims of the brow pads, levels the head's slight roll, mirrors each mask about the visor's center line (the model
# is symmetric), and fits these paths. Units are the 1024-unit mark: the helmet is 900 wide, the visor 710.
VISOR_PATH = "M-75 -76C-94 -76 -114 -72 -133 -70C-153 -68 -172 -65 -192 -63C-211 -61 -231 -59 -250 -56C-269 -53 -292 -55 -308 -46C-323 -37 -335 -19 -342 -2C-350 15 -352 36 -354 55C-356 75 -357 95 -356 114C-354 133 -353 154 -346 172C-339 189 -325 205 -313 220C-302 236 -291 255 -275 265C-259 274 -238 276 -219 278C-200 281 -180 279 -160 279C-141 279 -121 279 -101 279C-82 279 -62 279 -43 279C-23 279 -3 279 16 279C36 279 55 279 75 279C94 279 114 279 134 279C153 279 173 279 192 279C212 278 234 282 251 275C269 269 284 255 297 242C311 228 323 211 332 194C342 178 350 159 354 141C358 122 356 101 355 82C355 62 354 42 349 24C345 5 341 -19 329 -31C317 -44 295 -48 276 -53C258 -57 237 -58 218 -60C198 -62 179 -65 160 -67C140 -69 121 -73 101 -74C82 -75 62 -75 43 -75C23 -75 3 -73 -16 -73C-36 -73 -55 -77 -75 -76Z"
HELMET_PATH = "M-340 -266L-415 -146L-418 -135L-411 -113L-442 -67L-406 31L-406 155L-372 205L-438 240L-450 343L450 343L438 240L372 205L406 155L406 31L442 -67L411 -113L418 -135L415 -146L338 -267L269 -272L247 -303L82 -344L15 -338L-82 -344L-247 -303L-269 -272L-329 -271Z"
BROW_RIMS = "M129 -271L128 -258L123 -250L124 -209L119 -163L123 -144L134 -139L138 -119L150 -118L155 -122L152 -145L162 -209L180 -223L195 -225L257 -216L305 -203L320 -190L331 -172L349 -153L386 -123L390 -110L397 -105L397 -96L420 -90L422 -100L411 -114L418 -141L338 -267L329 -271L143 -279Z M-129 -271L-143 -279L-329 -271L-338 -267L-418 -141L-411 -114L-422 -100L-420 -90L-397 -96L-397 -105L-390 -110L-386 -123L-349 -153L-331 -172L-320 -190L-305 -203L-257 -216L-195 -225L-180 -223L-162 -209L-152 -145L-155 -122L-150 -118L-138 -119L-134 -139L-123 -144L-119 -163L-124 -209L-123 -250L-128 -258Z"
HELMET_CLIP_Y = 344


GLASS_TINTS = {
    "cyan": (("#8af0ff", 0.55), ("#1aa6c9", 0.30), ("#05394a", 0.20)),
    "gold": (("#fff0b0", 0.95), ("#e0a53a", 0.92), ("#5a3a0c", 0.95)),
}


def visor_defs(view, visor, tint="cyan"):
    """gradients and clips shared by the traced variants"""
    (c0, o0), (c1, o1), (c2, o2) = GLASS_TINTS[tint]
    rim = ("#fff6d8", "#f2c35a") if tint == "gold" else (ICE, CYAN)
    return (f'<defs><clipPath id="visorclip-{view}"><path d="{visor}"/></clipPath>'
            f'<radialGradient id="glass-{view}" cx="0.42" cy="0.78" r="0.75" fx="0.38" fy="0.85">'
            f'<stop offset="0" stop-color="{c0}" stop-opacity="{o0}"/><stop offset="0.5" stop-color="{c1}" stop-opacity="{o1}"/>'
            f'<stop offset="1" stop-color="{c2}" stop-opacity="{o2}"/></radialGradient>'
            f'<linearGradient id="edge-{view}" x1="0" y1="0" x2="1" y2="1">'
            f'<stop offset="0" stop-color="{rim[0]}"/><stop offset="0.45" stop-color="{rim[1]}"/>'
            f'<stop offset="1" stop-color="{rim[1]}" stop-opacity="0.35"/></linearGradient></defs>')


def specular(view, visor, strength=1.0):
    """curved highlights, as on a convex glass: a wide arc under the top edge and a short bright one on the
    upper left, clipped to the glass"""
    return (f'<defs><linearGradient id="spec-{view}" x1="0" y1="0" x2="1" y2="0">'
            f'<stop offset="0" stop-color="#ffffff" stop-opacity="{0.15 * strength:.2f}"/>'
            f'<stop offset="0.3" stop-color="#ffffff" stop-opacity="{0.8 * strength:.2f}"/>'
            f'<stop offset="0.7" stop-color="#ffffff" stop-opacity="{0.35 * strength:.2f}"/>'
            f'<stop offset="1" stop-color="#ffffff" stop-opacity="0"/></linearGradient></defs>'
            f'<g clip-path="url(#visorclip-{view})" fill="none" stroke="url(#spec-{view})" stroke-linecap="round">'
            f'<ellipse cx="0" cy="330" rx="560" ry="372" stroke-width="22"/>'
            f'<path d="M-300 70Q-250 -10 -150 -22" stroke="#ffffff" stroke-opacity="{0.85 * strength:.2f}" stroke-width="20"/></g>')


def visor_glass(view, visor, tint="cyan"):
    return visor_defs(view, visor, tint) + f'<path d="{visor}" fill="url(#glass-{view})"/>'


def visor_edge(view, visor, width=26):
    """the visor's rim: bright where it catches the light at the upper left, fading to the lower right"""
    return (f'<g filter="url(#glow)"><path d="{visor}" fill="none" stroke="url(#edge-{view})" stroke-width="{width}" '
            f'stroke-linejoin="round"/></g>')


def helmet_shell(view, helmet, clip_y):
    """the helmet as a dark shape barely lighter than the backdrop, with a cyan rim light along its upper left edge"""
    return (f'<defs><linearGradient id="shell-{view}" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="#16343f"/><stop offset="1" stop-color="#081820"/></linearGradient>'
            f'<linearGradient id="fade-{view}" x1="0" y1="-500" x2="0" y2="{clip_y}" gradientUnits="userSpaceOnUse">'
            f'<stop offset="0.75" stop-color="#fff"/><stop offset="1" stop-color="#000"/></linearGradient>'
            f'<mask id="shellmask-{view}" maskUnits="userSpaceOnUse" x="-512" y="-512" width="1024" height="1024">'
            f'<rect x="-512" y="-512" width="1024" height="1024" fill="url(#fade-{view})"/></mask>'
            f'<linearGradient id="rim-{view}" x1="0" y1="0" x2="1" y2="1">'
            f'<stop offset="0" stop-color="{CYAN}" stop-opacity="0.9"/><stop offset="0.6" stop-color="{CYAN}" stop-opacity="0.15"/>'
            f'<stop offset="1" stop-color="{CYAN}" stop-opacity="0"/></linearGradient></defs>'
            f'<g mask="url(#shellmask-{view})"><path d="{helmet}" fill="url(#shell-{view})"/>'
            f'<g filter="url(#glow)"><path d="{helmet}" fill="none" stroke="url(#rim-{view})" stroke-width="12" stroke-linejoin="round"/></g></g>')


def hud_reflection(cx=0, cy=0, r=60, opacity=0.6):
    ring = "".join(f'<path d="{arc(r, q + 20, q + 70)}" transform="translate({cx} {cy})"/>' for q in (0, 90, 180, 270))
    hairs = (f"M{cx} {cy - r * 1.6}V{cy - r * 1.25} M{cx} {cy + r * 1.25}V{cy + r * 1.6} "
             f"M{cx - r * 1.6} {cy}H{cx - r * 1.25} M{cx + r * 1.25} {cy}H{cx + r * 1.6}")
    return (f'<g filter="url(#glow)" fill="none" stroke="{CYAN}" stroke-opacity="{opacity}" stroke-width="{r / 5:.1f}">'
            f'{ring}<path d="{hairs}"/></g><circle cx="{cx}" cy="{cy}" r="{r / 3.5:.1f}" fill="{AMBER}"/>')


def brow_rims(opacity=0.85):
    """the lit lower rims of the two brow pads above the visor"""
    return f'<g filter="url(#glow)"><path d="{BROW_RIMS}" fill="{CYAN}" fill-opacity="{opacity}"/></g>'


# visor-traced: the visor alone, enlarged and centered
TRACED_ALONE = 'transform="scale(1.2) translate(0 -103)"'


def visor_traced_middle():
    return f'<g {TRACED_ALONE}>{visor_glass("f", VISOR_PATH)}</g>'


def visor_traced_front():
    return (f'<g {TRACED_ALONE}>{visor_defs("f2", VISOR_PATH)}{specular("f2", VISOR_PATH)}'
            f'{visor_edge("f2", VISOR_PATH, 22)}</g>')


# visor-helmet: the visor set in the helmet: a dark shell with a rim light, and the brow pads' lit edges
HELMET_VIEW = 'transform="scale(0.94)"'


def visor_helmet_middle():
    return f'<g {HELMET_VIEW}>{helmet_shell("h", HELMET_PATH, HELMET_CLIP_Y)}{brow_rims()}{visor_glass("h", VISOR_PATH)}</g>'


def visor_helmet_front():
    return (f'<g {HELMET_VIEW}>{visor_defs("h2", VISOR_PATH)}{specular("h2", VISOR_PATH)}'
            f'{visor_edge("h2", VISOR_PATH, 16)}</g>')


def visor_helmet_gold_middle():
    return f'<g {HELMET_VIEW}>{helmet_shell("g", HELMET_PATH, HELMET_CLIP_Y)}{brow_rims()}{visor_glass("g", VISOR_PATH, "gold")}</g>'


def visor_helmet_gold_front():
    return (f'<g {HELMET_VIEW}>{visor_defs("g2", VISOR_PATH, "gold")}{specular("g2", VISOR_PATH)}'
            f'{visor_edge("g2", VISOR_PATH, 12)}</g>')


# visor-closeup: a close crop of the helmet, so the gold glass fills the icon, with the brow pads at the top and
# a cyan HUD reticle reflected in the glass
CLOSE = 'transform="scale(1.3) translate(0 -70)"'


def visor_closeup_middle():
    return f'<g {CLOSE}>{helmet_shell("c", HELMET_PATH, HELMET_CLIP_Y)}{brow_rims()}{visor_glass("c", VISOR_PATH, "gold")}</g>'


def visor_closeup_front():
    return (f'<g {CLOSE}>{visor_defs("c2", VISOR_PATH, "gold")}{specular("c2", VISOR_PATH)}'
            f'<g clip-path="url(#visorclip-c2)">{hud_reflection(110, 120, 40, 0.9)}</g>{visor_edge("c2", VISOR_PATH, 10)}</g>')


# The inside view. Halo: CE draws no visor frame in first person (its HUD bitmaps are reticles, meters and the motion
# tracker, nothing around the view), so the frame here is derived from the traced visor itself: `inside_projection`
# puts the traced outline on a glass that wraps around the eye (curving back toward the face at the sides and below)
# and projects it from an eye just behind the upper half of the glass. The outline is symmetric, so mirroring it for
# the view from behind changes nothing. Under that perspective the top edge rises toward the corners, which leaves a
# dip at the top center where the brow comes down, and the bottom edge rises at the center, where the chin sits.
# APERTURE_HALF is that projected outline simplified to straight runs with chamfered corners, with the brow notch and
# the chin piece squared off; units have the aperture 1000 wide, and `aperture_path` scales it into the mark.
EYE_DISTANCE, WRAP_RADIUS, TUCK_RADIUS, EYE_HEIGHT = 300, 500, 600, 100


def bezier_points(path, steps=16):
    """sample an absolute M/C/Z path into points"""
    import re
    tokens = re.findall(r"[MCZ]|-?\d+(?:\.\d+)?", path)
    points, i, current = [], 0, (0.0, 0.0)
    while i < len(tokens):
        if tokens[i] == "M":
            current = (float(tokens[i + 1]), float(tokens[i + 2]))
            points.append(current)
            i += 3
        elif tokens[i] == "C":
            (x1, y1, x2, y2, x3, y3) = (float(t) for t in tokens[i + 1:i + 7])
            x0, y0 = current
            for k in range(1, steps + 1):
                s = k / steps
                a, b, c, d = (1 - s) ** 3, 3 * (1 - s) ** 2 * s, 3 * (1 - s) * s * s, s ** 3
                points.append((a * x0 + b * x1 + c * x2 + d * x3, a * y0 + b * y1 + c * y2 + d * y3))
            current = (x3, y3)
            i += 7
        else:
            i += 1
    return points


def inside_projection():
    """the traced visor outline as the eye inside sees it, scaled to 1000 wide and centered"""
    projected = []
    for x, y in bezier_points(VISOR_PATH):
        depth = EYE_DISTANCE - x * x / (2 * WRAP_RADIUS) - (y - EYE_HEIGHT) ** 2 / (2 * TUCK_RADIUS)
        projected.append((x / depth, (y - EYE_HEIGHT) / depth))
    xs, ys = [p[0] for p in projected], [p[1] for p in projected]
    scale = 1000 / (max(xs) - min(xs))
    cx, cy = (max(xs) + min(xs)) / 2, (max(ys) + min(ys)) / 2
    return [((x - cx) * scale, (y - cy) * scale) for x, y in projected]


# right half, from the top center clockwise to the bottom center. The chin piece rises less than the projection
# says (42 units, not 58), so the silhouette does not pinch into a bow tie at small sizes.
APERTURE_HALF = ((0, -150), (62, -150), (98, -178), (430, -194), (482, -146), (500, 22), (452, 110), (322, 192),
                 (156, 180), (112, 150), (0, 150))


def aperture_path(sx=0.87, sy=1.29):
    """the aperture in mark units: 870 wide, so the shell shows on every side, inside the visionOS circle too"""
    right = [(x * sx, y * sy) for x, y in APERTURE_HALF]
    left = [(-x, y) for x, y in reversed(right[1:-1])]
    return "M" + "L".join(f"{x:.1f} {y:.1f}" for x, y in right + left) + "Z"


APERTURE = aperture_path()
SHELL = f"M-4000 -4000H4000V4000H-4000Z {APERTURE}"


def inside_view_back(width, height, scale, dusk=False):
    """the world seen through the visor, bright against the dark shell: a sky over a soft horizon band, with the
    ring arcing through it drawn in the middle layer; `dusk` warms the horizon"""
    horizon = 0.58
    glow = "#ffd9a0" if dusk else "#cfeeff"
    return (f'<defs><linearGradient id="world" x1="0" y1="0" x2="0" y2="{height}" gradientUnits="userSpaceOnUse">'
            f'<stop offset="0" stop-color="#9fd6ff"/><stop offset="{horizon - 0.06}" stop-color="#2a7fa8"/>'
            f'<stop offset="{horizon}" stop-color="{glow}" stop-opacity="0.9"/><stop offset="{horizon + 0.07}" stop-color="#1f5b78"/>'
            f'<stop offset="1" stop-color="#0b2a3a"/></linearGradient></defs>'
            f'<rect width="{width}" height="{height}" fill="url(#world)"/>')


def ring_arc(opacity=0.55, width=5):
    """the ring: a thin arc rising out of the horizon at both sides, its apex at 30% of the height"""
    return (f'<path d="M-560 78A600 300 0 0 1 560 78" fill="none" stroke="#ffffff" stroke-opacity="{opacity}" '
            f'stroke-width="{width}" transform="rotate(-6)"/>')


def helmet_interior(rim=CYAN):
    """the helmet's inner shell as a dark mass around the aperture, and a lit lip at the aperture's edge whose glow
    falls only on the shell, with a fine bright line on its inner edge"""
    return (f'<defs><linearGradient id="shellfill" x1="0" y1="-512" x2="0" y2="512" gradientUnits="userSpaceOnUse">'
            f'<stop offset="0" stop-color="#0e1a22"/><stop offset="1" stop-color="#050a0d"/></linearGradient>'
            f'<clipPath id="shellclip"><path d="{SHELL}" clip-rule="evenodd"/></clipPath>'
            f'<clipPath id="apclip"><path d="{APERTURE}"/></clipPath>'
            f'<filter id="lipglow" x="-20%" y="-20%" width="140%" height="140%"><feGaussianBlur stdDeviation="12"/></filter></defs>'
            f'<path d="{SHELL}" fill="url(#shellfill)" fill-rule="evenodd"/>'
            # seams where the brow and chin pieces meet the shell
            f'<path d="M-54 -237L-80 -272H80L54 -237 M-97 231L-118 268H118L97 231" fill="none" stroke="#1d3540" stroke-width="6"/>'
            f'<g clip-path="url(#shellclip)"><path d="{APERTURE}" fill="none" stroke="{rim}" stroke-opacity="0.7" '
            f'stroke-width="30" filter="url(#lipglow)"/></g>'
            # a hairline 9 wide inside the glass, then the 14-wide lip over all but its inner 2
            f'<g clip-path="url(#apclip)"><path d="{APERTURE}" fill="none" stroke="{ICE}" stroke-opacity="0.7" '
            f'stroke-width="18" stroke-linejoin="miter"/></g>'
            f'<path d="{APERTURE}" fill="none" stroke="{rim}" stroke-width="14" stroke-linejoin="miter"/>')


def hud_guides(ladders=False, opacity=0.55, color="#cfeeff"):
    """thin guide lines inside the aperture: brackets that follow its top and bottom edges, a tick pair at the top
    and bottom center, and optionally short scale ladders at the sides"""
    top = "M-350 -214L-108 -201L-78 -168H78L108 -201L350 -214"
    bottom = "M-260 206L-145 214L-108 160H108L145 214L260 206"
    ticks = "M-48 -138H48 M-24 -118H24 M-48 132H48 M-24 112H24"
    d = [top, bottom, ticks]
    if ladders:
        for sx in (-1, 1):
            x = sx * 330
            d += [f"M{x} {y}H{x - sx * (24 if i % 2 else 44)}" for i, y in enumerate(range(-90, 91, 30))]
    return (f'<path d="{" ".join(d)}" fill="none" stroke="{color}" stroke-opacity="{opacity}" stroke-width="6" '
            f'stroke-linejoin="miter"/>')


def inside_frame_middle():
    return ring_arc()


def inside_frame_front():
    return helmet_interior()


def inside_hud_middle():
    return ring_arc() + hud_guides()


def inside_ladders_middle():
    return ring_arc() + hud_guides(ladders=True)


def inside_contact_middle():
    """the guides, and one amber contact low at the left, as on the motion tracker"""
    return ring_arc() + hud_guides() + f'<circle cx="-300" cy="150" r="16" fill="{AMBER}"/>'


def inside_dusk_middle():
    return ring_arc(0.8, 8)


INSIDE_BACKS = {
    "inside-frame": lambda w, h, s: inside_view_back(w, h, s),
    "inside-hud": lambda w, h, s: inside_view_back(w, h, s),
    "inside-ladders": lambda w, h, s: inside_view_back(w, h, s),
    "inside-contact": lambda w, h, s: inside_view_back(w, h, s),
    "inside-dusk": lambda w, h, s: inside_view_back(w, h, s, dusk=True),
}
# the detailed concepts fall back to the bare frame at 60 px and below, where their guide lines would be fuzz
SMALL = {"inside-hud": "inside-frame", "inside-ladders": "inside-frame", "inside-contact": "inside-frame"}
SMALL_MAX = 60


def inside_derivation(out):
    """a diagram: the traced front view of the visor beside its projection from inside, and the simplified aperture"""
    front = bezier_points(VISOR_PATH)
    fx = [p[0] for p in front]
    fy = [p[1] for p in front]
    k = 600 / (max(fx) - min(fx))
    fcx, fcy = (max(fx) + min(fx)) / 2, (max(fy) + min(fy)) / 2
    front_d = "M" + "L".join(f"{(x - fcx) * k:.1f} {(y - fcy) * k:.1f}" for x, y in front) + "Z"
    proj_d = "M" + "L".join(f"{x * 0.6:.1f} {y * 0.6:.1f}" for x, y in inside_projection()) + "Z"
    ap = "M" + "L".join(f"{x * 0.6:.1f} {y * 0.6:.1f}" for x, y in
                        [*APERTURE_HALF, *[(-x, y) for x, y in reversed(APERTURE_HALF[1:-1])]]) + "Z"
    label = 'fill="#c8d4da" font-family="Menlo" font-size="22"'
    body = (f'<rect width="1460" height="360" fill="#0b1216"/>'
            f'<g transform="translate(360 170)"><path d="{front_d}" fill="none" stroke="{CYAN}" stroke-width="4"/></g>'
            f'<text x="40" y="340" {label}>traced, from outside</text>'
            f'<g transform="translate(1080 170)"><path d="{proj_d}" fill="none" stroke="{CYAN}" stroke-opacity="0.5" stroke-width="4"/>'
            f'<path d="{ap}" fill="none" stroke="{ICE}" stroke-width="3"/></g>'
            f'<text x="760" y="340" {label}>from inside (cyan) and simplified (white)</text>')
    render(f'<svg xmlns="http://www.w3.org/2000/svg" width="1460" height="360">{body}</svg>', out, 1460, 360)


CONCEPTS = {
    "reticle": ("A broken reticle ring with tick marks, crosshairs and a chevron; amber center pip.",
                reticle_middle, reticle_front),
    "visor": ("An abstract octagonal visor lens in cyan glass with a HUD reticle and meter bars.",
              visor_middle, visor_front),
    "wordmark": ("The letters VISR in chamfered strokes inside HUD corner brackets.",
                 wordmark_middle, wordmark_front),
    "visor-traced": ("The Halo: CE visor traced front-on from the game, as cyan glass with a lit rim and a specular streak.",
                     visor_traced_middle, visor_traced_front),
    "visor-helmet": ("The traced visor in cyan glass, set in a dark helmet shell with a cyan rim light.",
                     visor_helmet_middle, visor_helmet_front),
    "visor-helmet-gold": ("The traced visor in gold glass, set in the dark helmet shell with a cyan rim light.",
                          visor_helmet_gold_middle, visor_helmet_gold_front),
    "visor-closeup": ("A close crop of the traced helmet: gold visor glass filling the icon, a cyan HUD reticle reflected in it.",
                      visor_closeup_middle, visor_closeup_front),
    "inside-frame": ("From inside the helmet: the CE visor's aperture, a lit lip in the dark inner shell, a bright sky "
                     "and the ring's thin arc beyond it.", inside_frame_middle, inside_frame_front),
    "inside-hud": ("inside-frame with thin HUD guides: brackets along the top and bottom edges and center tick pairs; "
                   "the bare frame at 60 px and below.", inside_hud_middle, inside_frame_front),
    "inside-ladders": ("inside-hud with short scale ladders at the sides.", inside_ladders_middle, inside_frame_front),
    "inside-contact": ("inside-hud with one amber motion-tracker contact low at the left.", inside_contact_middle,
                       inside_frame_front),
    "inside-dusk": ("inside-frame at dusk: a warm horizon and a brighter ring.", inside_dusk_middle, inside_frame_front),
}


def layer_svgs(concept, width, height, mark_scale, x=None, y=None, with_wordmark=False):
    """(back, middle, front) SVG documents for one canvas"""
    _, middle, front = CONCEPTS[concept]
    t = place(width, height, mark_scale, x, y)
    blur = 10
    back_body = INSIDE_BACKS[concept](width, height, mark_scale) if concept in INSIDE_BACKS else backdrop(width, height, mark_scale)
    back = svg(width, height, back_body, blur)
    mid = svg(width, height, f'<g transform="{t}">{middle()}</g>', blur)
    extra = ""
    if with_wordmark:
        # top shelf: the word beside the mark
        wx = width * 0.66
        extra = f'<g transform="{place(width, height, mark_scale * 1.4, wx, height / 2)}"><g filter="url(#glow)">{wordmark()}</g></g>'
    fr = svg(width, height, f'<g transform="{t}">{front()}</g>{extra}', blur)
    return back, mid, fr


def flat_svg(concept, width, height, mark_scale, **kwargs):
    back, mid, fr = layer_svgs(concept, width, height, mark_scale, **kwargs)

    def inner(doc):
        return doc[doc.index("</defs>") + len("</defs>"):-len("</svg>")]
    return svg(width, height, inner(back) + inner(mid) + inner(fr))


def render(document, out, width, height, opaque=False):
    with tempfile.NamedTemporaryFile("w", suffix=".svg", delete=False) as f:
        f.write(document)
    subprocess.run(["rsvg-convert", "--width", str(width), "--height", str(height), "--output", str(out), f.name], check=True)
    Path(f.name).unlink()
    if opaque:
        subprocess.run(["magick", str(out), "-background", "black", "-alpha", "remove", "-alpha", "off",
                        # no date chunks, so a rerun writes the same bytes
                        "-define", "png:exclude-chunks=date,time", f"PNG24:{out}"], check=True)


def write_contents(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")


INFO = {"author": "xcode", "version": 1}
IOS_SIZES = (20, 29, 40, 58, 60, 76, 80, 87, 120, 152, 167, 180, 1024)


def ios_catalog(concept):
    folder = IOS / "Assets.xcassets/AppIcon.appiconset"
    for size in IOS_SIZES:
        source = SMALL.get(concept, concept) if size <= SMALL_MAX else concept
        render(flat_svg(source, 1024, 1024, 1.0), folder / f"AppIcon-{size}.png", size, size, opaque=True)


def stack_layers(folder, concept, names, sizes, ext, idiom, mark_fraction):
    """write a layered stack: names like ("Front", "Middle", "Back"), sizes as [(scale, width, height)]"""
    write_contents(folder / "Contents.json", {"info": INFO, "layers": [{"filename": f"{n}.{ext}"} for n in names]})
    for name in names:
        imageset = folder / f"{name}.{ext}/Content.imageset"
        if imageset.parent.exists():
            shutil.rmtree(imageset.parent)
        write_contents(imageset.parent / "Contents.json", {"info": INFO})
        imageset.mkdir()
        images = []
        for scale, width, height in sizes:
            # visionOS layers name their file after the layer; tvOS layers by scale
            filename = f"{name}.png" if idiom == "vision" else f"Content@{scale}x.png"
            back, mid, fr = layer_svgs(concept, width, height, height * mark_fraction / 1024)
            doc = {"Back": back, "Middle": mid, "Front": fr}[name]
            render(doc, imageset / filename, width, height, opaque=name == "Back")
            images.append({"filename": filename, "idiom": idiom, "scale": f"{scale}x"})
        write_contents(imageset / "Contents.json", {"images": images, "info": INFO})


def tv_catalog(concept):
    brand = IOS / "Assets-tvOS.xcassets/AppIcon.brandassets"
    names = ("Front", "Middle", "Back")
    stack_layers(brand / "App Icon.imagestack", concept, names, [(1, 400, 240), (2, 800, 480)], "imagestacklayer", "tv", 0.86)
    stack_layers(brand / "App Icon - App Store.imagestack", concept, names, [(1, 1280, 768)], "imagestacklayer", "tv", 0.86)
    for name, width in (("Top Shelf Image", 1920), ("Top Shelf Image Wide", 2320)):
        for scale in (1, 2):
            w, h = width * scale, 720 * scale
            render(flat_svg(concept, w, h, h * 0.78 / 1024, x=w * 0.3, with_wordmark=True),
                   brand / f"{name}.imageset/{name}@{scale}x.png", w, h, opaque=True)


def vision_catalog(concept):
    stack = IOS / "Assets-visionOS.xcassets/AppIcon.solidimagestack"
    stack_layers(stack, concept, ("Front", "Middle", "Back"), [(2, 1024, 1024)], "solidimagestacklayer", "vision", 1.0)


LABEL_FONT = "/System/Library/Fonts/Menlo.ttc"  # ImageMagick has no default font here
SHEETS = {
    # the first three concepts, each as iOS, visionOS, and 120 and 60 px Home Screen sizes
    "concepts.png": (("reticle", "visor", "wordmark"), (("ios", 360), ("visionos", 360), ("ios", 120), ("ios", 60)), False),
    # the visor outlines beside the reticle, down to the 40 and 29 px Settings and Spotlight sizes
    "concepts-visor-2.png": (("reticle", "visor-traced", "visor-helmet", "visor-helmet-gold", "visor-closeup"),
                           (("ios", 360), ("visionos", 360), ("ios", 180), ("ios", 40), ("ios", 29)), True),
    # the view from inside the helmet, out through the visor's aperture, down to the 60, 40 and 29 px sizes
    "concepts-visor-3.png": (("inside-frame", "inside-hud", "inside-ladders", "inside-contact", "inside-dusk"),
                           (("ios", 360), ("visionos", 360), ("ios", 180), ("ios", 60), ("ios", 40), ("ios", 29)), True),
}


def previews(out):
    out.mkdir(parents=True, exist_ok=True)
    for concept, (description, _, _) in CONCEPTS.items():
        back, mid, fr = layer_svgs(concept, 1024, 1024, 1.0)
        for name, doc in (("back", back), ("middle", mid), ("front", fr)):
            (out / f"{concept}-{name}.svg").write_text(doc)
            render(doc, out / f"{concept}-{name}.png", 1024, 1024)
        flat = out / f"{concept}-flat.png"
        render(flat_svg(concept, 1024, 1024, 1.0), flat, 1024, 1024, opaque=True)
        # mockups: the iOS rounded square and the visionOS circle, next to a 60-point size
        subprocess.run(["magick", str(flat), "(", "+clone", "-alpha", "extract", "-fill", "black", "-colorize", "100",
                        "-fill", "white", "-draw", "roundrectangle 0,0 1023,1023 230,230", ")", "-alpha", "off",
                        "-compose", "CopyOpacity", "-composite", str(out / f"{concept}-ios.png")], check=True)
        subprocess.run(["magick", str(flat), "(", "+clone", "-fill", "black", "-colorize", "100",
                        "-fill", "white", "-draw", "circle 512,512 512,0", ")", "-alpha", "off",
                        "-compose", "CopyOpacity", "-composite", str(out / f"{concept}-visionos.png")], check=True)
        (out / f"{concept}.txt").write_text(description + "\n")
    for sheet, (concepts, cells, labeled) in SHEETS.items():
        rows = []
        if sheet == "concepts-visor-3.png":
            # first, where the aperture comes from
            diagram = out / "inside-derivation.png"
            inside_derivation(diagram)
            row = out / "derivation-row.png"
            subprocess.run(["magick", "-background", "#202428", "(", "-font", LABEL_FONT, "-fill", "#c8d4da", "-pointsize", "26",
                            "label:derivation", "-gravity", "west", "-extent", "300x", ")", str(diagram),
                            "-bordercolor", "#202428", "-border", "12", "-gravity", "center", "+append", str(row)], check=True)
            rows.append(str(row))
        for concept in concepts:
            row = out / f"{concept}-row.png"
            label = ["(", "-font", LABEL_FONT, "-fill", "#c8d4da", "-pointsize", "26", f"label:{concept}",
                     "-gravity", "west", "-extent", "300x", ")"] if labeled else []
            images = [arg for kind, size in cells
                      for arg in ("(", str(out / f"{SMALL.get(concept, concept) if size <= SMALL_MAX else concept}-{kind}.png"),
                                  "-resize", f"{size}x{size}", ")")]
            subprocess.run(["magick", "-background", "#202428", *label, *images,
                            "-bordercolor", "#202428", "-border", "12", "-gravity", "center", "+append", str(row)], check=True)
            rows.append(str(row))
        subprocess.run(["magick", "-background", "#202428", *rows, "-gravity", "west", "-append",
                        "-bordercolor", "#202428", "-border", "24", str(out / sheet)], check=True)
        for row in rows:
            Path(row).unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--concept", choices=sorted(CONCEPTS), default=DEFAULT_CONCEPT)
    parser.add_argument("--catalogs", action="store_true", help="render the concept into the iOS, tvOS and visionOS catalogs")
    parser.add_argument("--previews", type=Path, help="write every concept's layers and mockups into this folder")
    args = parser.parse_args()
    if not (args.catalogs or args.previews):
        parser.error("nothing to do: pass --catalogs or --previews")
    if args.previews:
        previews(args.previews)
    if args.catalogs:
        ios_catalog(args.concept)
        tv_catalog(args.concept)
        vision_catalog(args.concept)


if __name__ == "__main__":
    main()
