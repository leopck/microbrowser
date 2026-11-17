# Inter font input

`Inter.ttf` is the unmodified official Inter variable font from Google Fonts.
Its two variation axes are optical size (`opsz`, 14–32) and weight (`wght`, 100–900).

- Upstream TTF: <https://raw.githubusercontent.com/google/fonts/main/ofl/inter/Inter%5Bopsz%2Cwght%5D.ttf>
- Input SHA256: `29160a80ff49ddcab2c97711247e08b1fab27a484a329ce8b813d820dc559031`
- Upstream license: <https://raw.githubusercontent.com/google/fonts/main/ofl/inter/OFL.txt>

`OFL.txt` is the exact upstream SIL Open Font License 1.1 text. Its copyright statement
is `Copyright 2020 The Inter Project Authors (https://github.com/rsms/inter)` with no
additional Reserved Font Name declaration.

The font-generation tools need Python with Pillow and `libraqm` support. The embedded
renderer does not need either dependency. On a development machine, install the
authored Pillow version before regenerating:

```sh
python3 -m pip install -r tools/requirements-fonts.txt
```

The font unit tests are skipped only when Pillow is unavailable. All other runtime
and application tests remain independent of that authoring dependency.

Pillow versions can change antialiasing. This repository records the exact Pillow,
FreeType, `libraqm`, and HarfBuzz versions used for the committed `core/font_data.cpp` tables.
Regenerate the C++ data from the repository root:

```sh
python3 tools/build_fonts.py --output core/font_data.cpp
```

Pillow is used only by this authoring-time command. The generator sets `opsz` to the
requested pixel size and `wght` to 400 or 600. FreeType clamps `opsz` to the upstream
14–32 range, so the 12px and 36px faces use clamped optical values of 14 and 32 while
retaining their requested glyph pixel sizes.

The committed data was generated with Pillow 12.2.0, FreeType 2.14.3, libraqm 0.10.3,
and HarfBuzz 13.2.1. Antialiased output can differ between rasterizer versions; use the
same versions to reproduce the C++ byte-for-byte. The font and generator inputs are
otherwise deterministic and contain no timestamps.

Pillow places the top raster row of U+007C (`|`) one pixel above the ascender at 14px.
To ensure every packed image stays inside its line box, coverage is clipped at the
line-box edges. In this input that clipping affects only U+007C in the two 14px faces;
its advance and all other metrics remain those reported by FreeType.
