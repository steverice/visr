#!/usr/bin/env python3
"""Draw the VISR app icon as SVG and render it into the asset catalogs.

The icon is original vector art: a dark backdrop with a faint grid, and a HUD
mark on top in two layers (middle and front), so visionOS and tvOS can float
the layers apart. Three concepts are drawn; `--concept` picks the one that goes
into the catalogs.

    python3 tools/visr_icon.py --catalogs                 # render the default concept into port/ios/Assets*.xcassets
    python3 tools/visr_icon.py --concept visor --catalogs # render another concept
    python3 tools/visr_icon.py --previews DIR             # every concept: layers, a Home Screen mockup, a visionOS mockup

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


CONCEPTS = {
    "reticle": ("A broken reticle ring with tick marks, crosshairs and a chevron; amber center pip.",
                reticle_middle, reticle_front),
    "visor": ("An abstract octagonal visor lens in cyan glass with a HUD reticle and meter bars.",
              visor_middle, visor_front),
    "wordmark": ("The letters VISR in chamfered strokes inside HUD corner brackets.",
                 wordmark_middle, wordmark_front),
}


def layer_svgs(concept, width, height, mark_scale, x=None, y=None, with_wordmark=False):
    """(back, middle, front) SVG documents for one canvas"""
    _, middle, front = CONCEPTS[concept]
    t = place(width, height, mark_scale, x, y)
    blur = 10
    back = svg(width, height, backdrop(width, height, mark_scale), blur)
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
        render(flat_svg(concept, 1024, 1024, 1.0), folder / f"AppIcon-{size}.png", size, size, opaque=True)


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
    # one sheet: each concept as iOS, visionOS, and 120 and 60 px Home Screen sizes
    rows = []
    for concept in CONCEPTS:
        row = out / f"{concept}-row.png"
        subprocess.run(["magick", "-background", "#202428",
                        "(", str(out / f"{concept}-ios.png"), "-resize", "360x360", ")",
                        "(", str(out / f"{concept}-visionos.png"), "-resize", "360x360", ")",
                        "(", str(out / f"{concept}-ios.png"), "-resize", "120x120", ")",
                        "(", str(out / f"{concept}-ios.png"), "-resize", "60x60", ")",
                        "-bordercolor", "#202428", "-border", "12", "-gravity", "center", "+append", str(row)], check=True)
        rows.append(str(row))
    subprocess.run(["magick", "-background", "#202428", *rows, "-gravity", "west", "-append",
                    "-bordercolor", "#202428", "-border", "24", str(out / "concepts.png")], check=True)
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
