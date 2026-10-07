# Notices

VISR's own code, and the code it inherits from its upstream projects, is under
CC0 1.0 (see [LICENSE.md](LICENSE.md)). The components below keep their own
licenses. Each one's full license text sits next to its source in this
repository, and the Apple builds copy the notices of what they link into the
app's `Licenses` folder (`tools/ios_build.py`).

## Upstream projects

| Project | License | What VISR takes from it |
| --- | --- | --- |
| [punpckhdq/halo](https://github.com/punpckhdq/halo) | CC0 1.0 | The decompiled game in `source/` |
| [bnunu/halo-1](https://github.com/bnunu/halo-1) | CC0 1.0 | Further decompilation in `source/` |
| [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) (formerly cybersecurity/halo-ce-universal) | CC0 1.0 | The ports in `port/`, `tools/` and `pgo/`, the high-resolution HUD, menus and titles in `port/assets/` |
| [NicholasDominici/halo-ce-ios](https://github.com/NicholasDominici/halo-ce-ios) | CC0 1.0 | The iOS port in `port/ios/` and `port/runtime/` |
| [pfista/halo-og](https://github.com/pfista/halo-og) | CC0 1.0 | Metal renderer fixes |

## Vendored libraries

| Component | License | Location | In the Apple apps |
| --- | --- | --- | --- |
| [extract-xiso](https://github.com/XboxDev/extract-xiso) | BSD-style, 4 clauses, with an advertising clause | `port/third_party/extract-xiso/LICENSE.TXT`, `port/runtime/xiso.c`, `port/linux/src/xiso.c` | Yes |
| [Expat](https://libexpat.github.io/) | MIT | `port/third_party/expat/COPYING` | Yes |
| [KCP](https://github.com/skywind3000/kcp) | MIT | `port/third_party/kcp/LICENSE` | Yes |
| [Monocypher](https://monocypher.org/) | BSD-2-Clause or CC0 1.0 | `port/third_party/monocypher/LICENCE.md` | Yes |
| [stb_truetype](https://github.com/nothings/stb) | MIT or public domain (Unlicense) | `port/third_party/stb/LICENSE` | Yes |
| [tomlc17](https://github.com/cktan/tomlc17) | MIT | `port/third_party/tomlc17/LICENSE` | Yes |
| [zlib](https://zlib.net/) 1.3.2 | zlib | `port/third_party/zlib/LICENSE` | Yes |
| [musl](https://musl.libc.org/) math functions | MIT | `port/third_party/musl-math/COPYRIGHT` | No, Linux build only (the Apple builds download musl itself) |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) | Apache-2.0 or GPL-2.0-or-later (we use Apache-2.0) | `port/third_party/mbedtls/LICENSE` | No, desktop builds only |
| [miniupnpc](https://miniupnp.tuxfamily.org/) | BSD-3-Clause | `port/third_party/miniupnpc/LICENSE` | No, desktop builds only |
| zlib, as the game shipped it | zlib | `source/memory/zlib/` | Part of the decompiled source tree |
| libtiff, as the game shipped it | libtiff (Sam Leffler and Silicon Graphics, notice in each file) | `source/bitmaps/libtiff/` | Part of the decompiled source tree |

## Downloaded at build time

`tools/ios_build.py` downloads these and does not commit them:

| Component | License |
| --- | --- |
| [SDL](https://libsdl.org/) 3.4.16 | zlib |
| [musl](https://musl.libc.org/) 1.2.5 | MIT, with the component notices in its `COPYRIGHT` |
| Khronos OpenGL ES and EGL headers | MIT and Apache-2.0, as their registries state in each header |

## Fonts

| Font | License | Location |
| --- | --- | --- |
| [Overpass](https://overpassfont.org/) | SIL Open Font License 1.1 | `port/assets/fonts/Overpass-OFL.txt` |
| OpenCE (Newtown, respaced) | SIL Open Font License 1.1 | `port/assets/fonts/OpenCE-OFL.txt` |
| Newtown by Roger White | Public domain | `port/assets/fonts/Newtown-LICENSE.txt` |

## Required acknowledgement

This product includes software developed by in <in@fishtank.com>.

## Game content and trademarks

Halo, Halo: Combat Evolved, Master Chief and the related names and logos are
trademarks of Microsoft Corporation. Halo: Combat Evolved © Microsoft
Corporation. VISR is not affiliated with or endorsed by Microsoft, Halo Studios
or Bungie.

The repository does not contain the game's maps, sounds, textures or videos.
The HUD, menu and title pictures in `port/assets/` are high-resolution redraws
made for OpenCE, not files from the game. The app icon's helmet, visor and brow
outlines are traced from a frame of the game (see `port/ios/ICON.md`). The menu
layouts and strings in `port/assets/menus/` are generated from the game's tags.
None of the licenses above, CC0 included, grants any rights to Microsoft's
game, its assets or its trademarks.
