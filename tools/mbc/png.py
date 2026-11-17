from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass

from .errors import PngError

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
MAX_PNG_BYTES = 16 * 1024 * 1024
MAX_DIMENSION = 2048
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
ALLOWED_DEPTHS = {
    0: {1, 2, 4, 8, 16},
    2: {8, 16},
    3: {1, 2, 4, 8},
    4: {8, 16},
    6: {8, 16},
}


@dataclass(frozen=True, slots=True)
class PngImage:
    width: int
    height: int
    rgba: bytes


@dataclass(frozen=True, slots=True)
class AssetImage:
    width: int
    height: int
    pixels: bytes


@dataclass(frozen=True, slots=True)
class _Header:
    width: int
    height: int
    depth: int
    color_type: int


def _chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    if not data.startswith(PNG_SIGNATURE):
        raise PngError("not a PNG file (bad signature)")
    if len(data) > MAX_PNG_BYTES:
        raise PngError("PNG file exceeds 16 MiB")
    chunks: list[tuple[bytes, bytes]] = []
    offset = len(PNG_SIGNATURE)
    while offset < len(data):
        if len(data) - offset < 12:
            raise PngError("truncated PNG chunk header")
        length = struct.unpack_from(">I", data, offset)[0]
        chunk_type = data[offset + 4 : offset + 8]
        end = offset + 12 + length
        if end > len(data):
            name = chunk_type.decode("ascii", errors="replace")
            raise PngError(f"truncated {name} chunk")
        payload = data[offset + 8 : offset + 8 + length]
        expected_crc = struct.unpack_from(">I", data, offset + 8 + length)[0]
        actual_crc = zlib.crc32(chunk_type)
        actual_crc = zlib.crc32(payload, actual_crc) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            name = chunk_type.decode("ascii", errors="replace")
            raise PngError(f"CRC mismatch in {name} chunk")
        if any(not (0x41 <= byte <= 0x5A or 0x61 <= byte <= 0x7A) for byte in chunk_type):
            raise PngError("invalid PNG chunk type")
        chunks.append((chunk_type, payload))
        offset = end
    if not chunks:
        raise PngError("PNG has no chunks")
    return chunks


def _header(chunks: list[tuple[bytes, bytes]]) -> _Header:
    if chunks[0][0] != b"IHDR" or len(chunks[0][1]) != 13:
        raise PngError("first chunk must be a 13-byte IHDR")
    width, height, depth, color_type, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", chunks[0][1]
    )
    if width == 0 or height == 0:
        raise PngError("PNG dimensions must be nonzero")
    if width > MAX_DIMENSION or height > MAX_DIMENSION:
        raise PngError("PNG dimensions exceed 2048 pixels")
    if color_type not in CHANNELS or depth not in ALLOWED_DEPTHS[color_type]:
        raise PngError(f"unsupported PNG color type/bit depth {color_type}/{depth}")
    if compression != 0 or filtering != 0:
        raise PngError("unsupported PNG compression or filter method")
    if interlace != 0:
        raise PngError("interlaced PNG is not supported")
    return _Header(width, height, depth, color_type)


def _metadata(
    chunks: list[tuple[bytes, bytes]], header: _Header
) -> tuple[list[tuple[int, int, int]], bytes, bytes]:
    compressed: list[bytes] = []
    palette: list[tuple[int, int, int]] = []
    transparency = b""
    seen_idat = False
    closed_idat = False
    seen_iend = False
    seen_plte = False
    seen_trns = False
    for index, (chunk_type, payload) in enumerate(chunks[1:], start=1):
        if seen_iend:
            raise PngError("chunks after IEND")
        if chunk_type == b"IHDR":
            raise PngError("duplicate IHDR chunk")
        if chunk_type == b"IDAT":
            if closed_idat:
                raise PngError("IDAT chunks must be consecutive")
            if header.color_type == 3 and not seen_plte:
                raise PngError("indexed PNG requires PLTE before IDAT")
            seen_idat = True
            compressed.append(payload)
            continue
        if seen_idat:
            closed_idat = True
        if chunk_type == b"IEND":
            if payload:
                raise PngError("IEND must be empty")
            if not seen_idat:
                raise PngError("PNG has no IDAT data")
            if index != len(chunks) - 1:
                raise PngError("IEND is not the final chunk")
            seen_iend = True
        elif chunk_type == b"PLTE":
            if seen_plte or seen_idat:
                raise PngError("PLTE must occur once before IDAT")
            if header.color_type in (0, 4):
                raise PngError("PLTE is not allowed for grayscale PNG")
            if len(payload) == 0 or len(payload) % 3 or len(payload) > 768:
                raise PngError("PLTE length must be a nonzero multiple of three, at most 768")
            palette = [
                (payload[index], payload[index + 1], payload[index + 2])
                for index in range(0, len(payload), 3)
            ]
            if header.color_type == 3 and len(palette) > 1 << header.depth:
                raise PngError("PLTE has more entries than the bit depth permits")
            seen_plte = True
        elif chunk_type == b"tRNS":
            if seen_trns or seen_idat:
                raise PngError("tRNS must occur once before IDAT")
            if header.color_type in (4, 6):
                raise PngError("tRNS is not allowed with an alpha channel")
            expected = {0: 2, 2: 6}.get(header.color_type)
            if expected is not None and len(payload) != expected:
                raise PngError("invalid tRNS length for this color type")
            if header.color_type == 3:
                if not seen_plte:
                    raise PngError("palette tRNS must follow PLTE")
                if len(payload) > len(palette):
                    raise PngError("tRNS has more entries than PLTE")
            transparency = payload
            seen_trns = True
        elif not chunk_type[0] & 0x20:
            name = chunk_type.decode("ascii", errors="replace")
            raise PngError(f"unsupported critical PNG chunk {name}")
    if not seen_iend:
        raise PngError("PNG is missing IEND")
    if header.color_type == 3 and not seen_plte:
        raise PngError("indexed PNG has no PLTE")
    return palette, transparency, b"".join(compressed)


