from __future__ import annotations

import math
import unittest

try:
    from PIL import ImageFont
except ModuleNotFoundError as error:
    if error.name != "PIL":
        raise
    raise unittest.SkipTest("font regeneration requires tools/requirements-fonts.txt") from error

from tools import font_data

FONT_PATH = font_data.DEFAULT_FONT_PATH
CPP_PATH = font_data.REPO_ROOT / "core" / "font_data.cpp"


def expected_bitmap(
    font: ImageFont.FreeTypeFont, codepoint: int, line_height: int
) -> tuple[int, int, int, int, bytes]:
    ascent, descent = font.getmetrics()
    leading = (line_height - ascent - descent) // 2
    mask, (bearing_x, ascender_y) = font.getmask2(chr(codepoint))
    width, mask_height = mask.size
    mask_y = leading + ascender_y
    first_row = max(0, -mask_y)
    last_row = min(mask_height, line_height - mask_y)
    pixels = bytes(mask)
    rows = [
        pixels[y * width : (y + 1) * width]
        for y in range(first_row, last_row)
    ]
    packed = bytearray()
    for row in rows:
        for x in range(0, width, 2):
            high = (row[x] + 8) // 17
            low = (row[x + 1] + 8) // 17 if x + 1 < width else 0
            packed.append(high * 16 + low)
    return bearing_x, mask_y + first_row, width, len(rows), bytes(packed)


