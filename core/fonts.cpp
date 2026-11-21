#include "fonts.h"

#include <algorithm>
#include <stdexcept>

namespace mb::fonts {

std::span<const uint8_t> decode_glyph_coverage(
    const Face& face, const Glyph& glyph, std::span<uint8_t> scratch) {
    const std::size_t row_bytes = (std::size_t(glyph.width) + 1U) / 2U;
    const std::size_t expected_bytes = row_bytes * glyph.height;
    if (expected_bytes > scratch.size()) {
        throw std::invalid_argument("glyph coverage exceeds decoder capacity");
    }
    if (glyph.bitmap_offset > face.coverage.size()) {
        throw std::invalid_argument("glyph offset exceeds its face coverage");
    }
    const auto encoded = face.coverage.subspan(glyph.bitmap_offset);
    if (glyph.coverage_encoding == CoverageEncoding::packed_nibbles) {
        if (expected_bytes > encoded.size()) {
            throw std::invalid_argument("glyph exceeds available nibble coverage");
        }
        return encoded.first(expected_bytes);
    }
    if (glyph.coverage_encoding != CoverageEncoding::byte_runs) {
        throw std::invalid_argument("unsupported glyph coverage encoding");
    }

    std::size_t read_offset = 0;
    std::size_t write_offset = 0;
    while (write_offset < expected_bytes) {
        if (read_offset >= encoded.size()) {
            throw std::invalid_argument("glyph byte runs are truncated");
        }
        const uint8_t token = encoded[read_offset++];
        const std::size_t count = std::size_t(token & 0x7fU) + 1U;
        if (count > expected_bytes - write_offset) {
            throw std::invalid_argument("glyph byte runs exceed the declared dimensions");
        }
        if ((token & 0x80U) != 0) {
            if (read_offset >= encoded.size()) {
                throw std::invalid_argument("glyph repeated-byte token is truncated");
            }
            std::fill_n(scratch.begin() + write_offset, count, encoded[read_offset++]);
        } else {
            if (read_offset > encoded.size() || count > encoded.size() - read_offset) {
                throw std::invalid_argument("glyph literal bytes are truncated");
            }
            std::copy_n(encoded.begin() + read_offset, count, scratch.begin() + write_offset);
            read_offset += count;
        }
        write_offset += count;
    }
    return scratch.first(expected_bytes);
}

bool face_exists(int pixel_size, int weight) noexcept {
    return std::any_of(faces.begin(), faces.end(), [=](const Face& face) {
        return face.pixel_size == pixel_size && face.weight == weight;
    });
}

const Face& face_for(int pixel_size, int weight) {
    const auto match = std::find_if(faces.begin(), faces.end(), [=](const Face& face) {
        return face.pixel_size == pixel_size && face.weight == weight;
    });
    if (match == faces.end()) throw std::invalid_argument("requested font size and weight are not built in");
    return *match;
}

const Glyph& glyph_for(const Face& face, std::size_t index) {
    if (index >= face.glyphs.size()) throw std::out_of_range("glyph index exceeds its font table");
    return face.glyphs[index];
}

int pair_advance(const Face& face, std::size_t previous, std::size_t current) noexcept {
    if (previous == glyph_count) return 0;
    if (previous > glyph_count || current >= glyph_count) return 0;
    constexpr std::size_t kBitsPerValue = 11;
    constexpr uint32_t kValueMask = (1U << kBitsPerValue) - 1U;
    if (face.kerning_rows.size() != glyph_count + 1U) return 0;
    if (face.kerning_rows.back() != face.kerning.size() * 8U / kBitsPerValue) return 0;
    std::size_t begin = face.kerning_rows[previous];
    std::size_t end = face.kerning_rows[previous + 1U];
    while (begin < end) {
        const std::size_t index = (begin + end) / 2U;
        const std::size_t byte_offset = index * kBitsPerValue / 8U;
        const std::size_t shift = (index * kBitsPerValue) % 8U;
        if (byte_offset >= face.kerning.size()) return 0;
        const std::size_t remaining = face.kerning.size() - byte_offset;
        if (remaining * 8U < shift + kBitsPerValue) return 0;
        uint32_t window = uint32_t(face.kerning[byte_offset]);
        if (remaining > 1U) window |= uint32_t(face.kerning[byte_offset + 1U]) << 8U;
        if (remaining > 2U) window |= uint32_t(face.kerning[byte_offset + 2U]) << 16U;
        const uint32_t value = (window >> shift) & kValueMask;
        const uint32_t glyph = value >> 4U;
        if (glyph == current) return int(value & 0x0fU) - 8;
        if (glyph < current) begin = index + 1U;
        else end = index;
    }
    return 0;
}

} // namespace mb::fonts