def _scanlines(compressed: bytes, header: _Header) -> list[bytes]:
    bits_per_pixel = CHANNELS[header.color_type] * header.depth
    stride = (header.width * bits_per_pixel + 7) // 8
    expected = header.height * (stride + 1)
    decompressor = zlib.decompressobj()
    try:
        raw = decompressor.decompress(compressed, expected + 1)
        if len(raw) <= expected:
            raw += decompressor.flush(expected + 1 - len(raw))
    except zlib.error as error:
        raise PngError(f"invalid PNG compressed data: {error}") from error
    if len(raw) != expected:
        raise PngError(f"PNG decompressed size is {len(raw)}, expected {expected}")
    if decompressor.unconsumed_tail or decompressor.unused_data:
        raise PngError("PNG has excess or trailing compressed data")
    if not decompressor.eof:
        raise PngError("truncated PNG compressed stream")
    rows: list[bytes] = []
    previous = bytes(stride)
    filter_bytes = max(1, (bits_per_pixel + 7) // 8)
    offset = 0
    for row_index in range(header.height):
        filter_type = raw[offset]
        encoded = raw[offset + 1 : offset + stride + 1]
        offset += stride + 1
        if filter_type > 4:
            raise PngError(f"unknown PNG row filter {filter_type} on row {row_index}")
        row = bytearray(stride)
        for index, value in enumerate(encoded):
            left = row[index - filter_bytes] if index >= filter_bytes else 0
            above = previous[index]
            upper_left = previous[index - filter_bytes] if index >= filter_bytes else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = above
            elif filter_type == 3:
                predictor = (left + above) // 2
            else:
                estimate = left + above - upper_left
                distances = (abs(estimate - left), abs(estimate - above), abs(estimate - upper_left))
                predictor = (left, above, upper_left)[distances.index(min(distances))]
            row[index] = (value + predictor) & 0xFF
        rows.append(bytes(row))
        previous = bytes(row)
    return rows


def _sample(row: bytes, index: int, depth: int) -> int:
    if depth == 8:
        return row[index]
    if depth == 16:
        return struct.unpack_from(">H", row, index * 2)[0]
    bit_index = index * depth
    shift = 8 - depth - bit_index % 8
    return (row[bit_index // 8] >> shift) & ((1 << depth) - 1)


def _byte_sample(value: int, depth: int) -> int:
    if depth == 16:
        return value >> 8
    if depth == 8:
        return value
    return value * 255 // ((1 << depth) - 1)


def _rgba(
    rows: list[bytes], header: _Header, palette: list[tuple[int, int, int]], transparency: bytes
) -> bytes:
    output = bytearray(header.width * header.height * 4)
    write = 0
    depth = header.depth
    for row in rows:
        for pixel in range(header.width):
            alpha = 255
            if header.color_type == 0:
                gray = _sample(row, pixel, depth)
                red = green = blue = _byte_sample(gray, depth)
                if transparency and gray == struct.unpack(">H", transparency)[0]:
                    alpha = 0
            elif header.color_type == 2:
                samples = [_sample(row, pixel * 3 + channel, depth) for channel in range(3)]
                red, green, blue = [_byte_sample(value, depth) for value in samples]
                if transparency and tuple(samples) == struct.unpack(">HHH", transparency):
                    alpha = 0
            elif header.color_type == 3:
                index = _sample(row, pixel, depth)
                if index >= len(palette):
                    raise PngError(f"palette index {index} is outside PLTE")
                red, green, blue = palette[index]
                alpha = transparency[index] if index < len(transparency) else 255
            elif header.color_type == 4:
                gray = _byte_sample(_sample(row, pixel * 2, depth), depth)
                red = green = blue = gray
                alpha = _byte_sample(_sample(row, pixel * 2 + 1, depth), depth)
            else:
                red, green, blue, alpha = [
                    _byte_sample(_sample(row, pixel * 4 + channel, depth), depth)
                    for channel in range(4)
                ]
            output[write : write + 4] = bytes((red, green, blue, alpha))
            write += 4
    return bytes(output)


def decode_png(data: bytes) -> PngImage:
    chunks = _chunks(data)
    header = _header(chunks)
    palette, transparency, compressed = _metadata(chunks, header)
    rows = _scanlines(compressed, header)
    return PngImage(header.width, header.height, _rgba(rows, header, palette, transparency))


def _resize(image: PngImage, width: int, height: int) -> PngImage:
    if image.width == width and image.height == height:
        return image
    rgba = bytearray(width * height * 4)
    destination = 0
    for y in range(height):
        source_y = y * image.height // height
        for x in range(width):
            source_x = x * image.width // width
            source = (source_y * image.width + source_x) * 4
            rgba[destination : destination + 4] = image.rgba[source : source + 4]
            destination += 4
    return PngImage(width, height, bytes(rgba))


def rgb565_asset(data: bytes, resize: tuple[int, int] | None = None) -> AssetImage:
    image = decode_png(data)
    if resize is not None:
        image = _resize(image, *resize)
    pixels = bytearray(image.width * image.height * 2)
    for pixel in range(image.width * image.height):
        red, green, blue = image.rgba[pixel * 4 : pixel * 4 + 3]
        encoded = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        struct.pack_into("<H", pixels, pixel * 2, encoded)
    return AssetImage(image.width, image.height, bytes(pixels))
