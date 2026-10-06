# VISR app icon

The icon is original vector art made for VISR. It uses no Microsoft or Bungie
art: no character, no helmet or visor silhouette, no ring, and no logo or
typeface from the games.

It shows a HUD reticle: a cyan ring broken at the four compass points, with
tick marks every 10 degrees, four white crosshair bars, a white chevron, and an
amber pip below it. It sits on a dark teal backdrop with a faint grid, lit a
little above center.

## Layers

The art comes in three layers, so visionOS and tvOS can float them apart:

- **Back**: the backdrop and grid, opaque.
- **Middle**: the cyan ring and its tick marks.
- **Front**: the crosshair bars, the chevron and the amber pip.

The iOS and iPadOS icons are the three layers flattened into one opaque square.
iOS applies the rounded-square mask and visionOS the circle at display time, so
nothing is baked in. The mark stays inside a circle of radius 420 on the
1024-unit canvas, which visionOS's circular crop leaves whole.

## How it is made

`tools/visr_icon.py` writes each layer as SVG and renders it with
`rsvg-convert` (librsvg); `magick` (ImageMagick) flattens the opaque layers and
draws the preview mockups. Nothing is resized from a bitmap: every size is
rendered from the vector source.

```sh
python3 tools/visr_icon.py --catalogs           # render the icon into all three asset catalogs
python3 tools/visr_icon.py --previews DIR       # every concept's layers, plus iOS and visionOS mockups
```

`--catalogs` writes:

- `Assets.xcassets/AppIcon.appiconset`: the iPhone and iPad sizes, 20 to 1024 pixels.
- `Assets-tvOS.xcassets/AppIcon.brandassets`: the Front, Middle and Back layers of
  the 400 x 240 Home Screen icon (at 1x and 2x) and the 1280 x 768 App Store
  icon, and the Top Shelf images (the reticle beside the letters VISR).
- `Assets-visionOS.xcassets/AppIcon.solidimagestack`: the Front, Middle and Back
  layers at 1024 x 1024.

## Other concepts

The script draws two more concepts. To use one, pass it with `--concept` and
rerun `--catalogs`:

- `visor`: an abstract octagonal visor lens in cyan glass with a small reticle
  and meter bars. It turns into a thin sliver at Home Screen sizes.
- `wordmark`: the letters VISR in chamfered strokes inside HUD corner brackets.
  The letters are drawn as strokes in the script, so no font is needed.
