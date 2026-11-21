from __future__ import annotations

import hashlib
import math
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Sequence

from PIL import ImageFont, features

PIXEL_SIZES = (12, 14, 16, 20, 28, 36)
WEIGHTS = (400, 600)
CODEPOINTS = tuple(range(32, 127)) + (0x00B0, 0x2212, 0x2022, 0x2190, 0x2192, 0x2713)
GLYPH_COUNT = len(CODEPOINTS)
MAX_COVERAGE_BYTES = 512 * 1024
EXPANDED_GLYPH_CAPACITY = 1024
PACKED_KERNING_BITS = 18
PACKED_KERNING_ADVANCE_BIAS = 8
FALLBACK_GLYPH_INDEX = ord("?") - 32
REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_FONT_PATH = REPO_ROOT / "third_party" / "inter" / "Inter.ttf"

# GPOS kerning is retained, while substitutions that would require runtime shaping
# are excluded from pair measurements.
KERNING_FEATURES = ("kern", "-liga", "-clig", "-dlig", "-calt")


@dataclass(frozen=True)
class GeneratedGlyph:
    bitmap_offset: int
    width: int
    height: int
    advance: int
    bearing_x: int
    bearing_y: int
    run_length_coded: bool = False


@dataclass(frozen=True)
class KerningPair:
    previous: int
    current: int
    advance: int


@dataclass(frozen=True)
class GeneratedFace:
    pixel_size: int
    weight: int
    line_height: int
    glyphs: tuple[GeneratedGlyph, ...]
    coverage: bytes
    kerning: tuple[KerningPair, ...]


def load_font(font_path: Path, pixel_size: int, weight: int) -> ImageFont.FreeTypeFont:
    if not features.check("raqm"):
        raise RuntimeError("Pillow with libraqm support is required to extract Inter GPOS kerning")
    font = ImageFont.truetype(
        str(font_path), size=pixel_size, layout_engine=ImageFont.Layout.RAQM
    )
    axes = font.get_variation_axes()
    if tuple(axis["name"] for axis in axes) != (b"Optical size", b"Weight"):
        raise ValueError("Inter.ttf must have Optical size followed by Weight variation axes")
    font.set_variation_by_axes([pixel_size, weight])
    return font


def _pack_rows(rows: Sequence[bytes]) -> bytes:
    packed = bytearray()
    for row in rows:
        for x in range(0, len(row), 2):
            high = (row[x] + 8) // 17
            low = (row[x + 1] + 8) // 17 if x + 1 < len(row) else 0
            packed.append((high << 4) | low)
    return bytes(packed)