class FontDataTests(unittest.TestCase):
    faces: tuple[font_data.GeneratedFace, ...]
    fonts: dict[tuple[int, int], ImageFont.FreeTypeFont]

    @classmethod
    def setUpClass(cls) -> None:
        cls.faces = font_data.generate_faces(FONT_PATH)
        cls.fonts = {}
        for pixel_size in font_data.PIXEL_SIZES:
            for weight in font_data.WEIGHTS:
                font = ImageFont.truetype(
                    str(FONT_PATH), size=pixel_size, layout_engine=ImageFont.Layout.RAQM
                )
                font.set_variation_by_axes([pixel_size, weight])
                cls.fonts[pixel_size, weight] = font

    def test_faces_have_requested_order(self) -> None:
        self.assertEqual(
            [(face.pixel_size, face.weight) for face in self.faces],
            [(size, weight) for size in font_data.PIXEL_SIZES for weight in font_data.WEIGHTS],
        )
        self.assertEqual(len(font_data.CODEPOINTS), 101)
        self.assertEqual(font_data.CODEPOINTS[:95], tuple(range(32, 127)))
        self.assertEqual(
            font_data.CODEPOINTS[95:], (0x00B0, 0x2212, 0x2022, 0x2190, 0x2192, 0x2713)
        )
        self.assertEqual(font_data.CODEPOINTS[font_data.FALLBACK_GLYPH_INDEX], ord("?"))
        for face in self.faces:
            self.assertEqual(len(face.glyphs), font_data.GLYPH_COUNT)
            font = self.fonts[face.pixel_size, face.weight]
            ascent, descent = font.getmetrics()
            expected_height = max(
                ascent + descent, math.ceil(face.pixel_size * 1.25)
            )
            self.assertEqual(face.line_height, expected_height)

    def test_glyph_offsets_and_bounds_cover_all_glyphs(self) -> None:
        for face in self.faces:
            next_offset = 0
            for glyph in face.glyphs:
                self.assertGreater(glyph.advance, 0)
                self.assertLessEqual(glyph.advance, 255)
                self.assertTrue(-128 <= glyph.bearing_x <= 127)
                self.assertGreaterEqual(glyph.bearing_y, 0)
                self.assertLessEqual(glyph.bearing_y + glyph.height, face.line_height)
                decoded = font_data.decode_glyph_coverage(face, glyph)
                encoded = font_data.encode_byte_runs(decoded) if glyph.run_length_coded else decoded
                self.assertLessEqual(len(decoded), font_data.EXPANDED_GLYPH_CAPACITY)
                self.assertEqual(glyph.bitmap_offset, next_offset)
                next_offset += len(encoded)
            self.assertEqual(next_offset, len(face.coverage))

    def test_coverage_matches_pillow_masks(self) -> None:
        for face in self.faces:
            font = self.fonts[face.pixel_size, face.weight]
            for codepoint, glyph in zip(font_data.CODEPOINTS, face.glyphs, strict=True):
                with self.subTest(face=face.pixel_size, weight=face.weight, codepoint=codepoint):
                    bearing_x, bearing_y, width, height, coverage = expected_bitmap(
                        font, codepoint, face.line_height
                    )
                    self.assertEqual(glyph.bearing_x, bearing_x)
                    self.assertEqual(glyph.bearing_y, bearing_y)
                    self.assertEqual(glyph.width, width)
                    self.assertEqual(glyph.height, height)
                    self.assertEqual(font_data.decode_glyph_coverage(face, glyph), coverage)

    def test_glyph_advances_match_freetype(self) -> None:
        for face in self.faces:
            font = self.fonts[face.pixel_size, face.weight]
            for codepoint, glyph in zip(font_data.CODEPOINTS, face.glyphs, strict=True):
                self.assertEqual(glyph.advance, round(font.getlength(chr(codepoint))))

    def test_kerning_matches_gpos_pair_metrics(self) -> None:
        characters = [chr(codepoint) for codepoint in font_data.CODEPOINTS]
        for face in self.faces:
            font = self.fonts[face.pixel_size, face.weight]
            advances = [font.getlength(character) for character in characters]
            expected = {}
            for previous, previous_character in enumerate(characters):
                for current, current_character in enumerate(characters):
                    pair_advance = font.getlength(
                        previous_character + current_character,
                        features=font_data.KERNING_FEATURES,
                    )
                    delta = round(
                        pair_advance - advances[previous] - advances[current]
                    )
                    if delta:
                        expected[previous, current] = delta
            actual = {(pair.previous, pair.current): pair.advance for pair in face.kerning}
            self.assertEqual(len(actual), len(face.kerning))
            self.assertEqual(actual, expected)
            self.assertTrue(actual)
            self.assertEqual(
                list(actual),
                sorted(actual),
                "Kerning pairs must support binary lookup by glyph indices",
            )

    def test_byte_runs_round_trip_boundaries(self) -> None:
        samples = (b"", b"\x00" * 128, b"\x00" * 129, b"abc" * 85,
                   b"\x00\xff" * 64, b"abbbbbbbc")
        for sample in samples:
            encoded = font_data.encode_byte_runs(sample)
            self.assertEqual(font_data.decode_byte_runs(encoded, len(sample)), sample)

    def test_packed_kerning_round_trip(self) -> None:
        for face in self.faces:
            packed = font_data.pack_kerning_pairs(face.kerning)
            self.assertEqual(font_data.decode_kerning_pairs(packed), face.kerning)
        limits = (
            font_data.KerningPair(100, 127, -8),
            font_data.KerningPair(127, 100, 7),
        )
        self.assertEqual(
            font_data.decode_kerning_pairs(font_data.pack_kerning_pairs(limits)), limits
        )

    def test_packed_kerning_rejects_unrepresentable_values(self) -> None:
        invalid_pairs = (
            font_data.KerningPair(128, 0, 0),
            font_data.KerningPair(0, 128, 0),
            font_data.KerningPair(0, 0, -9),
            font_data.KerningPair(0, 0, 8),
        )
        for pair in invalid_pairs:
            with self.assertRaises(ValueError):
                font_data.pack_kerning_pairs((pair,))

    def test_total_coverage_is_bounded(self) -> None:
        self.assertLessEqual(
            sum(len(face.coverage) for face in self.faces),
            font_data.MAX_COVERAGE_BYTES,
        )

    def test_checked_in_cpp_matches_generator(self) -> None:
        self.assertEqual(
            CPP_PATH.read_text(encoding="utf-8"),
            font_data.render_cpp(FONT_PATH, self.faces),
        )


if __name__ == "__main__":
    unittest.main()
