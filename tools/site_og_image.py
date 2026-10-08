#!/usr/bin/env python3
"""Draw the GitHub Pages site's Open Graph image (docs/site/og.png): the app icon beside the VISR
wordmark, as the page's header shows them, in the page's dark colors.

    python3 tools/site_og_image.py [--icon PNG] [--font FONT] [--weight N] [--text TEXT] [--out PNG]

The defaults are the 1024 px app icon, the site's wordmark font (docs/site/fonts) at the h1's bold
weight, and docs/site/og.png. Sizes follow docs/site/style.css's .brand rules (a 180 px icon, a 4.5rem
wordmark, a 24 px gap, 0.06em letter spacing), scaled so the icon is 330 px on a 1200x630 card.
Rerun it when the icon, the wordmark's font or the page's dark colors change.

Needs Pillow (pip install pillow); it reads WOFF2 fonts directly."""
import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = Path(__file__).resolve().parent.parent
WIDTH, HEIGHT = 1200, 630
BACKGROUND = (8, 18, 22)   # style.css's dark --bg
INK = (232, 246, 250)      # dark --ink
ACCENT = (63, 214, 255)    # dark --accent

ICON_SIZE = 330
SCALE = ICON_SIZE / 180    # the page's icon is 180 px
FONT_SIZE = round(72 * SCALE)
GAP = round(24 * SCALE)
TRACKING = 0.06            # em


def rounded_mask(size, radius):
    """An antialiased rounded-square mask, drawn at 4x and reduced."""
    big = Image.new("L", (size * 4, size * 4), 0)
    ImageDraw.Draw(big).rounded_rectangle((0, 0, size * 4 - 1, size * 4 - 1), radius=radius * 4, fill=255)
    return big.resize((size, size), Image.LANCZOS)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--icon", type=Path, default=ROOT / "port/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png")
    parser.add_argument("--font", type=Path, default=ROOT / "docs/site/fonts/Orbitron-VariableFont_wght.woff2")
    parser.add_argument("--weight", type=int, default=700, help="for a variable font: the h1's bold is 700")
    parser.add_argument("--text", default="VISR")
    parser.add_argument("--out", type=Path, default=ROOT / "docs/site/og.png")
    args = parser.parse_args()

    font = ImageFont.truetype(str(args.font), FONT_SIZE)
    try:
        font.set_variation_by_axes([args.weight])
    except OSError:
        pass    # a static font has no weight axis

    card = Image.new("RGB", (WIDTH, HEIGHT), BACKGROUND)
    measure = ImageDraw.Draw(card)
    advances = [measure.textlength(ch, font=font) for ch in args.text]
    tracking = TRACKING * FONT_SIZE
    text_width = sum(advances) + tracking * (len(args.text) - 1)
    cap_top, cap_bottom = font.getbbox(args.text)[1], font.getbbox(args.text)[3]

    left = round((WIDTH - (ICON_SIZE + GAP + text_width)) / 2)
    top = round((HEIGHT - ICON_SIZE) / 2)

    # A faint glow in the icon's rim color, so the dark card isn't flat
    glow = Image.new("L", (WIDTH, HEIGHT), 0)
    ImageDraw.Draw(glow).ellipse((left - 120, top - 120, left + ICON_SIZE + 120, top + ICON_SIZE + 120), fill=46)
    glow = glow.filter(ImageFilter.GaussianBlur(90))
    card = Image.composite(Image.new("RGB", (WIDTH, HEIGHT), ACCENT), card, glow)

    # The icon, with the page's 22% corner radius and its shadow (0 6px 20px rgba(0, 0, 0, 0.25), scaled)
    mask = rounded_mask(ICON_SIZE, round(0.22 * ICON_SIZE))
    shadow = Image.new("L", (WIDTH, HEIGHT), 0)
    shadow.paste(mask, (left, top + round(6 * SCALE)))
    shadow = shadow.filter(ImageFilter.GaussianBlur(10 * SCALE)).point(lambda v: v * 0.45)
    card = Image.composite(Image.new("RGB", (WIDTH, HEIGHT), (0, 0, 0)), card, shadow)
    icon = Image.open(args.icon).convert("RGBA").resize((ICON_SIZE, ICON_SIZE), Image.LANCZOS)
    card.paste(icon, (left, top), mask)

    # The wordmark, centered on the icon by its capital height
    draw = ImageDraw.Draw(card)
    x = left + ICON_SIZE + GAP
    y = top + ICON_SIZE / 2 - (cap_bottom - cap_top) / 2 - cap_top
    for ch, advance in zip(args.text, advances):
        draw.text((x, y), ch, font=font, fill=INK)
        x += advance + tracking

    card.save(args.out, optimize=True)
    print(f"{args.out}: {WIDTH}x{HEIGHT}")


if __name__ == "__main__":
    main()