def _rasterize_glyph(
    font: ImageFont.FreeTypeFont,
    codepoint: int,
    line_height: int,
    leading: int,
    bitmap_offset: int,
) -> tuple[GeneratedGlyph, bytes]:
    mask, (bearing_x, ascender_y) = font.getmask2(chr(codepoint))
    mask_width, mask_height = mask.size
    pixels = bytes(mask)
    if len(pixels) != mask_width * mask_height:
        raise RuntimeError(f"Unexpected Pillow mask stride for U+{codepoint:04X}")

    mask_y = leading + ascender_y
    first_source_row = max(0, -mask_y)
    last_source_row = min(mask_height, line_height - mask_y)
    rows = [
        pixels[y * mask_width : (y + 1) * mask_width]
        for y in range(first_source_row, last_source_row)
    ]
    coverage = _pack_rows(rows)
    advance = round(font.getlength(chr(codepoint)))
    glyph = GeneratedGlyph(
        bitmap_offset=bitmap_offset,
        width=mask_width,
        height=len(rows),
        advance=advance,
        bearing_x=bearing_x,
        bearing_y=mask_y + first_source_row,
    )

    if not 0 <= glyph.bearing_y <= line_height:
        raise ValueError(f"Glyph U+{codepoint:04X} starts outside its line box")
    if glyph.bearing_y + glyph.height > line_height:
        raise ValueError(f"Glyph U+{codepoint:04X} extends below its line box")
    if not 0 <= glyph.advance <= 255 or not -128 <= glyph.bearing_x <= 127:
        raise ValueError(f"Glyph U+{codepoint:04X} metrics exceed the shared field types")
    if len(coverage) != ((glyph.width + 1) // 2) * glyph.height:
        raise ValueError(f"Glyph U+{codepoint:04X} has an invalid packed row size")
    return glyph, coverage


def _generate_kerning(font: ImageFont.FreeTypeFont) -> tuple[KerningPair, ...]:
    characters = [chr(codepoint) for codepoint in CODEPOINTS]
    advances = [font.getlength(character) for character in characters]
    pairs = []
    for previous, previous_character in enumerate(characters):
        for current, current_character in enumerate(characters):
            pair_advance = font.getlength(
                previous_character + current_character, features=KERNING_FEATURES
            )
            delta = round(pair_advance - advances[previous] - advances[current])
            if delta == 0:
                continue
            if not -128 <= delta <= 127:
                raise ValueError("Kerning delta exceeds int8_t")
            pairs.append(KerningPair(previous, current, delta))
    return tuple(pairs)


def encode_byte_runs(raw: bytes) -> bytes:
    if not raw:
        return b""

    # Pick the cheapest mix of literal tokens (length + bytes) and repeated
    # tokens (length + value), each holding at most 128 bytes.
    cost = [0] + [2 * len(raw) + 1] * len(raw)
    previous: dict[int, tuple[bool, int]] = {}
    for end in range(1, len(raw) + 1):
        for start in range(max(0, end - 128), end):
            candidate = cost[start] + end - start + 1
            if candidate < cost[end]:
                cost[end] = candidate
                previous[end] = (False, start)

        run_start = end - 1
        while run_start > 0 and raw[run_start - 1] == raw[end - 1]:
            run_start -= 1
        for start in range(max(run_start, end - 128), end - 2):
            candidate = cost[start] + 2
            if candidate < cost[end]:
                cost[end] = candidate
                previous[end] = (True, start)

    groups = []
    end = len(raw)
    while end:
        repeated, start = previous[end]
        if repeated:
            groups.append(bytes((0x80 | (end - start - 1), raw[start])))
        else:
            groups.append(bytes((end - start - 1,)) + raw[start:end])
        end = start
    return b"".join(reversed(groups))


def decode_byte_runs(encoded: bytes, output_size: int) -> bytes:
    decoded = bytearray()
    offset = 0
    while len(decoded) < output_size:
        if offset >= len(encoded):
            raise ValueError("Truncated byte-run glyph")
        token = encoded[offset]
        offset += 1
        count = (token & 0x7F) + 1
        if len(decoded) + count > output_size:
            raise ValueError("Byte-run glyph exceeds its declared dimensions")
        if token & 0x80:
            if offset >= len(encoded):
                raise ValueError("Truncated byte-run value")
            decoded.extend([encoded[offset]] * count)
            offset += 1
        else:
            if offset + count > len(encoded):
                raise ValueError("Truncated literal glyph bytes")
            decoded.extend(encoded[offset : offset + count])
            offset += count
    return bytes(decoded)


def decode_glyph_coverage(face: GeneratedFace, glyph: GeneratedGlyph) -> bytes:
    expected_size = ((glyph.width + 1) // 2) * glyph.height
    encoded = face.coverage[glyph.bitmap_offset :]
    if glyph.run_length_coded:
        return decode_byte_runs(encoded, expected_size)
    if len(encoded) < expected_size:
        raise ValueError("Truncated uncompressed glyph")
    return encoded[:expected_size]


def _compact_face(face: GeneratedFace) -> GeneratedFace:
    glyphs = []
    coverage = bytearray()
    for glyph in face.glyphs:
        raw = decode_glyph_coverage(face, glyph)
        if len(raw) > EXPANDED_GLYPH_CAPACITY:
            raise ValueError("Glyph exceeds the bounded decoder buffer")
        encoded = encode_byte_runs(raw)
        run_length_coded = len(encoded) < len(raw)
        glyphs.append(replace(
            glyph,
            bitmap_offset=len(coverage),
            run_length_coded=run_length_coded,
        ))
        coverage.extend(encoded if run_length_coded else raw)
    return replace(face, glyphs=tuple(glyphs), coverage=bytes(coverage))


def pack_kerning_pairs(pairs: Sequence[KerningPair]) -> bytes:
    packed = bytearray((len(pairs) * PACKED_KERNING_BITS + 7) // 8)
    for index, pair in enumerate(pairs):
        if not 0 <= pair.previous < 128 or not 0 <= pair.current < 128:
            raise ValueError("Kerning glyph indices must fit seven bits")
        if not -8 <= pair.advance <= 7:
            raise ValueError("Kerning advance must fit four signed bits")
        value = (((pair.previous << 7) | pair.current) << 4) | (
            pair.advance + PACKED_KERNING_ADVANCE_BIAS
        )
        byte_offset, shift = divmod(index * PACKED_KERNING_BITS, 8)
        wide = value << shift
        for byte_index in range(3):
            packed[byte_offset + byte_index] |= (wide >> (8 * byte_index)) & 0xFF
    return bytes(packed)


def decode_kerning_pairs(encoded: bytes) -> tuple[KerningPair, ...]:
    pairs = []
    for index in range(len(encoded) * 8 // PACKED_KERNING_BITS):
        byte_offset, shift = divmod(index * PACKED_KERNING_BITS, 8)
        if byte_offset + 3 > len(encoded):
            raise ValueError("Truncated packed kerning record")
        window = 0
        for byte_index in range(3):
            window |= encoded[byte_offset + byte_index] << (8 * byte_index)
        value = (window >> shift) & ((1 << PACKED_KERNING_BITS) - 1)
        pair = value >> 4
        previous, current = divmod(pair, 128)
        advance = (value & 0xF) - PACKED_KERNING_ADVANCE_BIAS
        pairs.append(KerningPair(previous, current, advance))
    return tuple(pairs)


def _generate_face(font_path: Path, pixel_size: int, weight: int) -> GeneratedFace:
    font = load_font(font_path, pixel_size, weight)
    ascent, descent = font.getmetrics()
    line_height = max(ascent + descent, math.ceil(pixel_size * 1.25))
    leading = (line_height - ascent - descent) // 2
    if not 0 <= line_height <= 255:
        raise ValueError("Line height exceeds uint8_t")

    glyphs = []
    coverage = bytearray()
    for codepoint in CODEPOINTS:
        glyph, packed = _rasterize_glyph(
            font, codepoint, line_height, leading, len(coverage)
        )
        glyphs.append(glyph)
        coverage.extend(packed)
    return _compact_face(GeneratedFace(
        pixel_size=pixel_size,
        weight=weight,
        line_height=line_height,
        glyphs=tuple(glyphs),
        coverage=bytes(coverage),
        kerning=_generate_kerning(font),
    ))


def generate_faces(font_path: Path = DEFAULT_FONT_PATH) -> tuple[GeneratedFace, ...]:
    faces = tuple(
        _generate_face(font_path, pixel_size, weight)
        for pixel_size in PIXEL_SIZES
        for weight in WEIGHTS
    )
    if sum(len(face.coverage) for face in faces) > MAX_COVERAGE_BYTES:
        raise ValueError("Generated coverage exceeds 512 KiB")
    return faces


def _array_definition(element_type: str, name: str, count: int, body: str) -> str:
    if count == 0:
        return f"constexpr std::array<{element_type}, 0> {name}{{}};"
    return (
        f"constexpr std::array<{element_type}, {count}> {name}{{{{\n"
        f"{body}\n"
        "    }};"
    )


def _glyph_array(name: str, glyphs: Sequence[GeneratedGlyph]) -> str:
    rows = []
    for codepoint, glyph in zip(CODEPOINTS, glyphs, strict=True):
        encoding = "byte_runs" if glyph.run_length_coded else "packed_nibbles"
        rows.append(
            "        "
            f"Glyph{{{glyph.bitmap_offset}U, {glyph.width}U, {glyph.height}U, "
            f"{glyph.advance}U, {glyph.bearing_x}, {glyph.bearing_y}, "
            f"CoverageEncoding::{encoding}}}, // U+{codepoint:04X}"
        )
    return _array_definition("Glyph", name, len(glyphs), "\n".join(rows))


def _coverage_array(name: str, coverage: bytes) -> str:
    rows = []
    for offset in range(0, len(coverage), 16):
        values = ", ".join(f"0x{value:02X}" for value in coverage[offset : offset + 16])
        rows.append(f"        {values},")
    return _array_definition("uint8_t", name, len(coverage), "\n".join(rows))


def _kerning_array(name: str, pairs: Sequence[KerningPair]) -> str:
    return _coverage_array(name, pack_kerning_pairs(pairs))


def render_cpp(font_path: Path, faces: Sequence[GeneratedFace]) -> str:
    if len(faces) != len(PIXEL_SIZES) * len(WEIGHTS):
        raise ValueError("Exactly twelve faces are required")
    input_sha256 = hashlib.sha256(font_path.read_bytes()).hexdigest()
    sections = [
        "// Generated by tools/build_fonts.py; do not edit.\n"
        f"// Inter.ttf SHA256: {input_sha256}\n"
        "// Inter is licensed under SIL OFL 1.1; see third_party/inter/OFL.txt.\n"
        '#include "font_data.h"\n\n'
        "namespace mb::fonts {\nnamespace {\n"
    ]
    for face in faces:
        suffix = f"{face.pixel_size}_{face.weight}"
        sections.extend(
            (
                _glyph_array(f"glyphs_{suffix}", face.glyphs) + "\n",
                _coverage_array(f"coverage_{suffix}", face.coverage) + "\n",
                _kerning_array(f"kerning_{suffix}", face.kerning) + "\n",
            )
        )
    sections.append("} // namespace\n\n")

    initializers = []
    for face in faces:
        suffix = f"{face.pixel_size}_{face.weight}"
        initializers.append(
            f"    Face{{{face.pixel_size}U, {face.weight}U, {face.line_height}U, "
            f"glyphs_{suffix}, coverage_{suffix}, kerning_{suffix}}},"
        )
    sections.append(
        "const std::array<Face, 12> faces{{\n"
        + "\n".join(initializers)
        + "\n}};\n\n"
    )
    sections.append(
        "std::size_t glyph_index(uint32_t codepoint) noexcept {\n"
        "    if (codepoint >= 0x20U && codepoint <= 0x7EU) return codepoint - 0x20U;\n"
        "    switch (codepoint) {\n"
        "    case 0x00B0U: return 95U;\n"
        "    case 0x2212U: return 96U;\n"
        "    case 0x2022U: return 97U;\n"
        "    case 0x2190U: return 98U;\n"
        "    case 0x2192U: return 99U;\n"
        "    case 0x2713U: return 100U;\n"
        f"    default: return {FALLBACK_GLYPH_INDEX}U;\n"
        "    }\n"
        "}\n\n"
        "} // namespace mb::fonts\n"
    )
    return "".join(sections)
